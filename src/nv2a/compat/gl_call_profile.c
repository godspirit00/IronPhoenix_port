/* GL call profiler (diagnostic). XB_GL_CALL_PROFILE=1 wraps every GL entry point; time spent in
 * each function on the renderer thread is logged every 2 s as "gl calls:" (top 16 by time). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "gl_call_profile.h"

void nv2a_log(const char *fmt, ...);

volatile unsigned long g_glcp_thread;
static LARGE_INTEGER g_freq;
static struct { uint64_t n, ticks; } *g_stats;
static int64_t g_report_at;
static int g_installed;

int64_t glcp_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static void report(int64_t now)
{
    int order[16], used = 0;
    uint64_t total = 0, calls = 0;
    for (int i = 0; i < glcp_count; i++) {
        total += g_stats[i].ticks;
        calls += g_stats[i].n;
    }
    while (used < 16) {
        int best = -1;
        for (int i = 0; i < glcp_count; i++) {
            bool taken = false;
            for (int j = 0; j < used; j++) taken |= order[j] == i;
            if (!taken && g_stats[i].n && (best < 0 || g_stats[i].ticks > g_stats[best].ticks)) best = i;
        }
        if (best < 0) break;
        order[used++] = best;
    }
    char buf[2048];
    int o = 0;
    for (int j = 0; j < used; j++) {
        int i = order[j];
        o += snprintf(buf + o, sizeof buf - o, " %s %.1fms/%llu", glcp_names[i] + 2,
                      g_stats[i].ticks * 1000.0 / g_freq.QuadPart, (unsigned long long)g_stats[i].n);
    }
    nv2a_log("gl calls: total %.0f ms / %llu calls |%s", total * 1000.0 / g_freq.QuadPart,
             (unsigned long long)calls, buf);
    memset(g_stats, 0, sizeof(*g_stats) * glcp_count);
    g_report_at = now + 2 * g_freq.QuadPart;
}

void glcp_record(int index, int64_t start)
{
    int64_t now = glcp_now();
    g_stats[index].n++;
    g_stats[index].ticks += now - start;
    if (now >= g_report_at) report(now);
}

/* Called after gladLoadGL. */
void nv2a_gl_call_profile_install(void)
{
    if (g_installed || !getenv("XB_GL_CALL_PROFILE")) return;
    g_installed = 1;
    QueryPerformanceFrequency(&g_freq);
    g_stats = calloc(glcp_count, sizeof(*g_stats));
    g_report_at = glcp_now() + 2 * g_freq.QuadPart;
    glcp_install_wrappers();
}

/* The renderer (pfifo) thread registers itself. */
void nv2a_gl_call_profile_set_thread(void)
{
    g_glcp_thread = GetCurrentThreadId();
}
