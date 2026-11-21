/* GL call profiler (diagnostic, XB_GL_CALL_PROFILE=1). See tools/gen_gl_call_profile.py. */
#pragma once
#include <stdint.h>
#include <glad/gl.h>

extern volatile unsigned long g_glcp_thread; /* renderer thread id; 0 = not set */
int64_t glcp_now(void);
void glcp_record(int index, int64_t start);

static inline int64_t glcp_begin(void)
{
    extern unsigned long __stdcall GetCurrentThreadId(void);
    return GetCurrentThreadId() == g_glcp_thread ? glcp_now() : 0;
}

static inline void glcp_end(int index, int64_t start)
{
    if (start) glcp_record(index, start);
}

extern const char *const glcp_names[];
extern const int glcp_count;
void glcp_install_wrappers(void);
