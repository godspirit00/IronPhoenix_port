/* Win32 implementations of the QEMU primitives used by the NV2A code. */
#include "nv2a_compat.h"
#include <windows.h>
#include <direct.h>
#include <io.h>

Error *error_fatal;
/* Shader disk cache key: bump the suffix whenever generated GLSL changes, or stale binaries load. */
const char xemu_version[] = "ironphoenix-glsl2";
const size_t xemu_version_len = sizeof(xemu_version) - 1;
struct nv2a_config g_config = {
    .display = {.renderer = CONFIG_DISPLAY_RENDERER_OPENGL, .quality = {.surface_scale = 1}},
    .perf = {.cache_shaders = true},
    .audio = {.use_dsp = true, .use_dsp_jit = false, .hrtf = true, .volume_limit = 1.0f, .vp = {.num_workers = 0}},
};
AddressSpace address_space_memory;

void qemu_mutex_init(QemuMutex *m) { InitializeSRWLock((SRWLOCK *)&m->srw); }
void qemu_mutex_destroy(QemuMutex *m) {}
void qemu_mutex_lock(QemuMutex *m) { AcquireSRWLockExclusive((SRWLOCK *)&m->srw); }
void qemu_mutex_unlock(QemuMutex *m) { ReleaseSRWLockExclusive((SRWLOCK *)&m->srw); }
bool qemu_mutex_trylock(QemuMutex *m) { return TryAcquireSRWLockExclusive((SRWLOCK *)&m->srw); }

void qemu_cond_init(QemuCond *c) { InitializeConditionVariable((CONDITION_VARIABLE *)&c->cv); }
void qemu_cond_destroy(QemuCond *c) {}
void qemu_cond_signal(QemuCond *c) { WakeConditionVariable((CONDITION_VARIABLE *)&c->cv); }
void qemu_cond_broadcast(QemuCond *c) { WakeAllConditionVariable((CONDITION_VARIABLE *)&c->cv); }
void qemu_cond_wait(QemuCond *c, QemuMutex *m) {
    SleepConditionVariableSRW((CONDITION_VARIABLE *)&c->cv, (SRWLOCK *)&m->srw, INFINITE, 0);
}
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms) {
    return SleepConditionVariableSRW((CONDITION_VARIABLE *)&c->cv, (SRWLOCK *)&m->srw, ms, 0);
}

void qemu_event_init(QemuEvent *e, bool init) { e->handle = CreateEventA(NULL, TRUE, init, NULL); }
void qemu_event_destroy(QemuEvent *e) { CloseHandle(e->handle); }
void qemu_event_set(QemuEvent *e) { SetEvent(e->handle); }
void qemu_event_reset(QemuEvent *e) { ResetEvent(e->handle); }
void qemu_event_wait(QemuEvent *e) { WaitForSingleObject(e->handle, INFINITE); }

struct thread_start {
    void *(*fn)(void *);
    void *arg;
};
static DWORD WINAPI thread_trampoline(void *p) {
    struct thread_start s = *(struct thread_start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode) {
    struct thread_start *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    t->handle = CreateThread(NULL, 4 << 20, thread_trampoline, s, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
}
void *qemu_thread_join(QemuThread *t) {
    if (!t->handle) {
        return NULL; /* already joined (the shader cache joins its loader thread repeatedly) */
    }
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    t->handle = NULL;
    return NULL;
}

int64_t qemu_clock_get_ns(QEMUClockType type) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

/* ---- timers: a single service thread polls armed QEMUTimers ---- */
#define MAX_TIMERS 16
static QEMUTimer *g_timers[MAX_TIMERS];
static SRWLOCK g_timer_lock = SRWLOCK_INIT;
static HANDLE g_timer_thread, g_timer_wake;

static DWORD WINAPI timer_service(void *unused) {
    for (;;) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t next = INT64_MAX;
        QEMUTimer *fire[MAX_TIMERS];
        int nfire = 0;
        AcquireSRWLockExclusive(&g_timer_lock);
        for (int i = 0; i < MAX_TIMERS; i++) {
            QEMUTimer *t = g_timers[i];
            if (!t || t->expire_time < 0) continue;
            if (t->expire_time <= now) {
                t->expire_time = -1;
                fire[nfire++] = t;
            } else if (t->expire_time < next) {
                next = t->expire_time;
            }
        }
        ReleaseSRWLockExclusive(&g_timer_lock);
        for (int i = 0; i < nfire; i++) fire[i]->cb(fire[i]->opaque);
        DWORD wait = INFINITE;
        if (next != INT64_MAX) {
            int64_t ms = (next - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)) / 1000000;
            wait = ms < 0 ? 0 : (DWORD)ms;
        }
        WaitForSingleObject(g_timer_wake, wait);
    }
    return 0;
}

void timer_init_full(QEMUTimer *t, QEMUClockType type, int scale, QEMUTimerCB *cb, void *opaque) {
    t->expire_time = -1;
    t->cb = cb;
    t->opaque = opaque;
    t->scale = scale;
    AcquireSRWLockExclusive(&g_timer_lock);
    if (!g_timer_thread) {
        g_timer_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_timer_thread = CreateThread(NULL, 0, timer_service, NULL, 0, NULL);
    }
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!g_timers[i]) {
            g_timers[i] = t;
            break;
        }
    ReleaseSRWLockExclusive(&g_timer_lock);
}
void timer_mod(QEMUTimer *t, int64_t expire) {
    AcquireSRWLockExclusive(&g_timer_lock);
    t->expire_time = expire * t->scale;
    ReleaseSRWLockExclusive(&g_timer_lock);
    SetEvent(g_timer_wake);
}
void timer_del(QEMUTimer *t) {
    AcquireSRWLockExclusive(&g_timer_lock);
    t->expire_time = -1;
    ReleaseSRWLockExclusive(&g_timer_lock);
}
bool timer_pending(QEMUTimer *t) { return t->expire_time >= 0; }

void error_report(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    nv2a_log("%s", buf);
}

int qemu_mkdir(const char *path) { return _mkdir(path); }
int qemu_unlink(const char *path) { return _unlink(path); }
