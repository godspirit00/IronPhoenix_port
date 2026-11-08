#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

bool g_log_kernel = false;
bool g_log_files = false;
char g_game_dir[512];
char g_save_dir[512];

static FILE *g_log;
static CRITICAL_SECTION g_log_lock;

void log_init(const char *path) {
    InitializeCriticalSection(&g_log_lock);
    g_log = fopen(path, "w");
}

static void vlog(const char *prefix, const char *fmt, va_list ap) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[%5lu %6lu] %s", GetCurrentThreadId() % 100000, GetTickCount() % 1000000, prefix);
    vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    EnterCriticalSection(&g_log_lock);
    fputs(buf, stderr);
    fputc('\n', stderr);
    if (g_log) {
        fputs(buf, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_log_lock);
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
