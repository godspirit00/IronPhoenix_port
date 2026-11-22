#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

bool g_log_kernel = false;
bool g_log_files = false;
char g_game_dir[512];
char g_save_dir[512];

// Asynchronous log. Callers format and timestamp a line and copy it into a buffer under a short
// lock; a writer thread does the stderr / log file I/O in batches. Before this, every line was
// an unbuffered stderr write plus an fflush under one global lock, on the calling thread: with
// the log on a slow disk (laptop HDD on the GTX 1060 machine) a single write took 250-630 ms and
// froze whichever thread logged - the vblank thread (no vblank, no flips) or the renderer.
// A full buffer makes the caller wait (no line is dropped). XB_LOG_SYNC=1 restores direct writes.
static FILE *g_log;
static CRITICAL_SECTION g_log_lock;
static CONDITION_VARIABLE g_log_have_data, g_log_have_space, g_log_written;
enum { LOG_BUF_SIZE = 1 << 20 };
static char *g_log_fill;           // buffer callers append to (guarded by g_log_lock)
static size_t g_log_fill_len;
static char *g_log_spare;          // buffer the writer owns between swaps
static uint64_t g_log_queued_seq;  // lines appended so far
static uint64_t g_log_written_seq; // lines written and flushed
static bool g_log_async, g_log_ready;
static DWORD g_log_writer_tid;

static void log_write_out(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    fflush(stderr);
    if (g_log) {
        fwrite(p, 1, n, g_log);
        fflush(g_log);
    }
}

static void vlog(const char *prefix, const char *fmt, va_list ap);
static void log_from_writer(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

static DWORD WINAPI log_writer(void *) {
    // above the game thread, below the emulator's real-time threads: a starved writer would
    // otherwise hold lines back until the process ends (and lose them on a kill)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    for (;;) {
        EnterCriticalSection(&g_log_lock);
        while (!g_log_fill_len) SleepConditionVariableCS(&g_log_have_data, &g_log_lock, INFINITE);
        char *p = g_log_fill;
        size_t n = g_log_fill_len;
        uint64_t seq = g_log_queued_seq;
        g_log_fill = g_log_spare;
        g_log_fill_len = 0;
        g_log_spare = p;
        LeaveCriticalSection(&g_log_lock);
        WakeAllConditionVariable(&g_log_have_space);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        log_write_out(p, n);
        QueryPerformanceCounter(&t1);
        EnterCriticalSection(&g_log_lock);
        uint64_t backlog = g_log_queued_seq - seq;
        g_log_written_seq = seq;
        LeaveCriticalSection(&g_log_lock);
        WakeAllConditionVariable(&g_log_written);
        // a write this slow would have stalled the logging thread before the log was asynchronous
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        if (ms >= 100) log_from_writer("log: write of %zu bytes took %.0f ms (%llu lines queued meanwhile)", n, ms, (unsigned long long)backlog);
    }
}

void log_init(const char *path) {
    InitializeCriticalSection(&g_log_lock);
    InitializeConditionVariable(&g_log_have_data);
    InitializeConditionVariable(&g_log_have_space);
    InitializeConditionVariable(&g_log_written);
    g_log = fopen(path, "w");
    const char *sync = getenv("XB_LOG_SYNC");
    g_log_fill = (char *)malloc(LOG_BUF_SIZE);
    g_log_spare = (char *)malloc(LOG_BUF_SIZE);
    g_log_async = !(sync && atoi(sync)) && g_log_fill && g_log_spare;
    if (g_log_async) {
        HANDLE h = CreateThread(nullptr, 0, log_writer, nullptr, 0, &g_log_writer_tid);
        if (h) CloseHandle(h);
        else g_log_async = false;
    }
    atexit(log_flush);
    g_log_ready = true;
}

unsigned long log_writer_tid() { return g_log_writer_tid; }

void log_flush() {
    if (!g_log_ready || !g_log_async || GetCurrentThreadId() == g_log_writer_tid) return;
    EnterCriticalSection(&g_log_lock);
    uint64_t target = g_log_queued_seq;
    DWORD start = GetTickCount();
    // bounded: a crash or exit must not hang on a stuck disk (or a suspended writer)
    while (g_log_written_seq < target && GetTickCount() - start < 3000)
        SleepConditionVariableCS(&g_log_written, &g_log_lock, 100);
    LeaveCriticalSection(&g_log_lock);
}

static void vlog(const char *prefix, const char *fmt, va_list ap) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[%5lu %6lu] %s", GetCurrentThreadId() % 100000, GetTickCount() % 1000000, prefix);
    if (n < 0) n = 0;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    int m = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    if (m < 0) m = 0;
    size_t len = (size_t)n + (size_t)m;
    if (len > sizeof buf - 2) len = sizeof buf - 2;
    buf[len++] = '\n';
    buf[len] = 0;
    if (!g_log_ready) { // before log_init
        fputs(buf, stderr);
        return;
    }
    if (!g_log_async || GetCurrentThreadId() == g_log_writer_tid) {
        LARGE_INTEGER t0, t1, qf;
        QueryPerformanceCounter(&t0);
        EnterCriticalSection(&g_log_lock);
        log_write_out(buf, len);
        LeaveCriticalSection(&g_log_lock);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&qf);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
        if (!g_log_async && ms >= 100) { // the calling thread was blocked this long (lock + write)
            char note[96];
            int k = snprintf(note, sizeof note, "[%5lu %6lu] log: synchronous write blocked this thread %.0f ms\n",
                             GetCurrentThreadId() % 100000, GetTickCount() % 1000000, ms);
            EnterCriticalSection(&g_log_lock);
            log_write_out(note, (size_t)k);
            LeaveCriticalSection(&g_log_lock);
        }
        return;
    }
    EnterCriticalSection(&g_log_lock);
    while (g_log_fill_len + len > LOG_BUF_SIZE) SleepConditionVariableCS(&g_log_have_space, &g_log_lock, INFINITE);
    memcpy(g_log_fill + g_log_fill_len, buf, len);
    g_log_fill_len += len;
    g_log_queued_seq++;
    LeaveCriticalSection(&g_log_lock);
    WakeConditionVariable(&g_log_have_data);
}

void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("FATAL: ", fmt, ap);
    va_end(ap);
    log_flush();
    if (g_log) fflush(g_log);
    TerminateProcess(GetCurrentProcess(), 1);
    for (;;) {}
}

// Walk the EBP chain from the caller and log return addresses (best effort; FPO frames are skipped).
void log_backtrace(const char *tag) {
    uint32_t *bp = (uint32_t *)__builtin_frame_address(0);
    char buf[600];
    int n = 0;
    for (int i = 0; i < 20 && !IsBadReadPtr(bp, 8) && n < (int)sizeof buf - 12; i++) {
        n += snprintf(buf + n, sizeof buf - n, "%08x ", bp[1]);
        uint32_t *next = (uint32_t *)bp[0];
        if (next <= bp) break;
        bp = next;
    }
    logf("backtrace[%s]: %s", tag, buf);
}

extern "C" void nv2a_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && buf[n - 1] == '\n') buf[--n] = 0;
    logf("%s", buf);
}
