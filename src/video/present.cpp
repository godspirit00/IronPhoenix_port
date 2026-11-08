// Game window + presentation of the NV2A front buffer (OpenGL)
#include "video.h"
#include "../kernel/kernel.h"
#include <windows.h>
#include <dwmapi.h>
#include <glad/gl.h>
#include <glad/wgl.h>
#include <stdio.h>
#include <vector>

extern "C" {
void nv2a_host_context_init(void);
void nv2a_host_init(uint8_t *ram, uint32_t ram_size);
int nv2a_get_framebuffer_surface(void);
void nv2a_gl_wait_display_fence(void);
void nv2a_release_framebuffer_surface(void);
const uint8_t *nv2a_get_dac_palette(void);
void *glo_create_window_context(void *hdc);
bool nv2a_renderer_ready(void);
}

extern bool g_window_focused;
VideoConfig g_video;

static HWND g_hwnd;
static HDC g_hdc;
static HGLRC g_glrc;
static bool g_fullscreen;
static WINDOWPLACEMENT g_prev_placement = {sizeof(WINDOWPLACEMENT)};
static volatile bool g_quit;

static GLuint g_prog, g_vao, g_vbo;
static GLint g_loc_scale, g_loc_tex, g_loc_palette;

static void toggle_fullscreen() {
    DWORD style = GetWindowLongA(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (GetWindowPlacement(g_hwnd, &g_prev_placement) &&
            GetMonitorInfoA(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } else {
        SetWindowLongA(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_prev_placement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_quit = true;
        return 0;
    case WM_ACTIVATE:
        g_window_focused = LOWORD(wp) != WA_INACTIVE;
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { // Alt+Enter
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            toggle_fullscreen();
            return 0;
        }
        break;
    case WM_SYSCHAR: // suppress the beep on Alt+Enter
        if (wp == VK_RETURN) return 0;
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        fatal("present shader compile failed: %s", log);
    }
    return s;
}

static void init_gl_objects() {
    const char *vs = R"(#version 330 core
uniform vec2 u_scale;
out vec2 v_tc;
void main() {
    vec2 p = vec2((gl_VertexID & 1) * 2.0 - 1.0, (gl_VertexID >> 1) * 2.0 - 1.0);
    v_tc = p * 0.5 + 0.5;
    gl_Position = vec4(p * u_scale, 0.0, 1.0);
})";
    const char *fs = R"(#version 400 core
uniform sampler2D u_tex;
uniform uint u_palette[256];
in vec2 v_tc;
out vec4 o_color;
float gamma_ch(int ch, float c) {
    return float(bitfieldExtract(u_palette[uint(clamp(c, 0.0, 1.0) * 255.0)], ch * 8, 8)) / 255.0;
}
void main() {
    vec4 c = texture(u_tex, v_tc);
    o_color = vec4(gamma_ch(0, c.r), gamma_ch(1, c.g), gamma_ch(2, c.b), 1.0);
})";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_prog);
    g_loc_scale = glGetUniformLocation(g_prog, "u_scale");
    g_loc_tex = glGetUniformLocation(g_prog, "u_tex");
    g_loc_palette = glGetUniformLocation(g_prog, "u_palette");
    glGenVertexArrays(1, &g_vao);
}

void video_init() {
    WNDCLASSA wc = {};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "IronPhoenixWindow";
    RegisterClassA(&wc);
    RECT r = {0, 0, g_video.window_width, g_video.window_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("IronPhoenixWindow", "Iron Phoenix", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    g_hdc = GetDC(g_hwnd);

    // Shared offscreen contexts for the GPU emulation (render + display) are created here,
    // then our window context joins the same share group.
    nv2a_host_context_init();
    g_glrc = (HGLRC)glo_create_window_context(g_hdc);
    if (!g_glrc) fatal("cannot create window GL context");
    wglMakeCurrent(g_hdc, g_glrc);
    // Never let the driver block on vblank: a vsync'd SwapBuffers also throttles the emulated
    // GPU's shared context. Vsync is done by pacing on DWM composition instead (main loop).
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(0);
    init_gl_objects();

    ShowWindow(g_hwnd, SW_SHOW);
    if (g_video.fullscreen) toggle_fullscreen();
    nv2a_host_init(xptr<uint8_t>(XBOX_CONTIG_BASE), XBOX_DEVICE_RAM);
}

// Debug: periodically save the presented frame as a BMP (build/shots/)
static void save_screenshot(int w, int h) {
    static int n;
    CreateDirectoryA("shots", nullptr);
    std::vector<uint8_t> px((size_t)w * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());
    char name[64];
    snprintf(name, sizeof name, "shots/shot_%04d.bmp", n++);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    uint32_t off = 54, dib = 40, planes_bpp = 1 | (24 << 16), img = row * h;
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = 0; y < h; y++) {
        memcpy(line.data(), px.data() + (size_t)y * w * 3, w * 3);
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    logf("screenshot %s", name);
}

static void present_frame() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    bool acquired = nv2a_renderer_ready();
    GLuint tex = acquired ? (GLuint)nv2a_get_framebuffer_surface() : 0;
    if (tex) nv2a_gl_wait_display_fence();
    if (tex) {
        float target = g_video.aspect > 0 ? g_video.aspect : 4.0f / 3.0f;
        float win = wh ? (float)ww / (float)wh : 1.0f;
        float sx = 1, sy = 1;
        if (win > target) sx = target / win;
        else sy = win / target;
        glUseProgram(g_prog);
        glBindVertexArray(g_vao);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glUniform1i(g_loc_tex, 0);
        glUniform2f(g_loc_scale, sx, sy);
        const uint8_t *pal = nv2a_get_dac_palette();
        GLuint p[256];
        for (int i = 0; i < 256; i++) p[i] = (pal[i * 3 + 2] << 16) | (pal[i * 3 + 1] << 8) | pal[i * 3];
        glUniform1uiv(g_loc_palette, 256, p);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    if (acquired) nv2a_release_framebuffer_surface();
    if (g_video.shot_every_ms) {
        static DWORD last = GetTickCount();
        if (GetTickCount() - last >= (DWORD)g_video.shot_every_ms) {
            last = GetTickCount();
            save_screenshot(ww, wh);
        }
    }
    SwapBuffers(g_hdc);
}

void video_main_loop() {
    while (!g_quit) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        present_frame();
        if (!g_video.vsync || FAILED(DwmFlush())) Sleep(1);
    }
    logf("window closed - exiting");
    ExitProcess(0);
}
