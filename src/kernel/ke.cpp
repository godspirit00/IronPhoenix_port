// Ke*/Ps*/Ob* - threads, IRQL, dispatcher objects, timers, DPCs, interrupts
#include "kernel.h"
#include <windows.h>
#include <map>
#include <vector>
#include <deque>
#include <mmsystem.h>
#include <stdio.h>

uint32_t g_xthread_tls_slot;
static CRITICAL_SECTION g_dispatch_lock;   // held while IRQL >= DISPATCH_LEVEL
static CRITICAL_SECTION g_obj_lock;        // protects object maps + signal state transitions
static std::map<uint32_t, HANDLE> g_obj_events; // Xbox dispatcher object address -> host event
static std::map<HANDLE, uint32_t> g_handle_objs; // host handle -> Xbox-visible object (ObReferenceObjectByHandle)

extern "C" volatile uint32_t xv_KeTickCount[1];
static LARGE_INTEGER g_boot_qpc, g_qpf;

// --------------------------------------------------------------------------- threads
XThreadBlock *xthread_create_block(uint32_t tls_size) {
    XThreadBlock *b = (XThreadBlock *)VirtualAlloc(nullptr, sizeof(XThreadBlock), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    b->SelfPcr = xaddr(b->kpcr);
    b->Prcb = xaddr(b->kpcr + 0x28);
    b->CurrentThread = xaddr(b->ethread);
    b->Header.Type = X_ThreadObject;
    b->Header.Size = sizeof(b->ethread) / 4;
    if (tls_size) {
        // Xbox TLS layout: the block sits right below NtTib.StackBase (fs:[4]) and XAPI's
        // _tls_index is -(TlsSize/4), so [fs:[4] + idx*4] == [TlsData] == TlsData+4 (set by XapiThreadStartup).
        b->tls_alloc = (uint8_t *)calloc(1, tls_size + 16);
        b->TlsData = xaddr(b->tls_alloc);
        b->TlsArray = b->TlsData + tls_size;
    }
    return b;
}

static CRITICAL_SECTION g_thread_list_lock;
static std::vector<XThreadBlock *> g_thread_list;

// Debug: every Xbox-visible thread with a duplicated host handle
void xthread_list(std::vector<std::pair<HANDLE, DWORD>> &out) {
    EnterCriticalSection(&g_thread_list_lock);
    for (XThreadBlock *b : g_thread_list) out.push_back({(HANDLE)b->host_handle, b->host_tid});
    LeaveCriticalSection(&g_thread_list_lock);
}

void xthread_attach(XThreadBlock *b) {
    b->host_tid = GetCurrentThreadId();
    HANDLE self;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
    EnterCriticalSection(&g_thread_list_lock);
    g_thread_list.push_back(b);
    LeaveCriticalSection(&g_thread_list_lock);
    if (!b->host_handle) b->host_handle = self;
    *(uint32_t *)(b->ethread + ETHREAD_UNIQUE_THREAD_OFF) = b->host_tid;
    TlsSetValue(g_xthread_tls_slot, b);
}

XThreadBlock *xthread_current() {
    XThreadBlock *b = (XThreadBlock *)TlsGetValue(g_xthread_tls_slot);
    if (!b) {
        b = xthread_create_block(0);
        xthread_attach(b);
    }
    return b;
}

typedef void(XSTDCALL *SystemRoutine_t)(uint32_t start, uint32_t ctx);
typedef void(XSTDCALL *StartRoutine_t)(uint32_t ctx);

static DWORD WINAPI xthread_main(void *arg) {
    XThreadBlock *b = (XThreadBlock *)arg;
    xthread_attach(b);
    LOGK("thread start routine=%08x ctx=%08x sys=%08x tls=%08x block=%p", b->start_routine, b->start_context, b->system_routine, b->TlsData, b);
    if (b->system_routine)
        ((SystemRoutine_t)xptr<void>(b->system_routine))(b->start_routine, b->start_context);
    else
        ((StartRoutine_t)xptr<void>(b->start_routine))(b->start_context);
    return 0;
}

KEXPORT uint32_t xk_PsCreateSystemThreadEx(uint32_t *ThreadHandle, uint32_t ThreadExtensionSize, uint32_t KernelStackSize,
                                           uint32_t TlsDataSize, uint32_t *ThreadId, uint32_t StartRoutine,
                                           uint32_t StartContext, uint32_t CreateSuspended, uint32_t DebuggerThread,
                                           uint32_t SystemRoutine) {
    XThreadBlock *b = xthread_create_block(TlsDataSize);
    b->start_routine = StartRoutine;
    b->start_context = StartContext;
    b->system_routine = SystemRoutine;
    DWORD tid;
    uint32_t stack = KernelStackSize < (1u << 20) ? (1u << 20) : KernelStackSize;
    HANDLE h = CreateThread(nullptr, stack, xthread_main, b, ((CreateSuspended & 0xFF) ? CREATE_SUSPENDED : 0) | STACK_SIZE_PARAM_IS_A_RESERVATION, &tid);
    if (!h) return X_STATUS_NO_MEMORY;
    b->host_handle = h;
    *(uint32_t *)(b->ethread + ETHREAD_UNIQUE_THREAD_OFF) = tid;
    EnterCriticalSection(&g_obj_lock);
    g_obj_events[xaddr(b->ethread)] = h;
    g_handle_objs[h] = xaddr(b->ethread);
    LeaveCriticalSection(&g_obj_lock);
    if (ThreadHandle) *ThreadHandle = xaddr(h);
    if (ThreadId) *ThreadId = tid;
    logf("PsCreateSystemThreadEx: tid=%lu start=%08x ctx=%08x sys=%08x tls=%u stack=%x", tid, StartRoutine,
         StartContext, SystemRoutine, TlsDataSize, KernelStackSize);
    return X_STATUS_SUCCESS;
}

KEXPORT void xk_PsTerminateSystemThread(uint32_t ExitStatus) {
    LOGK("PsTerminateSystemThread(%08x)", ExitStatus);
    XThreadBlock *b = xthread_current();
    if (b->Irql >= 2) ke_lower_irql(0);
    ExitThread(ExitStatus);
}

// --------------------------------------------------------------------------- IRQL
// Two host locks model a single Xbox CPU:
//  - g_dispatch_lock: held at IRQL >= DISPATCH_LEVEL (2) - DPCs, scheduler-level critical code.
//  - g_int_lock: held at device IRQL (> 2) - interrupt service routines, KeSynchronizeExecution.
// An ISR only takes g_int_lock, so it can run while another thread is at DISPATCH_LEVEL, as a real
// interrupt preempts DISPATCH_LEVEL code. (With one lock for both, DSound code spinning at
// DISPATCH_LEVEL for a voice-off acknowledged by its ISR froze everything for its 0.5 s timeout.)
static const uint32_t XBOX_CODE_BEGIN = 0x11000, XBOX_CODE_END = 0x4B0000; // reserved .axbe range
static CRITICAL_SECTION g_int_lock;
// Current holder of g_dispatch_lock, so an interrupt can freeze it while the ISR runs.
static volatile DWORD g_dispatch_owner_tid;
static HANDLE volatile g_dispatch_owner_handle;
static thread_local HANDLE t_self_handle;
static thread_local uint32_t t_hold_site;
static thread_local LARGE_INTEGER t_hold_start;
static bool g_hold_log = getenv("XB_IRQL_LOG") != nullptr;

uint8_t ke_raise_irql(uint8_t irql) {
    XThreadBlock *b = xthread_current();
    uint8_t old = b->Irql;
    if (old < 2 && irql >= 2) {
        EnterCriticalSection(&g_dispatch_lock);
        if (!t_self_handle)
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t_self_handle,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        g_dispatch_owner_handle = t_self_handle;
        g_dispatch_owner_tid = GetCurrentThreadId();
        t_hold_site = (uint32_t)(uintptr_t)__builtin_return_address(0);
        QueryPerformanceCounter(&t_hold_start);
    }
    if (old <= 2 && irql > 2) EnterCriticalSection(&g_int_lock);
    b->Irql = irql;
    return old;
}

void ke_lower_irql(uint8_t irql) {
    XThreadBlock *b = xthread_current();
    uint8_t old = b->Irql;
    b->Irql = irql;
    if (old > 2 && irql <= 2) LeaveCriticalSection(&g_int_lock);
    if (old >= 2 && irql < 2) {
        if (g_hold_log) {
            LARGE_INTEGER now, f;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&f);
            double ms = (now.QuadPart - t_hold_start.QuadPart) * 1000.0 / f.QuadPart;
            static volatile long n;
            if (ms > 2.0 && InterlockedIncrement(&n) < 300)
                logf("irql: DISPATCH held %.1f ms, raised from host %08x, lowered from host %08x", ms, t_hold_site,
                     (uint32_t)(uintptr_t)__builtin_return_address(0));
        }
        g_dispatch_owner_tid = 0;
        LeaveCriticalSection(&g_dispatch_lock);
    }
}

// Uniprocessor semantics for ISRs: a real interrupt preempts DISPATCH_LEVEL code, which stays
// frozen until the ISR returns (DSound relies on this - its ISR and DPC-level code share voice
// tables without locks). Suspend the dispatch-lock holder while it is executing Xbox code; if it
// is inside host code (kernel call, MMIO emulation - may hold host locks the ISR needs), retry.
volatile long g_isr_count, g_isr_frozen, g_isr_gaveup, g_isr_tries;
volatile long long g_isr_freeze_ticks;
static HANDLE freeze_dispatch_owner() {
    DWORD me = GetCurrentThreadId();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    struct Timer {
        LARGE_INTEGER &t0, &t1;
        ~Timer() { QueryPerformanceCounter(&t1); InterlockedAdd64((volatile LONG64 *)&g_isr_freeze_ticks, t1.QuadPart - t0.QuadPart); }
    } timer{t0, t1};
    InterlockedIncrement(&g_isr_count);
    for (int tries = 0; tries < 64; tries++) {
        DWORD tid = g_dispatch_owner_tid;
        HANDLE h = g_dispatch_owner_handle;
        if (!tid || tid == me || !h) return nullptr;
        if (SuspendThread(h) == (DWORD)-1) return nullptr;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL;
        bool ok = GetThreadContext(h, &c) != 0; // also waits until the suspension took effect
        if (g_dispatch_owner_tid != tid) { // released the lock meanwhile
            ResumeThread(h);
            continue;
        }
        if (ok && c.Eip >= XBOX_CODE_BEGIN && c.Eip < XBOX_CODE_END) {
            InterlockedIncrement(&g_isr_frozen);
            return h;
        }
        InterlockedIncrement(&g_isr_tries);
        ResumeThread(h);
        if (tries > 8) SwitchToThread();
    }
    InterlockedIncrement(&g_isr_gaveup);
    return nullptr; // give up: run the ISR concurrently
}

extern "C" void ke_isr_stats(char *buf, int n) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    snprintf(buf, n, "isr %ld frozen %ld retries %ld gaveup %ld freeze-time %.1f ms", g_isr_count, g_isr_frozen, g_isr_tries, g_isr_gaveup,
             g_isr_freeze_ticks * 1000.0 / f.QuadPart);
    g_isr_count = g_isr_frozen = g_isr_tries = g_isr_gaveup = 0;
    g_isr_freeze_ticks = 0;
}

// debug: who holds DISPATCH_LEVEL for long (XB_IRQL_LOG=1)
static thread_local uint32_t t_raise_site;
static thread_local LARGE_INTEGER t_raise_time;
static bool g_irql_log = getenv("XB_IRQL_LOG") != nullptr;
static void irql_note_raise(uint32_t old, uint32_t site) {
    if (g_irql_log && old < 2) {
        t_raise_site = site;
        QueryPerformanceCounter(&t_raise_time);
    }
}
static void irql_note_lower(uint32_t newirql, uint32_t site) {
    if (!g_irql_log || newirql >= 2 || xthread_current()->Irql < 2) return;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    double ms = (now.QuadPart - t_raise_time.QuadPart) * 1000.0 / f.QuadPart;
    static volatile long n;
    if (ms > 2.0 && InterlockedIncrement(&n) < 300) logf("irql: DISPATCH held %.1f ms raised at %08x lowered at %08x", ms, t_raise_site, site);
}
KEXPORT_FAST uint32_t xk_KfRaiseIrql(uint32_t NewIrql) {
    uint8_t old = ke_raise_irql((uint8_t)NewIrql);
    irql_note_raise(old, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return old;
}
KEXPORT_FAST void xk_KfLowerIrql(uint32_t NewIrql) {
    irql_note_lower(NewIrql, (uint32_t)(uintptr_t)__builtin_return_address(0));
    ke_lower_irql((uint8_t)NewIrql);
}
KEXPORT uint32_t xk_KeRaiseIrqlToDpcLevel() {
    uint8_t old = ke_raise_irql(2);
    irql_note_raise(old, (uint32_t)(uintptr_t)__builtin_return_address(0));
    return old;
}

// --------------------------------------------------------------------------- time
uint32_t timeout_to_ms(const int64_t *timeout) {
    if (!timeout) return INFINITE;
    int64_t t = *timeout;
    if (t < 0) {
        int64_t ms = (-t + 9999) / 10000;
        return ms > 0x7FFFFFFF ? INFINITE : (uint32_t)ms;
    }
    if (t == 0) return 0;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    int64_t now = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return t <= now ? 0 : (uint32_t)((t - now + 9999) / 10000);
}

KEXPORT void xk_KeQuerySystemTime(int64_t *CurrentTime) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    *CurrentTime = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

extern uint64_t xbox_tsc();
KEXPORT uint64_t xk_KeQueryPerformanceCounter() {
    // Xbox ACPI timer runs at 3.375 MHz
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = now.QuadPart - g_boot_qpc.QuadPart;
    return d / g_qpf.QuadPart * 3375000ull + d % g_qpf.QuadPart * 3375000ull / g_qpf.QuadPart;
}
KEXPORT uint64_t xk_KeQueryPerformanceFrequency() { return 3375000ull; }

static void precise_sleep_100ns(int64_t t) {
    if (t <= 0) {
        SwitchToThread();
        return;
    }
    // Use a high-resolution waitable timer for sub-ms sleeps
    static thread_local HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -t;
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, INFINITE);
    } else {
        Sleep((DWORD)((t + 9999) / 10000));
    }
}

KEXPORT uint32_t xk_KeDelayExecutionThread(uint32_t WaitMode, uint32_t Alertable, int64_t *Interval) {
    int64_t t = *Interval;
    if (t > 0) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        t = t - (((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    } else {
        t = -t;
    }
    if ((Alertable & 0xFF) && t >= 10000) {
        DWORD r = SleepEx((DWORD)(t / 10000), TRUE);
        return r == WAIT_IO_COMPLETION ? X_STATUS_USER_APC : X_STATUS_SUCCESS;
    }
    precise_sleep_100ns(t);
    return X_STATUS_SUCCESS;
}

KEXPORT void xk_KeStallExecutionProcessor(uint32_t MicroSeconds) {
    LARGE_INTEGER s, n;
    QueryPerformanceCounter(&s);
    int64_t ticks = (int64_t)MicroSeconds * g_qpf.QuadPart / 1000000;
    do {
        YieldProcessor();
        QueryPerformanceCounter(&n);
    } while (n.QuadPart - s.QuadPart < ticks);
}

// --------------------------------------------------------------------------- dispatcher objects
static bool is_event_type(uint8_t t) { return t == X_NotificationEvent || t == X_SynchronizationEvent; }
static bool is_auto_reset(uint8_t t) { return t == X_SynchronizationEvent || t == X_SynchronizationTimerObject; }

// Caller holds g_obj_lock.
static HANDLE obj_host_locked(uint32_t object) {
    auto it = g_obj_events.find(object);
    if (it != g_obj_events.end()) return it->second;
    X_DISPATCHER_HEADER *h = xptr<X_DISPATCHER_HEADER>(object);
    HANDLE e = CreateEventA(nullptr, !is_auto_reset(h->Type), h->SignalState > 0, nullptr);
    g_obj_events[object] = e;
    return e;
}

void *ke_object_host_event(uint32_t object) {
    EnterCriticalSection(&g_obj_lock);
    HANDLE h = obj_host_locked(object);
    LeaveCriticalSection(&g_obj_lock);
    return h;
}

void ke_signal_object(uint32_t object) {
    X_DISPATCHER_HEADER *h = xptr<X_DISPATCHER_HEADER>(object);
    EnterCriticalSection(&g_obj_lock);
    h->SignalState = 1;
    SetEvent(obj_host_locked(object));
    LeaveCriticalSection(&g_obj_lock);
}

// Sync host state with SignalState written directly by Xbox code (e.g. inlined KeClearEvent).
static HANDLE obj_prepare_wait(uint32_t object) {
    X_DISPATCHER_HEADER *h = xptr<X_DISPATCHER_HEADER>(object);
    EnterCriticalSection(&g_obj_lock);
    HANDLE e = obj_host_locked(object);
    if (h->Type != X_ThreadObject) {
        if (h->SignalState <= 0) ResetEvent(e);
        else SetEvent(e);
    }
    LeaveCriticalSection(&g_obj_lock);
    return e;
}

static void obj_after_wait(uint32_t object) {
    X_DISPATCHER_HEADER *h = xptr<X_DISPATCHER_HEADER>(object);
    if (is_auto_reset(h->Type)) h->SignalState = 0;
}

uint32_t ke_wait_objects(uint32_t count, const uint32_t *objects, bool wait_all, const int64_t *timeout, bool alertable) {
    HANDLE hs[64];
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; i++) hs[i] = obj_prepare_wait(objects[i]);
    DWORD ms = timeout_to_ms(timeout);
    DWORD r = WaitForMultipleObjectsEx(count, hs, wait_all, ms, alertable);
    if (r == WAIT_TIMEOUT) return X_STATUS_TIMEOUT;
    if (r == WAIT_IO_COMPLETION) return X_STATUS_USER_APC;
    if (r < WAIT_OBJECT_0 + count) {
        if (wait_all) {
            for (uint32_t i = 0; i < count; i++) obj_after_wait(objects[i]);
        } else {
            obj_after_wait(objects[r - WAIT_OBJECT_0]);
        }
        return r - WAIT_OBJECT_0;
    }
    logf("ke_wait_objects: wait failed r=%lx err=%lu", r, GetLastError());
    return X_STATUS_UNSUCCESSFUL;
}

KEXPORT uint32_t xk_KeSetEvent(uint32_t Event, uint32_t Increment, uint32_t Wait) {
    X_DISPATCHER_HEADER *h = xptr<X_DISPATCHER_HEADER>(Event);
    EnterCriticalSection(&g_obj_lock);
    int32_t prev = h->SignalState;
    h->SignalState = 1;
    SetEvent(obj_host_locked(Event));
    LeaveCriticalSection(&g_obj_lock);
    return prev;
}

KEXPORT uint32_t xk_KeWaitForSingleObject(uint32_t Object, uint32_t WaitReason, uint32_t WaitMode, uint32_t Alertable, int64_t *Timeout) {
    return ke_wait_objects(1, &Object, false, Timeout, Alertable & 0xFF);
}

KEXPORT uint32_t xk_KeWaitForMultipleObjects(uint32_t Count, uint32_t *Object, uint32_t WaitType, uint32_t WaitReason,
                                             uint32_t WaitMode, uint32_t Alertable, int64_t *Timeout, uint32_t WaitBlockArray) {
    return ke_wait_objects(Count, Object, WaitType == 0 /*WaitAll*/, Timeout, Alertable & 0xFF);
}

// Nt* handle based objects map 1:1 onto host handles.
KEXPORT uint32_t xk_NtCreateEvent(uint32_t *EventHandle, X_OBJECT_ATTRIBUTES *ObjectAttributes, uint32_t EventType, uint32_t InitialState) {
    char name[260] = {0};
    if (ObjectAttributes && ObjectAttributes->ObjectName) {
        X_ANSI_STRING *s = xptr<X_ANSI_STRING>(ObjectAttributes->ObjectName);
        snprintf(name, sizeof name, "Local\\xbox_%.*s", s->Length, xptr<char>(s->Buffer));
        for (char *p = name + 6; *p; p++) if (*p == '\\') *p = '_';
    }
    HANDLE h = CreateEventA(nullptr, EventType == X_NotificationEvent, InitialState & 0xFF, name[0] ? name : nullptr);
    if (!h) return X_STATUS_UNSUCCESSFUL;
    *EventHandle = xaddr(h);
    LOGK("NtCreateEvent(%s type=%u init=%u) -> %p", name, EventType, InitialState & 0xFF, h);
    return (name[0] && GetLastError() == ERROR_ALREADY_EXISTS) ? X_STATUS_OBJECT_NAME_EXISTS : X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtSetEvent(uint32_t EventHandle, int32_t *PreviousState) {
    HANDLE h = xptr<void>(EventHandle);
    if (PreviousState) *PreviousState = WaitForSingleObject(h, 0) == WAIT_OBJECT_0; // approximate
    // If the handle was exposed as a kernel object, keep its header in sync
    EnterCriticalSection(&g_obj_lock);
    auto it = g_handle_objs.find(h);
    if (it != g_handle_objs.end()) xptr<X_DISPATCHER_HEADER>(it->second)->SignalState = 1;
    LeaveCriticalSection(&g_obj_lock);
    return SetEvent(h) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

void *io_wait_object(uint32_t h);
static uint32_t nt_wait(uint32_t Handle, uint32_t Alertable, int64_t *Timeout) {
    HANDLE w = io_wait_object(Handle); // asynchronous file: signaled when its reads have completed
    if (!w) w = xptr<void>(Handle);
    DWORD r = WaitForSingleObjectEx(w, timeout_to_ms(Timeout), Alertable & 0xFF);
    if (r == WAIT_FAILED) {
        logf("NtWaitForSingleObject(%08x) failed: %lu", Handle, GetLastError());
        return X_STATUS_INVALID_HANDLE;
    }
    return r; // WAIT_OBJECT_0 / WAIT_TIMEOUT / WAIT_IO_COMPLETION share NTSTATUS values
}
KEXPORT uint32_t xk_NtWaitForSingleObject(uint32_t Handle, uint32_t Alertable, int64_t *Timeout) { return nt_wait(Handle, Alertable, Timeout); }
KEXPORT uint32_t xk_NtWaitForSingleObjectEx(uint32_t Handle, uint32_t WaitMode, uint32_t Alertable, int64_t *Timeout) { return nt_wait(Handle, Alertable, Timeout); }

extern bool io_close_handle(uint32_t h);
KEXPORT uint32_t xk_NtClose(uint32_t Handle) {
    if (io_close_handle(Handle)) return X_STATUS_SUCCESS;
    EnterCriticalSection(&g_obj_lock);
    g_handle_objs.erase(xptr<void>(Handle));
    LeaveCriticalSection(&g_obj_lock);
    return CloseHandle(xptr<void>(Handle)) ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

KEXPORT uint32_t xk_NtDuplicateObject(uint32_t SourceHandle, uint32_t *TargetHandle, uint32_t Options) {
    HANDLE out;
    if (!DuplicateHandle(GetCurrentProcess(), xptr<void>(SourceHandle), GetCurrentProcess(), &out, 0, FALSE,
                         DUPLICATE_SAME_ACCESS | ((Options & 1) ? DUPLICATE_CLOSE_SOURCE : 0)))
        return X_STATUS_INVALID_HANDLE;
    *TargetHandle = xaddr(out);
    return X_STATUS_SUCCESS;
}

// ObReferenceObjectByHandle: give Xbox code a dispatcher object backed by the host handle.
KEXPORT uint32_t xk_ObReferenceObjectByHandle(uint32_t Handle, uint32_t ObjectType, uint32_t *ReturnedObject) {
    HANDLE h = xptr<void>(Handle);
    if (Handle == 0xFFFFFFFE) { // NtCurrentThread()
        *ReturnedObject = xaddr(xthread_current()->ethread);
        return X_STATUS_SUCCESS;
    }
    EnterCriticalSection(&g_obj_lock);
    auto it = g_handle_objs.find(h);
    uint32_t obj;
    if (it != g_handle_objs.end()) {
        obj = it->second;
    } else {
        // Event handle: expose a KEVENT-shaped shadow whose host event is the handle itself
        X_DISPATCHER_HEADER *hdr = (X_DISPATCHER_HEADER *)calloc(1, 64);
        hdr->Type = X_NotificationEvent;
        hdr->Size = 4;
        hdr->SignalState = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        obj = xaddr(hdr);
        g_handle_objs[h] = obj;
        g_obj_events[obj] = h;
    }
    LeaveCriticalSection(&g_obj_lock);
    *ReturnedObject = obj;
    LOGK("ObReferenceObjectByHandle(%08x) -> %08x", Handle, obj);
    return X_STATUS_SUCCESS;
}

KEXPORT_FAST void xk_ObfDereferenceObject(uint32_t Object) {}

KEXPORT uint32_t xk_ObReferenceObjectByName(uint32_t ObjectName, uint32_t Attributes, uint32_t ObjectType, uint32_t ParseContext, uint32_t *Object) {
    X_ANSI_STRING *s = xptr<X_ANSI_STRING>(ObjectName);
    logf("ObReferenceObjectByName(%.*s) -> not found", s->Length, xptr<char>(s->Buffer));
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
}

// --------------------------------------------------------------------------- DPCs
static std::deque<uint32_t> g_dpc_queue;
static CRITICAL_SECTION g_dpc_qlock;
static HANDLE g_dpc_event;

KEXPORT void xk_KeInitializeDpc(X_KDPC *Dpc, uint32_t DeferredRoutine, uint32_t DeferredContext) {
    Dpc->Type = 19; // DpcObject
    Dpc->Inserted = 0;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

KEXPORT uint32_t xk_KeInsertQueueDpc(X_KDPC *Dpc, uint32_t SystemArgument1, uint32_t SystemArgument2) {
    EnterCriticalSection(&g_dpc_qlock);
    if (Dpc->Inserted) {
        LeaveCriticalSection(&g_dpc_qlock);
        return 0;
    }
    Dpc->Inserted = 1;
    Dpc->SystemArgument1 = SystemArgument1;
    Dpc->SystemArgument2 = SystemArgument2;
    g_dpc_queue.push_back(xaddr(Dpc));
    LeaveCriticalSection(&g_dpc_qlock);
    SetEvent(g_dpc_event);
    return 1;
}

KEXPORT uint32_t xk_KeRemoveQueueDpc(X_KDPC *Dpc) {
    EnterCriticalSection(&g_dpc_qlock);
    bool was = Dpc->Inserted;
    if (was) {
        for (auto it = g_dpc_queue.begin(); it != g_dpc_queue.end(); ++it)
            if (*it == xaddr(Dpc)) {
                g_dpc_queue.erase(it);
                break;
            }
        Dpc->Inserted = 0;
    }
    LeaveCriticalSection(&g_dpc_qlock);
    return was;
}

typedef void(XSTDCALL *DpcRoutine_t)(uint32_t dpc, uint32_t ctx, uint32_t a1, uint32_t a2);

volatile LONG64 g_dpc_running_since; // QPC when the current DPC started, 0 when idle (debug sampler)
volatile uint32_t g_dpc_routine;
HANDLE g_dpc_thread_handle;
static DWORD WINAPI dpc_thread(void *) {
    xthread_current();
    logf("dpc thread: tid %lu", GetCurrentThreadId());
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_dpc_thread_handle,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        WaitForSingleObject(g_dpc_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_dpc_qlock);
            if (g_dpc_queue.empty()) {
                LeaveCriticalSection(&g_dpc_qlock);
                break;
            }
            X_KDPC *d = xptr<X_KDPC>(g_dpc_queue.front());
            g_dpc_queue.pop_front();
            d->Inserted = 0;
            uint32_t r = d->DeferredRoutine, c = d->DeferredContext, a1 = d->SystemArgument1, a2 = d->SystemArgument2;
            LeaveCriticalSection(&g_dpc_qlock);
            uint8_t old = ke_raise_irql(2);
            LARGE_INTEGER d0, d1, f;
            QueryPerformanceCounter(&d0);
            g_dpc_routine = r;
            g_dpc_running_since = d0.QuadPart;
            ((DpcRoutine_t)xptr<void>(r))(xaddr(d), c, a1, a2);
            g_dpc_running_since = 0;
            QueryPerformanceCounter(&d1);
            ke_lower_irql(old);
            if (g_hold_log) {
                QueryPerformanceFrequency(&f);
                double ms = (d1.QuadPart - d0.QuadPart) * 1000.0 / f.QuadPart;
                static volatile long n;
                if (ms > 2.0 && InterlockedIncrement(&n) < 200) logf("dpc: routine %08x ctx %08x took %.1f ms", r, c, ms);
            }
        }
    }
}

// Run queued DPCs on the calling thread, which already holds the dispatch lock (IRQL 2).
static void run_queued_dpcs_locked() {
    for (;;) {
        EnterCriticalSection(&g_dpc_qlock);
        if (g_dpc_queue.empty()) {
            LeaveCriticalSection(&g_dpc_qlock);
            return;
        }
        X_KDPC *d = xptr<X_KDPC>(g_dpc_queue.front());
        g_dpc_queue.pop_front();
        d->Inserted = 0;
        uint32_t r = d->DeferredRoutine, c = d->DeferredContext, a1 = d->SystemArgument1, a2 = d->SystemArgument2;
        LeaveCriticalSection(&g_dpc_qlock);
        ((DpcRoutine_t)xptr<void>(r))(xaddr(d), c, a1, a2);
    }
}

// --------------------------------------------------------------------------- timers
struct TimerEntry {
    uint32_t timer;
    int64_t due_qpc;
};
static std::vector<TimerEntry> g_timers;
static CRITICAL_SECTION g_timer_lock;
static HANDLE g_timer_wake;

KEXPORT void xk_KeInitializeTimerEx(X_KTIMER *Timer, uint32_t Type) {
    Timer->Header.Type = (uint8_t)(X_NotificationTimerObject + Type);
    Timer->Header.Inserted = 0;
    Timer->Header.Size = sizeof(X_KTIMER) / 4;
    Timer->Header.SignalState = 0;
    Timer->DueTime = 0;
    Timer->Period = 0;
}

static int64_t qpc_now() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

static bool timer_remove_locked(uint32_t t) {
    for (size_t i = 0; i < g_timers.size(); i++)
        if (g_timers[i].timer == t) {
            g_timers.erase(g_timers.begin() + i);
            return true;
        }
    return false;
}

static uint32_t set_timer(X_KTIMER *Timer, int64_t DueTime, int32_t Period, uint32_t Dpc) {
    EnterCriticalSection(&g_timer_lock);
    bool was = timer_remove_locked(xaddr(Timer));
    Timer->Header.Inserted = 1;
    Timer->Header.SignalState = 0;
    Timer->Dpc = Dpc;
    Timer->Period = Period;
    int64_t rel100;
    if (DueTime < 0) {
        rel100 = -DueTime;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        rel100 = DueTime - (((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        if (rel100 < 0) rel100 = 0;
    }
    g_timers.push_back({xaddr(Timer), qpc_now() + rel100 * g_qpf.QuadPart / 10000000});
    // host event mirrors the signal state
    EnterCriticalSection(&g_obj_lock);
    ResetEvent(obj_host_locked(xaddr(Timer)));
    LeaveCriticalSection(&g_obj_lock);
    LeaveCriticalSection(&g_timer_lock);
    SetEvent(g_timer_wake);
    return was;
}

KEXPORT uint32_t xk_KeSetTimer(X_KTIMER *Timer, uint32_t DueLo, uint32_t DueHi, uint32_t Dpc) {
    return set_timer(Timer, (int64_t)(((uint64_t)DueHi << 32) | DueLo), 0, Dpc);
}
KEXPORT uint32_t xk_KeSetTimerEx(X_KTIMER *Timer, uint32_t DueLo, uint32_t DueHi, int32_t Period, uint32_t Dpc) {
    return set_timer(Timer, (int64_t)(((uint64_t)DueHi << 32) | DueLo), Period, Dpc);
}
KEXPORT uint32_t xk_KeCancelTimer(X_KTIMER *Timer) {
    EnterCriticalSection(&g_timer_lock);
    bool was = timer_remove_locked(xaddr(Timer));
    Timer->Header.Inserted = 0;
    LeaveCriticalSection(&g_timer_lock);
    return was;
}

static DWORD WINAPI timer_thread(void *) {
    xthread_current();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        int64_t now = qpc_now();
        int64_t next = INT64_MAX;
        std::vector<uint32_t> fired;
        EnterCriticalSection(&g_timer_lock);
        for (size_t i = 0; i < g_timers.size();) {
            if (g_timers[i].due_qpc <= now) {
                X_KTIMER *t = xptr<X_KTIMER>(g_timers[i].timer);
                fired.push_back(g_timers[i].timer);
                if (t->Period > 0) {
                    g_timers[i].due_qpc += (int64_t)t->Period * g_qpf.QuadPart / 1000;
                    if (g_timers[i].due_qpc < now) g_timers[i].due_qpc = now;
                } else {
                    t->Header.Inserted = 0;
                    g_timers.erase(g_timers.begin() + i);
                    continue;
                }
            }
            if (g_timers[i].due_qpc < next) next = g_timers[i].due_qpc;
            i++;
        }
        LeaveCriticalSection(&g_timer_lock);
        for (uint32_t t : fired) {
            X_KTIMER *kt = xptr<X_KTIMER>(t);
            ke_signal_object(t);
            if (kt->Dpc) {
                X_KDPC *d = xptr<X_KDPC>(kt->Dpc);
                LARGE_INTEGER st;
                GetSystemTimeAsFileTime((FILETIME *)&st);
                xk_KeInsertQueueDpc(d, st.LowPart, st.HighPart);
            }
        }
        // Keep KeTickCount (milliseconds since boot) fresh
        xv_KeTickCount[0] = (uint32_t)((qpc_now() - g_boot_qpc.QuadPart) * 1000 / g_qpf.QuadPart);
        DWORD wait = 1;
        if (next != INT64_MAX) {
            int64_t ms = (next - qpc_now()) * 1000 / g_qpf.QuadPart;
            wait = ms <= 0 ? 0 : (ms > 1 ? 1 : (DWORD)ms);
        }
        WaitForSingleObject(g_timer_wake, wait);
    }
}

// --------------------------------------------------------------------------- interrupts
static X_KINTERRUPT *g_interrupts[32];

KEXPORT uint32_t xk_HalGetInterruptVector(uint32_t BusInterruptLevel, uint8_t *Irql) {
    if (Irql) *Irql = (uint8_t)(26 - BusInterruptLevel);
    return BusInterruptLevel + 0x30;
}

KEXPORT void xk_KeInitializeInterrupt(X_KINTERRUPT *Interrupt, uint32_t ServiceRoutine, uint32_t ServiceContext,
                                      uint32_t Vector, uint32_t Irql, uint32_t InterruptMode, uint32_t ShareVector) {
    memset(Interrupt, 0, sizeof *Interrupt);
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - 0x30;
    Interrupt->Irql = Irql & 0xFF;
    Interrupt->Mode = (uint8_t)InterruptMode;
    Interrupt->ShareVector = (uint8_t)ShareVector;
}

KEXPORT uint32_t xk_KeConnectInterrupt(X_KINTERRUPT *Interrupt) {
    uint32_t v = Interrupt->BusInterruptLevel & 31;
    logf("KeConnectInterrupt: irq %u isr=%08x ctx=%08x", v, Interrupt->ServiceRoutine, Interrupt->ServiceContext);
    if (Interrupt->Connected) return 0;
    Interrupt->Connected = 1;
    g_interrupts[v] = Interrupt;
    return 1;
}

KEXPORT uint32_t xk_KeDisconnectInterrupt(X_KINTERRUPT *Interrupt) {
    uint32_t v = Interrupt->BusInterruptLevel & 31;
    if (!Interrupt->Connected) return 0;
    Interrupt->Connected = 0;
    if (g_interrupts[v] == Interrupt) g_interrupts[v] = nullptr;
    return 1;
}

typedef uint8_t(XSTDCALL *Isr_t)(uint32_t interrupt, uint32_t ctx);
typedef uint8_t(XSTDCALL *SyncRoutine_t)(uint32_t ctx);
extern "C" bool ke_fire_interrupt_c(uint32_t irq);

// Deliver an emulated hardware interrupt (called by HLE devices).
// inline_dpcs: run DPCs queued by the ISR right away on this thread. Only for the GPU interrupt:
// a DSound DPC may spin waiting for the next *APU* interrupt (idle-voice trap), which this very
// thread would have to deliver - inline DPCs on the APU thread deadlocked for DSound's 0.5 s timeout.
static bool ke_fire_interrupt_ex(uint32_t irq, bool inline_dpcs);
bool ke_fire_interrupt(uint32_t irq) { return ke_fire_interrupt_ex(irq, false); }
static bool ke_fire_interrupt_ex(uint32_t irq, bool inline_dpcs) {
    X_KINTERRUPT *i = g_interrupts[irq & 31];
    if (!i) return false;
    // Normally an ISR is serialized with DISPATCH_LEVEL code (takes the dispatch lock). If the
    // lock stays held for over 1 ms the holder is most likely spinning on something the ISR must
    // do (DSound voice-off wait), so preempt it: freeze it in Xbox code and run the ISR anyway.
    XThreadBlock *b = xthread_current();
    uint8_t old = b->Irql;
    bool got_dispatch = false;
    HANDLE frozen = nullptr;
    if (old < 2) {
        LARGE_INTEGER t0, now, f;
        QueryPerformanceCounter(&t0);
        QueryPerformanceFrequency(&f);
        for (;;) {
            if (TryEnterCriticalSection(&g_dispatch_lock)) {
                got_dispatch = true;
                break;
            }
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - t0.QuadPart) * 1000 > f.QuadPart) break;
            SwitchToThread();
        }
    }
    if (old <= 2) EnterCriticalSection(&g_int_lock);
    if (old < 2 && !got_dispatch) frozen = freeze_dispatch_owner();
    b->Irql = (uint8_t)i->Irql;
    ((Isr_t)xptr<void>(i->ServiceRoutine))(xaddr(i), i->ServiceContext);
    b->Irql = old;
    if (frozen) ResumeThread(frozen);
    if (old <= 2) LeaveCriticalSection(&g_int_lock);
    if (got_dispatch && !inline_dpcs) {
        LeaveCriticalSection(&g_dispatch_lock);
        SetEvent(g_dpc_event);
    } else if (got_dispatch) {
        // like a real CPU dropping below DISPATCH_LEVEL after the ISR: run queued DPCs right
        // here instead of waking the DPC thread (saves a thread hop per GPU notify interrupt)
        b->Irql = 2;
        if (!t_self_handle)
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t_self_handle,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
        g_dispatch_owner_handle = t_self_handle;
        g_dispatch_owner_tid = GetCurrentThreadId();
        run_queued_dpcs_locked();
        g_dispatch_owner_tid = 0;
        b->Irql = old;
        LeaveCriticalSection(&g_dispatch_lock);
    }
    return true;
}

extern "C" bool ke_fire_interrupt_c(uint32_t irq) { return ke_fire_interrupt_ex(irq, true); } // NV2A

KEXPORT uint32_t xk_KeSynchronizeExecution(X_KINTERRUPT *Interrupt, uint32_t SynchronizeRoutine, uint32_t SynchronizeContext) {
    uint8_t old = ke_raise_irql((uint8_t)Interrupt->Irql);
    uint8_t r = ((SyncRoutine_t)xptr<void>(SynchronizeRoutine))(SynchronizeContext);
    ke_lower_irql(old);
    return r;
}

// --------------------------------------------------------------------------- misc
KEXPORT uint32_t xk_KeSaveFloatingPointState(uint32_t p) { return X_STATUS_SUCCESS; }
KEXPORT uint32_t xk_KeRestoreFloatingPointState(uint32_t p) { return X_STATUS_SUCCESS; }
KEXPORT void xk_KeBugCheck(uint32_t code) { fatal("KeBugCheck(%08x)", code); }

void ke_init() {
    g_xthread_tls_slot = TlsAlloc();
    InitializeCriticalSection(&g_thread_list_lock);
    InitializeCriticalSection(&g_dispatch_lock);
    InitializeCriticalSection(&g_int_lock);
    InitializeCriticalSection(&g_obj_lock);
    InitializeCriticalSection(&g_dpc_qlock);
    InitializeCriticalSection(&g_timer_lock);
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_boot_qpc);
    timeBeginPeriod(1);
    g_dpc_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_timer_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, dpc_thread, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, timer_thread, nullptr, 0, nullptr));
}
