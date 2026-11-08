/* gloffscreen API on top of WGL: offscreen OpenGL 4.x core contexts that all share objects. */
#include "nv2a_compat.h"
#include <windows.h>
#include <glad/gl.h>
#include <glad/wgl.h>
#include "pgraph/thirdparty/gloffscreen/gloffscreen.h"

struct _GloContext {
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;
};

static GloContext *g_share_root;
static HMODULE g_opengl32;

static GLADapiproc glo_get_proc(const char *name)
{
    PROC p = wglGetProcAddress(name);
    if (p == 0 || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1) {
        if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
        p = GetProcAddress(g_opengl32, name);
    }
    return (GLADapiproc)p;
}
static int g_pixel_format;
static PIXELFORMATDESCRIPTOR g_pfd;
static bool g_wgl_loaded;

static HWND make_hidden_window(void) {
    static bool registered;
    if (!registered) {
        WNDCLASSA wc = {0};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "IronPhoenixGlo";
        RegisterClassA(&wc);
        registered = true;
    }
    return CreateWindowA("IronPhoenixGlo", "glo", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, NULL, NULL, GetModuleHandleA(NULL), NULL);
}

static void load_wgl(void) {
    if (g_wgl_loaded) return;
    /* A legacy dummy context is required to load the WGL extension entry points */
    HWND w = make_hidden_window();
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dc, &pfd);
    SetPixelFormat(dc, pf, &pfd);
    HGLRC tmp = wglCreateContext(dc);
    wglMakeCurrent(dc, tmp);
    if (!gladLoadWGL(dc, glo_get_proc)) fprintf(stderr, "glo: failed to load WGL\n");
    g_pixel_format = pf;
    g_pfd = pfd;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    ReleaseDC(w, dc);
    DestroyWindow(w);
    g_wgl_loaded = true;
}

GloContext *glo_context_create(void) {
    load_wgl();
    GloContext *c = g_malloc0(sizeof(*c));
    c->hwnd = make_hidden_window();
    c->hdc = GetDC(c->hwnd);
    SetPixelFormat(c->hdc, g_pixel_format, &g_pfd);
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };
    c->hglrc = wglCreateContextAttribsARB(c->hdc, g_share_root ? g_share_root->hglrc : NULL, attribs);
    if (!c->hglrc) {
        fprintf(stderr, "glo: wglCreateContextAttribsARB failed (%lu)\n", GetLastError());
        abort();
    }
    if (!g_share_root) {
        g_share_root = c;
        wglMakeCurrent(c->hdc, c->hglrc);
        if (!gladLoadGL(glo_get_proc)) {
            fprintf(stderr, "glo: failed to load OpenGL\n");
            abort();
        }
        nv2a_log("OpenGL: %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
        extern void debug_hook_gect(void);
        debug_hook_gect();
    }
    return c;
}

/* Context sharing everything with the glo contexts, but bound to a real window DC. */
void *glo_create_window_context(void *hdc_) {
    load_wgl();
    HDC hdc = (HDC)hdc_;
    SetPixelFormat(hdc, g_pixel_format, &g_pfd);
    int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0,
    };
    return wglCreateContextAttribsARB(hdc, g_share_root ? g_share_root->hglrc : NULL, attribs);
}

void glo_set_current(GloContext *context) {
    if (context) wglMakeCurrent(context->hdc, context->hglrc);
    else wglMakeCurrent(NULL, NULL);
}

bool glo_check_extension(const char *ext_name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, i);
        if (e && !strcmp(e, ext_name)) return true;
    }
    return false;
}

void glo_context_destroy(GloContext *context) {
    if (!context) return;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(context->hglrc);
    ReleaseDC(context->hwnd, context->hdc);
    DestroyWindow(context->hwnd);
    free(context);
}

void glo_readpixels(GLenum gl_format, GLenum gl_type, unsigned int bytes_per_pixel, unsigned int stride,
                    unsigned int width, unsigned int height, bool vflip, void *data) {
    glPixelStorei(GL_PACK_ROW_LENGTH, stride / bytes_per_pixel);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, gl_format, gl_type, data);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (vflip) {
        uint8_t *tmp = malloc(stride);
        for (unsigned y = 0; y < height / 2; y++) {
            uint8_t *a = (uint8_t *)data + y * stride, *b = (uint8_t *)data + (height - 1 - y) * stride;
            memcpy(tmp, a, stride);
            memcpy(a, b, stride);
            memcpy(b, tmp, stride);
        }
        free(tmp);
    }
}
