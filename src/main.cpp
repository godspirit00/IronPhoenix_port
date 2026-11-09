// Iron Phoenix native runtime - entry point
#include "common.h"
#include "xbe.h"
#include "kernel/kernel.h"
#include "cpu/patch.h"
#include "hle/hle.h"
#include "video/video.h"
extern "C" void nv2a_config_set_surface_scale(int scale);
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

void misc_init();
void debug_init(int dump_after_seconds);
void debug_add_tracepoints(const char *list);
void debug_start_profiler(int start_s, int secs);
void audio_set_dump(const char *path);
extern "C" void nv2a_set_refresh_rate(int hz);
extern "C" void nv2a_set_ui_squash(float s);
void apu_init();
void aci_init();
void nvnet_init();

static bool readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    static thread_local bool inside;
    if (inside) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_BREAKPOINT)
        return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
    CONTEXT *c = ep->ContextRecord;
    char mod[MAX_PATH] = "?";
    HMODULE hm = nullptr;
    if (c->Eip >= 0x11000 && c->Eip < 0x4B0000) strcpy(mod, "xbox code");
    else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)c->Eip, &hm))
        GetModuleFileNameA(hm, mod, sizeof mod);
    logf("EXCEPTION %08lx at %08lx (%s +%lx) info=[%08lx %08lx]", code, c->Eip, mod, hm ? c->Eip - (DWORD)hm : 0,
         (unsigned long)ep->ExceptionRecord->ExceptionInformation[0], (unsigned long)ep->ExceptionRecord->ExceptionInformation[1]);
    logf("  eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
         c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    uint32_t *sp = (uint32_t *)c->Esp;
    char buf[512];
    int n = 0;
    for (int i = 0; i < 16; i++) n += snprintf(buf + n, sizeof buf - n, "%08x ", readable(sp + i, 4) ? sp[i] : 0);
    logf("  stack: %s", buf);
    // Walk the EBP chain for a rough backtrace
    uint32_t *bp = (uint32_t *)c->Ebp;
    n = 0;
    for (int i = 0; i < 12 && readable(bp, 8); i++) {
        n += snprintf(buf + n, sizeof buf - n, "%08x ", bp[1]);
        bp = (uint32_t *)bp[0];
    }
    logf("  frames: %s", buf);
    inside = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI xbox_entry_thread(void *) {
    XThreadBlock *b = xthread_create_block(0);
    xthread_attach(b);
    logf("calling XBE entry point %08x", g_xbe.entry);
    ((void (*)())xptr<void>(g_xbe.entry))();
    logf("XBE entry point returned");
    return 0;
}

static void get_exe_dir(char *out, size_t n) {
    GetModuleFileNameA(nullptr, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    if (s) *s = 0;
}

int main(int argc, char **argv) {
    char exe_dir[512];
    int dump_after = 0;
    const char *trace_list = nullptr;
    const char *input_script = nullptr;
    get_exe_dir(exe_dir, sizeof exe_dir);
    snprintf(g_game_dir, sizeof g_game_dir, "%s\\game", exe_dir);
    snprintf(g_save_dir, sizeof g_save_dir, "%s\\save", exe_dir);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) snprintf(g_game_dir, sizeof g_game_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) snprintf(g_save_dir, sizeof g_save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "-vk")) g_log_kernel = true;
        else if (!strcmp(argv[i], "-vf")) g_log_files = true;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) g_video.surface_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) g_video.fullscreen = true;
        else if (!strcmp(argv[i], "--aspect") && i + 1 < argc) { // 16:9 (default) or 4:3
            const char *a = argv[++i];
            if (!strcmp(a, "4:3")) { g_video.aspect = 4.0f / 3.0f; g_video.window_width = 1280; g_video.window_height = 960; }
            else g_video.aspect = 16.0f / 9.0f;
        }
        else if (!strcmp(argv[i], "--stretch-ui")) g_video.stretch_ui = true; // widescreen: original stretched 2D UI
        else if (!strcmp(argv[i], "--novsync")) g_video.vsync = false;
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc) nv2a_set_refresh_rate(atoi(argv[++i])); // emulated vblank rate
        else if (!strcmp(argv[i], "--dump-after") && i + 1 < argc) dump_after = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace_list = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_script = argv[++i];
        else if (!strcmp(argv[i], "--audio-dump") && i + 1 < argc) audio_set_dump(argv[++i]);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            int a = 0, b = 10;
            sscanf(argv[++i], "%d,%d", &a, &b);
            debug_start_profiler(a, b);
        }
        else if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) g_video.shot_every_ms = (int)(atof(argv[++i]) * 1000);
    }
    char logpath[600];
    snprintf(logpath, sizeof logpath, "%s\\ironphoenix.log", exe_dir);
    log_init(logpath);
    setvbuf(stdout, nullptr, _IONBF, 0);
    logf("Iron Phoenix native runtime starting. game=%s save=%s", g_game_dir, g_save_dir);
    {
        typedef const char *(__cdecl * wine_version_fn)(void);
        auto wine_version = (wine_version_fn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version");
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        logf("host: %s%s, %lu logical CPUs", wine_version ? "Wine " : "Windows", wine_version ? wine_version() : "",
             si.dwNumberOfProcessors);
    }

    AddVectoredExceptionHandler(1, crash_handler);
    debug_init(dump_after);
    ke_init();
    mm_init();
    io_init();
    nv2a_config_set_surface_scale(g_video.surface_scale);
    // Widescreen UI: the game draws all 2D in 640x480 and the anamorphic 16:9 output stretches it;
    // squash 2D draws back to 4:3 proportions (hle/fixes.cpp turns off the game's own partial fix).
    if (g_video.aspect > 1.5f && !g_video.stretch_ui) nv2a_set_ui_squash((4.0f / 3.0f) / g_video.aspect);
    video_init();
    apu_init();
    aci_init();
    nvnet_init();
    misc_init();

    char xbe_path[600];
    snprintf(xbe_path, sizeof xbe_path, "%s\\default.xbe", g_game_dir);
    if (!xbe_load(xbe_path)) fatal("failed to load %s", xbe_path);
    xbe_resolve_kernel_imports();
    patch_fs_accesses();
    patch_timing();
    hle_install();
    if (getenv("XB_MMIO_BENCH")) { // debug: cost of one trapped MMIO access (NV_PMC_BOOT_0 read)
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceFrequency(&f);
        volatile uint32_t *reg = (volatile uint32_t *)0xFD000000u;
        uint32_t x = 0;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 10000; i++) x += *reg;
        QueryPerformanceCounter(&t1);
        logf("mmio bench: %.2f us per trapped read (%08x)", (t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / 10000, x);
    }
    if (trace_list) debug_add_tracepoints(trace_list);
    if (input_script) hle_input_set_script(input_script);

    HANDLE t = CreateThread(nullptr, 1 << 20, xbox_entry_thread, nullptr, 0, nullptr);
    video_main_loop(); // window / message pump / presentation
    (void)t;
    return 0;
}
