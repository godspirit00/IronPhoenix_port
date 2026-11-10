// Debug helpers: dump the state of all Xbox threads (EIP + EBP call chain).
#include "common.h"
#include "kernel/kernel.h"
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <stdio.h>
#include <stdlib.h>

extern void xthread_list(std::vector<std::pair<HANDLE, DWORD>> &out);

static bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

// --- symbolizer: function names from the exe's own COFF symbol table (kept by the -g link) ---
#include <algorithm>
#include <string>
static const uint32_t XBOX_CODE_BEGIN = 0x11000, XBOX_CODE_END = 0x4B0000;
struct Sym {
    uint32_t addr;
    bool func;
    std::string name;
};
static std::vector<Sym> g_syms;
static void load_symbols() {
    static bool done;
    if (done) return;
    done = true;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (d.size() < 0x400) return;
    uint32_t pe = *(uint32_t *)&d[0x3C];
    if (pe + 0x200 > d.size()) return;
    IMAGE_FILE_HEADER *fh = (IMAGE_FILE_HEADER *)&d[pe + 4];
    IMAGE_OPTIONAL_HEADER32 *oh = (IMAGE_OPTIONAL_HEADER32 *)(fh + 1);
    IMAGE_SECTION_HEADER *sh = (IMAGE_SECTION_HEADER *)((uint8_t *)oh + fh->SizeOfOptionalHeader);
    uint32_t symoff = fh->PointerToSymbolTable, nsym = fh->NumberOfSymbols;
    if (!symoff || symoff + nsym * 18 + 4 > d.size()) return;
    const char *strtab = (const char *)&d[symoff + nsym * 18];
    for (uint32_t i = 0; i < nsym; i++) {
        IMAGE_SYMBOL *sym = (IMAGE_SYMBOL *)&d[symoff + i * 18];
        i += sym->NumberOfAuxSymbols;
        if (sym->SectionNumber <= 0 || sym->SectionNumber > fh->NumberOfSections) continue;
        if (sym->StorageClass != IMAGE_SYM_CLASS_EXTERNAL && sym->StorageClass != IMAGE_SYM_CLASS_STATIC) continue;
        std::string name = sym->N.Name.Short ? std::string((const char *)sym->N.ShortName, strnlen((const char *)sym->N.ShortName, 8))
                                             : std::string(strtab + sym->N.Name.Long);
        if (name.empty() || name[0] == '.') continue; // section symbols
        uint32_t a = oh->ImageBase + sh[sym->SectionNumber - 1].VirtualAddress + sym->Value;
        g_syms.push_back({a, (sym->Type >> 4) == IMAGE_SYM_DTYPE_FUNCTION, name});
    }
    std::sort(g_syms.begin(), g_syms.end(), [](const Sym &a, const Sym &b) { return a.addr < b.addr; });
}
// "func+0x12" for our code, "xbox:0022e9d4" for game code, "module+0x34" for DLLs; nullptr if
// the address is not code (data symbol / unknown)
static const char *symbolize(uint32_t a, char *buf, size_t n) {
    if (a >= XBOX_CODE_BEGIN && a < XBOX_CODE_END) {
        snprintf(buf, n, "xbox:%08x", a);
        return buf;
    }
    load_symbols();
    auto it = std::upper_bound(g_syms.begin(), g_syms.end(), a, [](uint32_t v, const Sym &s) { return v < s.addr; });
    if (it != g_syms.begin() && a < 0x01000000) {
        --it;
        if (!it->func || a - it->addr > 0x10000) return nullptr;
        snprintf(buf, n, "%s+0x%x", it->name.c_str(), a - it->addr);
        return buf;
    }
    HMODULE hm;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &hm) &&
        hm != GetModuleHandleA(nullptr)) {
        char mod[MAX_PATH];
        GetModuleFileNameA(hm, mod, sizeof mod);
        const char *base = strrchr(mod, '\\');
        snprintf(buf, n, "%s+0x%x", base ? base + 1 : mod, a - (uint32_t)(uintptr_t)hm);
        return buf;
    }
    return nullptr;
}

void debug_dump_threads(const char *why) {
    std::vector<std::pair<HANDLE, DWORD>> threads;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) threads.push_back({h, te.th32ThreadID});
    }
    CloseHandle(snap);
    logf("==== thread dump (%s): %zu threads ====", why, threads.size());
    for (auto &t : threads) {
        if (t.second == GetCurrentThreadId()) continue;
        if (SuspendThread(t.first) == (DWORD)-1) {
            logf("  tid %5lu: SuspendThread failed (%lu)", t.second, GetLastError());
            continue;
        }
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(t.first, &c)) {
            logf("  tid %5lu: GetThreadContext failed (%lu)", t.second, GetLastError());
        } else {
            char buf[512];
            int n = 0;
            uint32_t *bp = (uint32_t *)c.Ebp;
            for (int i = 0; i < 16 && readable(bp, 8) && n < (int)sizeof buf - 12; i++) {
                n += snprintf(buf + n, sizeof buf - n, "%08x ", bp[1]);
                if ((uint32_t *)bp[0] <= bp) break;
                bp = (uint32_t *)bp[0];
            }
            uint32_t *sp = (uint32_t *)c.Esp;
            char sbuf[256];
            int m = 0;
            for (int i = 0; i < 8; i++) m += snprintf(sbuf + m, sizeof sbuf - m, "%08x ", readable(sp + i, 4) ? sp[i] : 0);
            logf("  tid %5lu eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx | frames: %s| stack: %s", t.second, c.Eip, c.Esp, c.Ebp,
                 c.Eax, c.Ecx, buf, sbuf);
            // symbolized: eip, then likely return addresses found on the stack (stale frames possible)
            char sym[128], line[1400];
            const char *e = symbolize(c.Eip, sym, sizeof sym);
            int k = snprintf(line, sizeof line, "    at %s |", e ? e : "?");
            uint32_t *sp2 = (uint32_t *)c.Esp;
            int depth = 4096;
            while (depth > 16 && !readable(sp2, depth * 4)) depth /= 2;
            for (int i = 0, found = 0; i < depth && found < 14 && k < (int)sizeof line - 140; i++) {
                const char *r = symbolize(sp2[i], sym, sizeof sym);
                if (r) {
                    k += snprintf(line + k, sizeof line - k, " %s", r);
                    found++;
                }
            }
            logf("%s", line);
        }
        ResumeThread(t.first);
        CloseHandle(t.first);
    }
}

extern "C" int64_t nv2a_us_since_flip(void);
extern "C" void nv2a_debug_state(char *buf, int n);
static DWORD WINAPI debug_watch_thread(void *arg) {
    DWORD after = (DWORD)(uintptr_t)arg;
    DWORD start = GetTickCount();
    bool dumped_after = !after;
    int stall_dumps = 0;
    bool in_stall = false;
    for (;;) {
        if (!dumped_after && GetTickCount() - start >= after * 1000) {
            debug_dump_threads("--dump-after");
            dumped_after = true;
        }
        if (GetAsyncKeyState(VK_F12) & 1) debug_dump_threads("F12");
        // stall watchdog (XB_STALL_DUMP=seconds): after that many seconds, dump threads when no
        // frame was flipped for 250 ms
        static const char *stall_env = getenv("XB_STALL_DUMP");
        bool stall_enabled = stall_env && GetTickCount() - start >= (DWORD)atoi(stall_env) * 1000;
        int64_t since = nv2a_us_since_flip();
        static const int stall_us = getenv("XB_STALL_MS") ? atoi(getenv("XB_STALL_MS")) * 1000 : 250000;
        if (stall_enabled && since > stall_us && !in_stall && stall_dumps < 20) {
            in_stall = true;
            stall_dumps++;
            for (int k = 0; k < 6; k++) {
                char why[32];
                snprintf(why, sizeof why, "stall %d.%d", stall_dumps, k);
                debug_dump_threads(why);
                char st[512];
                nv2a_debug_state(st, sizeof st);
                logf("  nv2a: %s", st);
                Sleep(stall_us >= 100000 ? 50 : 8);
            }
        } else if (since < 100000) {
            in_stall = false;
        }
        // Always on: the game stopped presenting for 10 s after it had been running (a hang).
        // Dump all threads + GPU state a few times once, so a user log shows where it is stuck.
        // XB_HANG_DUMP=0 disables, XB_HANG_DUMP=N sets the threshold in seconds.
        static const int hang_s = getenv("XB_HANG_DUMP") ? atoi(getenv("XB_HANG_DUMP")) : 10;
        static int hang_dumps;
        static bool hang_armed = true;
        if (hang_s > 0 && since > (int64_t)hang_s * 1000000 && hang_armed && hang_dumps < 3) {
            hang_armed = false;
            hang_dumps++;
            logf("hang: no frame presented for %lld s - dumping threads (XB_HANG_DUMP=0 disables)", (long long)(since / 1000000));
            for (int k = 0; k < 3; k++) {
                char why[32];
                snprintf(why, sizeof why, "hang %d.%d", hang_dumps, k);
                debug_dump_threads(why);
                char st[512];
                nv2a_debug_state(st, sizeof st);
                logf("  nv2a: %s", st);
                Sleep(300);
            }
        } else if (since < 1000000) {
            hang_armed = true;
        }
        Sleep(stall_enabled ? 5 : 20);
    }
}

// XB_DPC_SAMPLE=1: sample the DPC thread's stack whenever a DPC has been running for > 1.5 ms
extern volatile LONG64 g_dpc_running_since;
extern volatile uint32_t g_dpc_routine;
extern HANDLE g_dpc_thread_handle;
static DWORD WINAPI dpc_sampler(void *) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    int logged = 0;
    while (logged < 400) {
        Sleep(1);
        LONG64 since = g_dpc_running_since;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!since || (now.QuadPart - since) * 1000 < f.QuadPart * 3 / 2 || !g_dpc_thread_handle) continue;
        if (SuspendThread(g_dpc_thread_handle) == (DWORD)-1) continue;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_dpc_thread_handle, &c)) {
            char buf[600];
            int n = snprintf(buf, sizeof buf, "dpcsample routine %08x eip %08lx |", g_dpc_routine, c.Eip);
            uint32_t *sp = (uint32_t *)c.Esp;
            int depth = 2048;
            while (depth > 16 && !readable(sp, depth * 4)) depth /= 2;
            int found = 0;
            for (int i = 0; i < depth && found < 12 && n < (int)sizeof buf - 12; i++) {
                if ((sp[i] >= 0x11000 && sp[i] < 0x4B0000) || (sp[i] >= 0x4B0000 && sp[i] < 0x800000)) {
                    n += snprintf(buf + n, sizeof buf - n, " %08x", sp[i]);
                    found++;
                }
            }
            logf("%s", buf);
            logged++;
        }
        ResumeThread(g_dpc_thread_handle);
    }
    return 0;
}

void debug_init(int dump_after_seconds) {
    if (getenv("XB_DPC_SAMPLE")) CloseHandle(CreateThread(nullptr, 0, dpc_sampler, nullptr, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, debug_watch_thread, (void *)(uintptr_t)dump_after_seconds, 0, nullptr));
}

// ---------------------------------------------------------------------------
// Tracepoints: int3 at an Xbox code address, log registers, then continue
// (restore byte, single-step, re-arm). Use --trace ADDR[,ADDR...] on the command line.
struct Tracepoint {
    uint32_t addr;
    uint8_t orig;
    int hits;
    uint32_t last_regs[8][7]; // TRACE_DEDUP, per esi
    bool dedup_used[8];
};
static Tracepoint g_tps[32];
static int g_ntps;
static uint64_t g_tp_armed_ms;
static thread_local int g_rearm = -1;

static void tp_write(uint32_t addr, uint8_t v) {
    DWORD old;
    VirtualProtect((void *)addr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(volatile uint8_t *)addr = v;
    FlushInstructionCache(GetCurrentProcess(), (void *)addr, 1);
}

// Log up to `want` likely return addresses into game code found on a stack: dwords in the XBE's
// code range that follow a call instruction. Heuristic (stale frames show up too), for debugging.
extern "C" void debug_log_guest_stack(const uint32_t *sp, int want) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "   stack:"), got = 0;
    for (int k = 0; k < 512 && got < want && readable(sp + k, 4); k++) {
        uint32_t v = sp[k];
        if (v < 0x00011005 || v >= 0x00310000) continue;
        const uint8_t *p = (const uint8_t *)v;
        if (p[-5] == 0xE8 || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) ||
            (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)) {
            n += snprintf(buf + n, sizeof buf - n, " %08x", v);
            got++;
        }
    }
    logf("%s", buf);
}

static LONG CALLBACK tp_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;
    if (code == EXCEPTION_BREAKPOINT) {
        for (int i = 0; i < g_ntps; i++) {
            if (c->Eip != g_tps[i].addr) continue;
            Tracepoint &t = g_tps[i];
            // TRACE_AFTER_MS=N: ignore hits during the first N ms after the tracepoints were armed
            static const char *after = getenv("TRACE_AFTER_MS");
            bool counting = !after || GetTickCount64() - g_tp_armed_ms >= (uint64_t)_atoi64(after);
            // TRACE_DEDUP=1: per (tracepoint, esi) skip hits whose general registers equal the previous
            // hit's - logs state changes only, also when one site serves several objects (esi = this)
            static const bool dedup = getenv("TRACE_DEDUP") != nullptr;
            if (counting && dedup) {
                uint32_t regs[7] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
                int slot = -1;
                for (int s = 0; s < 8; s++)
                    if (t.dedup_used[s] && t.last_regs[s][4] == c->Esi) { slot = s; break; }
                if (slot < 0)
                    for (int s = 0; s < 8; s++)
                        if (!t.dedup_used[s]) { slot = s; t.dedup_used[s] = true; memset(t.last_regs[s], 0xFF, sizeof regs); break; }
                if (slot >= 0) {
                    if (!memcmp(regs, t.last_regs[slot], sizeof regs)) counting = false;
                    memcpy(t.last_regs[slot], regs, sizeof regs);
                }
            }
            if (counting && (t.hits++ < 50 || (getenv("TRACE_COUNT") && (t.hits % 1000) == 0))) {
                uint32_t *sp = (uint32_t *)c->Esp;
                logf("TP irql %d", xthread_current()->Irql);
                logf("TP %08x hit#%d eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx [sp]=%08x %08x %08x %08x",
                     t.addr, t.hits, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp,
                     readable(sp, 16) ? sp[0] : 0, readable(sp, 16) ? sp[1] : 0, readable(sp, 16) ? sp[2] : 0,
                     readable(sp, 16) ? sp[3] : 0);
                // TRACE_STACK=N: also list up to N return addresses found by scanning the stack
                if (const char *e = getenv("TRACE_STACK")) debug_log_guest_stack(sp, atoi(e));
                // TRACE_HEX=N: also dump 8 dwords at the pointer in stack slot N ([sp+4*N])
                if (const char *e = getenv("TRACE_HEX")) {
                    int k = atoi(e);
                    uint32_t *m = readable(sp + k, 4) ? (uint32_t *)sp[k] : nullptr;
                    if (m && readable(m, 32))
                        logf("   hex: %08x %08x %08x %08x %08x %08x %08x %08x", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
                }
                // TRACE_STR=N: also print the C string at the pointer in stack slot N ([sp+4*N])
                if (const char *e = getenv("TRACE_STR")) {
                    int k = atoi(e);
                    const char *s = readable(sp + k, 4) ? (const char *)sp[k] : nullptr;
                    if (s && readable(s, 1)) logf("   str: \"%.64s\"", s);
                }
                // TRACE_FLOATS=N: also dump 16 floats at the pointer in stack arg N ([sp+4*N])
                if (const char *e = getenv("TRACE_FLOATS")) {
                    int k = atoi(e);
                    float *m = readable(sp + k, 4) ? (float *)sp[k] : nullptr;
                    if (m && readable(m, 64))
                        logf("   %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f", m[0], m[1], m[2], m[3], m[4],
                             m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
                }
            }
            tp_write(t.addr, t.orig);
            c->EFlags |= 0x100; // single step
            g_rearm = i;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (code == EXCEPTION_SINGLE_STEP && g_rearm >= 0) {
        tp_write(g_tps[g_rearm].addr, 0xCC);
        g_rearm = -1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void debug_add_tracepoints(const char *list) {
    static bool installed;
    if (!installed) {
        AddVectoredExceptionHandler(1, tp_handler);
        installed = true;
        g_tp_armed_ms = GetTickCount64();
    }
    const char *p = list;
    while (*p && g_ntps < 32) {
        uint32_t a = strtoul(p, (char **)&p, 16);
        if (a) {
            g_tps[g_ntps].addr = a;
            g_tps[g_ntps].orig = *(uint8_t *)a;
            tp_write(a, 0xCC);
            logf("tracepoint armed at %08x", a);
            g_ntps++;
        }
        while (*p == ',' || *p == ' ') p++;
    }
}

// ---------------------------------------------------------------------------
// Sampling profiler: --profile START,SECONDS. Samples EIP of every thread each ms and writes
// profile.txt with the hottest addresses per thread (symbolize host ones with llvm-addr2line).
#include <map>
#include <algorithm>
static DWORD WINAPI profiler_thread(void *arg) {
    uint32_t packed = (uint32_t)(uintptr_t)arg;
    DWORD start = packed >> 16, secs = packed & 0xFFFF;
    Sleep(start * 1000);
    logf("profiler: sampling %lu s", secs);
    std::map<DWORD, std::map<uint32_t, uint32_t>> hits; // tid -> eip -> count
    std::map<DWORD, uint32_t> totals;
    std::map<DWORD, std::map<uint32_t, uint32_t>> callers; // tid -> first in-image return address -> count
    std::map<DWORD, std::map<uint32_t, uint32_t>> direct; // tid -> [esp+4] when stopped in a syscall stub
    DWORD self = GetCurrentThreadId();
    DWORD end = GetTickCount() + secs * 1000;
    while (GetTickCount() < end) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {sizeof(te)};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!h) continue;
            if (SuspendThread(h) != (DWORD)-1) {
                CONTEXT c = {};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) {
                    hits[te.th32ThreadID][c.Eip]++;
                    totals[te.th32ThreadID]++;
                    // outside our image: attribute to the nearest caller on the stack too
                    if (c.Eip >= 0x1000000) {
                        uint32_t *sp = (uint32_t *)c.Esp;
                        int depth = 4096;
                        while (depth > 16 && !readable(sp, depth * 4)) depth /= 2;
                        if (depth >= 2) direct[te.th32ThreadID][sp[0]]++;
                        for (int i = 0; i < depth; i++) {
                            if (sp[i] >= 0x11000 && sp[i] < 0x1000000) { // xbox or host image
                                callers[te.th32ThreadID][sp[i]]++;
                                break;
                            }
                        }
                    }
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
        Sleep(1);
    }
    FILE *f = fopen("profile.txt", "w");
    // name system addresses in-process (exports only, no PDBs needed)
    typedef BOOL(WINAPI * SymInit_t)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI * SymFromAddr_t)(HANDLE, DWORD64, PDWORD64, void *);
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    SymInit_t pSymInit = dh ? (SymInit_t)GetProcAddress(dh, "SymInitialize") : nullptr;
    SymFromAddr_t pSymFromAddr = dh ? (SymFromAddr_t)GetProcAddress(dh, "SymFromAddr") : nullptr;
    if (pSymInit) pSymInit(GetCurrentProcess(), nullptr, TRUE);
    auto sysname = [&](uint32_t a, char *out, size_t n) {
        out[0] = 0;
        if (a < 0x11000 || (a >= 0x11000 && a < 0x1000000)) return;
        struct {
            ULONG SizeOfStruct, TypeIndex;
            ULONG64 Reserved[2];
            ULONG Index, Size;
            ULONG64 ModBase;
            ULONG Flags;
            ULONG64 Value, Address;
            ULONG Register, Scope, Tag, NameLen, MaxNameLen;
            CHAR Name[256];
        } sym = {};
        sym.SizeOfStruct = 88;
        sym.MaxNameLen = 255;
        DWORD64 disp = 0;
        char modname[MAX_PATH] = "";
        HMODULE hm;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)a, &hm)) {
            GetModuleFileNameA(hm, modname, sizeof modname);
        }
        const char *base = strrchr(modname, '\\') ? strrchr(modname, '\\') + 1 : modname;
        if (pSymFromAddr && pSymFromAddr(GetCurrentProcess(), a, &disp, &sym)) snprintf(out, n, "%s!%s+%llx", base, sym.Name, disp);
        else snprintf(out, n, "%s", base);
    };
    for (auto &t : hits) {
        std::vector<std::pair<uint32_t, uint32_t>> v(t.second.begin(), t.second.end());
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
        // samples in the Windows kernel/ntdll wait functions mean the thread is idle
        fprintf(f, "thread %lu: %u samples\n", t.first, totals[t.first]);
        for (size_t i = 0; i < v.size() && i < 4000; i++) {
            char nm[300];
            sysname(v[i].first, nm, sizeof nm);
            fprintf(f, "  %08x %u %s\n", v[i].first, v[i].second, nm);
        }
        std::vector<std::pair<uint32_t, uint32_t>> w(callers[t.first].begin(), callers[t.first].end());
        std::sort(w.begin(), w.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < w.size() && i < 8; i++) fprintf(f, "  %08x %u <caller of system code>\n", w[i].first, w[i].second);
        std::vector<std::pair<uint32_t, uint32_t>> dv(direct[t.first].begin(), direct[t.first].end());
        std::sort(dv.begin(), dv.end(), [](auto &a, auto &b) { return a.second > b.second; });
        for (size_t i = 0; i < dv.size() && i < 12; i++) {
            char nm[300];
            sysname(dv[i].first, nm, sizeof nm);
            fprintf(f, "  %08x %u <direct %s\n", dv[i].first, dv[i].second, nm);
        }
    }
    fclose(f);
    logf("profiler: wrote profile.txt");
    return 0;
}

void debug_start_profiler(int start_s, int secs) {
    CloseHandle(CreateThread(nullptr, 0, profiler_thread, (void *)(uintptr_t)((start_s << 16) | (secs & 0xFFFF)), 0, nullptr));
}

// ---------------------------------------------------------------------------
// XB_HOOK_GECT=1: count the GL driver's GetExitCodeThread calls (IAT hook) and attribute them to
// the first return address in our image found on the stack. Logged every 2 s by debug_gect_report.
static BOOL(WINAPI *g_real_gect)(HANDLE, LPDWORD);
static volatile LONG g_gect_calls;
static volatile LONG g_gect_hist_addr[64], g_gect_hist_n[64];
static volatile LONG64 g_gect_ticks;
static uint32_t g_text_lo, g_text_hi;
static bool looks_like_return(const uint8_t *p) {
    if (p[-5] == 0xE8) return true;
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) return true;
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) return true;
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) return true;
    return false;
}
static volatile LONG g_gect_tid[4], g_gect_tid_n[4];
static BOOL WINAPI gect_hook(HANDLE h, LPDWORD code) {
    InterlockedIncrement(&g_gect_calls);
    LONG me = (LONG)GetCurrentThreadId();
    for (int k = 0; k < 4; k++) {
        LONG prev = InterlockedCompareExchange(&g_gect_tid[k], me, 0);
        if (prev == 0 || prev == me) { InterlockedIncrement(&g_gect_tid_n[k]); break; }
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    BOOL r = g_real_gect(h, code);
    QueryPerformanceCounter(&t1);
    InterlockedAdd64(&g_gect_ticks, t1.QuadPart - t0.QuadPart);
    uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
    uint32_t *top = (uint32_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    for (int i = 0; i < 2048 && sp + i < top; i++) {
        uint32_t a = sp[i];
        if (a >= 0x4B0000 && a < 0x786bc0 && looks_like_return((const uint8_t *)(uintptr_t)a)) {
            unsigned slot = (a * 2654435761u) >> 26;
            for (int k = 0; k < 64; k++) {
                unsigned s = (slot + k) & 63;
                LONG prev = InterlockedCompareExchange(&g_gect_hist_addr[s], (LONG)a, 0);
                if (prev == 0 || prev == (LONG)a) { InterlockedIncrement(&g_gect_hist_n[s]); break; }
            }
            break;
        }
    }
    return r;
}
extern "C" void debug_hook_gect(void) {
    if (!getenv("XB_HOOK_GECT")) return;
    {
        uint8_t *me = (uint8_t *)GetModuleHandleA(NULL);
        auto *nh = (IMAGE_NT_HEADERS *)(me + ((IMAGE_DOS_HEADER *)me)->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nh);
        for (int i = 0; i < nh->FileHeader.NumberOfSections; i++)
            if (!memcmp(sec[i].Name, ".text", 5)) {
                g_text_lo = (uint32_t)(uintptr_t)(me + sec[i].VirtualAddress);
                g_text_hi = g_text_lo + sec[i].Misc.VirtualSize;
            }
        logf("gect hook: host .text %08x-%08x", g_text_lo, g_text_hi);
    }
    HMODULE m = GetModuleHandleA("nvoglv32.dll");
    if (!m) { logf("gect hook: nvoglv32 not loaded"); return; }
    uint8_t *base = (uint8_t *)m;
    auto *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int patched = 0;
    for (auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        auto *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        auto *orig = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig->u1.AddressOfData; orig++, thunk++) {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto *ibn = (IMAGE_IMPORT_BY_NAME *)(base + orig->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, "GetExitCodeThread")) continue;
            DWORD old;
            VirtualProtect(&thunk->u1.Function, 4, PAGE_READWRITE, &old);
            g_real_gect = (BOOL(WINAPI *)(HANDLE, LPDWORD))thunk->u1.Function;
            thunk->u1.Function = (uintptr_t)gect_hook;
            VirtualProtect(&thunk->u1.Function, 4, old, &old);
            patched++;
        }
    }
    logf("gect hook: %d import(s) patched in nvoglv32", patched);
}
extern "C" void debug_gect_report(void) {
    if (!g_real_gect) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    char buf[1024]; int n = snprintf(buf, sizeof buf, "gect: %ld calls %.1f ms | tids", g_gect_calls, g_gect_ticks * 1000.0 / f.QuadPart);
    g_gect_calls = 0; g_gect_ticks = 0;
    for (int k = 0; k < 4; k++) { if (g_gect_tid_n[k]) n += snprintf(buf + n, sizeof buf - n, " %ld:%ld", g_gect_tid[k], g_gect_tid_n[k]); g_gect_tid_n[k] = 0; }
    n += snprintf(buf + n, sizeof buf - n, " |");
    for (int s = 0; s < 64; s++) {
        if (g_gect_hist_n[s]) n += snprintf(buf + n, sizeof buf - n, " %08lx:%ld", g_gect_hist_addr[s], g_gect_hist_n[s]);
        g_gect_hist_n[s] = 0;
    }
    logf("%s", buf);
}
