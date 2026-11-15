// XAPI input (XInitDevices / XGetDevices / XInput*) on top of host XInput + keyboard
#include "hle.h"
#include "../kernel/kernel.h"
#include "../cpu/patch.h"
#include "../symbols.gen.h"
#include <windows.h>
#include <xinput.h>
#include <stdlib.h>
#include <vector>
#include <atomic>
#include "desktop_input.h"

#pragma pack(push, 1)
struct X_XINPUT_GAMEPAD {
    uint16_t wButtons;
    uint8_t bAnalogButtons[8];
    int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct X_XINPUT_STATE {
    uint32_t dwPacketNumber;
    X_XINPUT_GAMEPAD Gamepad;
};
struct X_XINPUT_CAPABILITIES {
    uint8_t SubType;
    uint16_t Reserved;
    X_XINPUT_GAMEPAD In;
    uint16_t OutLeft, OutRight;
};
struct X_XINPUT_FEEDBACK {
    uint32_t dwStatus;
    uint32_t hEvent;
    uint8_t Reserved[58];
    uint16_t wLeftMotorSpeed, wRightMotorSpeed;
};
#pragma pack(pop)

enum {
    XB_DPAD_UP = 1, XB_DPAD_DOWN = 2, XB_DPAD_LEFT = 4, XB_DPAD_RIGHT = 8,
    XB_START = 0x10, XB_BACK = 0x20, XB_LEFT_THUMB = 0x40, XB_RIGHT_THUMB = 0x80,
};
enum { AB_A, AB_B, AB_X, AB_Y, AB_BLACK, AB_WHITE, AB_LT, AB_RT };

typedef DWORD(WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION *);
static XInputGetState_t pXInputGetState;
static XInputSetState_t pXInputSetState;
static uint32_t g_packet[4];
static bool g_reported_insert;
static const uint32_t kPadHandleBase = 0x7A000000; // fake device handles

static std::atomic<bool> g_window_focused{false}, g_mouse_client{false};
static std::atomic<unsigned> g_focus_epoch{0};
static bool g_context_supported;
static std::atomic<uint32_t> g_checked_manager{0};
static SRWLOCK g_desktop_lock = SRWLOCK_INIT, g_mouse_lock = SRWLOCK_INIT;
static DesktopMouseLook g_mouse;
static float g_mouse_sensitivity = 1.0f;
static bool g_mouse_enabled = true;

static DesktopContext input_context() {
    if (!g_context_supported) return DesktopContext::Startup;
    uint32_t manager = __atomic_load_n(xptr<uint32_t>(0x36224c), __ATOMIC_RELAXED);
    if (!manager) return DesktopContext::Startup;
    if (g_checked_manager.load() != manager) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(xptr<void>(manager), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            uint64_t(manager) + 0x5e8 > uintptr_t(mbi.BaseAddress) + mbi.RegionSize)
            return DesktopContext::Startup;
        g_checked_manager.store(manager);
    }
    auto field = [manager](unsigned offset) {
        return (int)__atomic_load_n(xptr<uint32_t>(manager + offset), __ATOMIC_RELAXED);
    };
    int app = field(0x5e4), ui = field(0x204), pending = field(0x208), top = field(0x20c);
    DesktopContext context = desktop_context(true, app, ui, pending, top);
    static std::atomic<int> previous{-1};
    if (previous.exchange((int)context) != (int)context)
        logf("desktop input: %s (app %d ui %d pending %d top %d)",
            context == DesktopContext::Gameplay ? "gameplay" : context == DesktopContext::Menu ? "menu" : "startup",
            app, ui, pending, top);
    return context;
}

void hle_input_configure_mouse(float sensitivity, bool enabled) {
    g_mouse_sensitivity = sensitivity;
    g_mouse_enabled = enabled;
}
void hle_input_mouse_capture(bool captured) {
    AcquireSRWLockExclusive(&g_mouse_lock);
    g_mouse.reset(GetTickCount(), captured);
    ReleaseSRWLockExclusive(&g_mouse_lock);
}
void hle_input_set_focus(bool focused) {
    if (g_window_focused.exchange(focused) != focused) ++g_focus_epoch;
    if (!focused) hle_input_mouse_capture(false);
}
void hle_input_set_mouse_client(bool inside) { g_mouse_client.store(inside); }
void hle_input_mouse_motion(int dx, int dy) {
    AcquireSRWLockExclusive(&g_mouse_lock);
    g_mouse.move(dx, dy, GetTickCount());
    ReleaseSRWLockExclusive(&g_mouse_lock);
}

static bool key(int vk) { return g_window_focused && (GetAsyncKeyState(vk) & 0x8000); }

// Debug/test: scripted input "--input 120:start,125.5:a" (seconds since start : button)
struct ScriptedPress {
    uint32_t at_ms;
    char button[16];
};
static std::vector<ScriptedPress> g_script;
static DWORD g_script_t0;
static bool g_script_exclusive;

bool hle_input_wants_mouse_capture() {
    return g_mouse_enabled && !g_script_exclusive && input_context() == DesktopContext::Gameplay;
}

void hle_input_set_script(const char *spec, bool exclusive) {
    g_script_exclusive = exclusive;
    g_script_t0 = GetTickCount();
    g_script.clear();
    const char *p = spec;
    while (*p) {
        ScriptedPress press = {};
        double t = strtod(p, (char **)&p);
        if (*p != ':') break;
        p++;
        int n = 0;
        while (*p && *p != ',' && n < 15) press.button[n++] = *p++;
        press.at_ms = (uint32_t)(t * 1000);
        g_script.push_back(press);
        if (*p == ',') p++;
    }
    logf("input script: %u presses", (unsigned)g_script.size());
}

static void apply_script(X_XINPUT_GAMEPAD *g) {
    uint32_t now = GetTickCount() - g_script_t0;
    for (size_t i = 0; i < g_script.size(); i++) {
        if (now < g_script[i].at_ms || now > g_script[i].at_ms + 150) continue;
        const char *b = g_script[i].button;
        if (!strcmp(b, "start")) g->wButtons |= XB_START;
        else if (!strcmp(b, "back")) g->wButtons |= XB_BACK;
        else if (!strcmp(b, "up")) g->wButtons |= XB_DPAD_UP;
        else if (!strcmp(b, "down")) g->wButtons |= XB_DPAD_DOWN;
        else if (!strcmp(b, "left")) g->wButtons |= XB_DPAD_LEFT;
        else if (!strcmp(b, "right")) g->wButtons |= XB_DPAD_RIGHT;
        else if (!strcmp(b, "a")) g->bAnalogButtons[AB_A] = 255;
        else if (!strcmp(b, "b")) g->bAnalogButtons[AB_B] = 255;
        else if (!strcmp(b, "x")) g->bAnalogButtons[AB_X] = 255;
        else if (!strcmp(b, "y")) g->bAnalogButtons[AB_Y] = 255;
        else if (!strcmp(b, "lup")) g->sThumbLY = 32767;
        else if (!strcmp(b, "ldown")) g->sThumbLY = -32768;
    }
}

static void read_pad(int port, X_XINPUT_GAMEPAD *g) {
    memset(g, 0, sizeof *g);
    if (g_script_exclusive) {
        if (port == 0) apply_script(g);
        return;
    }
    XINPUT_STATE s;
    if (pXInputGetState && pXInputGetState(port, &s) == ERROR_SUCCESS) {
        WORD b = s.Gamepad.wButtons;
        if (b & XINPUT_GAMEPAD_DPAD_UP) g->wButtons |= XB_DPAD_UP;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN) g->wButtons |= XB_DPAD_DOWN;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT) g->wButtons |= XB_DPAD_LEFT;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT) g->wButtons |= XB_DPAD_RIGHT;
        if (b & XINPUT_GAMEPAD_START) g->wButtons |= XB_START;
        if (b & XINPUT_GAMEPAD_BACK) g->wButtons |= XB_BACK;
        if (b & XINPUT_GAMEPAD_LEFT_THUMB) g->wButtons |= XB_LEFT_THUMB;
        if (b & XINPUT_GAMEPAD_RIGHT_THUMB) g->wButtons |= XB_RIGHT_THUMB;
        g->bAnalogButtons[AB_A] = (b & XINPUT_GAMEPAD_A) ? 255 : 0;
        g->bAnalogButtons[AB_B] = (b & XINPUT_GAMEPAD_B) ? 255 : 0;
        g->bAnalogButtons[AB_X] = (b & XINPUT_GAMEPAD_X) ? 255 : 0;
        g->bAnalogButtons[AB_Y] = (b & XINPUT_GAMEPAD_Y) ? 255 : 0;
        // Shoulder buttons stand in for the original Black / White buttons
        g->bAnalogButtons[AB_WHITE] = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
        g->bAnalogButtons[AB_BLACK] = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
        g->bAnalogButtons[AB_LT] = s.Gamepad.bLeftTrigger;
        g->bAnalogButtons[AB_RT] = s.Gamepad.bRightTrigger;
        g->sThumbLX = s.Gamepad.sThumbLX;
        g->sThumbLY = s.Gamepad.sThumbLY;
        g->sThumbRX = s.Gamepad.sThumbRX;
        g->sThumbRY = s.Gamepad.sThumbRY;
    }
    if (port == 0) {
        apply_script(g);
        // Keyboard fallback for player 1
        if (key(VK_UP)) g->wButtons |= XB_DPAD_UP;
        if (key(VK_DOWN)) g->wButtons |= XB_DPAD_DOWN;
        if (key(VK_LEFT)) g->wButtons |= XB_DPAD_LEFT;
        if (key(VK_RIGHT)) g->wButtons |= XB_DPAD_RIGHT;
        AcquireSRWLockExclusive(&g_desktop_lock);
        static DesktopContextKey enter, escape, left, right, middle, defend, evade;
        static unsigned focus_epoch;
        unsigned epoch = g_focus_epoch.load();
        if (focus_epoch != epoch) {
            focus_epoch = epoch;
            enter.disarm(); escape.disarm(); left.disarm(); right.disarm();
            middle.disarm(); defend.disarm(); evade.disarm();
        }
        DesktopContext context = input_context();
        if (g_window_focused.load()) {
            // Alt+Enter belongs to the window. Keep Return blocked until it is
            // released, even if Alt is released first after toggling fullscreen.
            if (key(VK_RETURN) && key(VK_MENU)) enter.disarm();
            if (enter.update(key(VK_RETURN), context)) {
                if (context != DesktopContext::Gameplay) g->bAnalogButtons[AB_A] = 255;
                if (context != DesktopContext::Menu) g->wButtons |= XB_START;
            }
            if (escape.update(key(VK_ESCAPE), context)) {
                if (context == DesktopContext::Gameplay) g->wButtons |= XB_START;
                else g->bAnalogButtons[AB_B] = 255;
            }
            bool mouse_client = g_mouse_client.load();
            if (left.update(key(VK_LBUTTON) && mouse_client, context)) g->bAnalogButtons[AB_X] = 255;
            if (right.update(key(VK_RBUTTON) && mouse_client, context)) g->bAnalogButtons[AB_Y] = 255;
            if (middle.update(key(VK_MBUTTON) && mouse_client, context)) g->bAnalogButtons[AB_RT] = 255;
            if (defend.update(key(VK_LCONTROL), context)) g->bAnalogButtons[AB_LT] = 255;
            if (evade.update(key(VK_LSHIFT), context)) g->bAnalogButtons[AB_B] = 255;
        }
        ReleaseSRWLockExclusive(&g_desktop_lock);
        if (key('P')) g->wButtons |= XB_START;
        if (key(VK_BACK)) g->wButtons |= XB_BACK;
        if (key('W')) g->sThumbLY = 32767;
        if (key('S')) g->sThumbLY = -32768;
        if (key('A')) g->sThumbLX = -32768;
        if (key('D')) g->sThumbLX = 32767;
        if (key('I')) g->sThumbRY = 32767;
        if (key('K')) g->sThumbRY = -32768;
        if (key('J')) g->sThumbRX = -32768;
        if (key('L')) g->sThumbRX = 32767;
        if (key(VK_SPACE)) g->bAnalogButtons[AB_A] = 255;
        if (key('E')) g->bAnalogButtons[AB_B] = 255;
        if (key('Q')) g->bAnalogButtons[AB_X] = 255;
        if (key('R')) g->bAnalogButtons[AB_Y] = 255;
        if (key('Z')) g->bAnalogButtons[AB_WHITE] = 255;
        if (key('C')) g->bAnalogButtons[AB_BLACK] = 255;
        if (key(VK_RSHIFT) || key('F')) g->bAnalogButtons[AB_RT] = 255;
        if (context == DesktopContext::Gameplay && g_window_focused.load()) {
            AcquireSRWLockExclusive(&g_mouse_lock);
            g_mouse.sample(GetTickCount(), g_mouse_sensitivity);
            if (std::abs((int)g_mouse.x) > std::abs((int)g->sThumbRX)) g->sThumbRX = g_mouse.x;
            if (std::abs((int)g_mouse.y) > std::abs((int)g->sThumbRY)) g->sThumbRY = g_mouse.y;
            ReleaseSRWLockExclusive(&g_mouse_lock);
        }
        static bool trace = getenv("XB_INPUT_TRACE") != nullptr;
        static X_XINPUT_GAMEPAD last{};
        if (trace && memcmp(&last, g, sizeof last)) {
            logf("desktop pad: context %d buttons %04x A %u B %u X %u Y %u LT %u RT %u RX %d RY %d",
                (int)context, g->wButtons, g->bAnalogButtons[AB_A], g->bAnalogButtons[AB_B],
                g->bAnalogButtons[AB_X], g->bAnalogButtons[AB_Y], g->bAnalogButtons[AB_LT],
                g->bAnalogButtons[AB_RT], g->sThumbRX, g->sThumbRY);
            last = *g;
        }
    }
}

static bool pad_connected(int port) {
    if (port == 0) return true; // keyboard always provides player 1
    XINPUT_STATE s;
    return pXInputGetState && pXInputGetState(port, &s) == ERROR_SUCCESS;
}

static uint32_t connected_mask() {
    uint32_t m = 0;
    for (int i = 0; i < 4; i++)
        if (pad_connected(i)) m |= 1u << i;
    return m;
}

static XSTDCALL void hle_XInitDevices(uint32_t count, uint32_t types) { logf("XInitDevices(%u)", count); }

static XSTDCALL uint32_t hle_XGetDevices(uint32_t type) {
    return type == SYM_XAPILIB_g_DeviceType_Gamepad ? connected_mask() : 0;
}

static XSTDCALL uint32_t hle_XGetDeviceChanges(uint32_t type, uint32_t *ins, uint32_t *rem) {
    *ins = *rem = 0;
    if (type != SYM_XAPILIB_g_DeviceType_Gamepad) return 0;
    static uint32_t last = 0;
    uint32_t now = connected_mask();
    if (!g_reported_insert) {
        g_reported_insert = true;
        last = 0;
    }
    *ins = now & ~last;
    *rem = last & ~now;
    last = now;
    return (*ins | *rem) != 0;
}

static XSTDCALL uint32_t hle_XInputOpen(uint32_t type, uint32_t port, uint32_t slot, uint32_t params) {
    if (type != SYM_XAPILIB_g_DeviceType_Gamepad || port > 3) return 0;
    logf("XInputOpen(port %u)", port);
    return kPadHandleBase + port;
}

static XSTDCALL void hle_XInputClose(uint32_t h) {}

static XSTDCALL uint32_t hle_XInputGetCapabilities(uint32_t h, X_XINPUT_CAPABILITIES *caps) {
    if ((h & ~3u) != kPadHandleBase) return ERROR_DEVICE_NOT_CONNECTED;
    memset(caps, 0, sizeof *caps);
    caps->SubType = 1; // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    caps->In.wButtons = 0xFF;
    memset(caps->In.bAnalogButtons, 0xFF, 8);
    caps->In.sThumbLX = caps->In.sThumbLY = caps->In.sThumbRX = caps->In.sThumbRY = (int16_t)0xFFFF;
    caps->OutLeft = caps->OutRight = 0xFFFF;
    return ERROR_SUCCESS;
}

static XSTDCALL uint32_t hle_XInputGetState(uint32_t h, X_XINPUT_STATE *st) {
    if ((h & ~3u) != kPadHandleBase) return ERROR_DEVICE_NOT_CONNECTED;
    int port = h & 3;
    if (!pad_connected(port)) return ERROR_DEVICE_NOT_CONNECTED;
    X_XINPUT_GAMEPAD g;
    read_pad(port, &g);
    if (memcmp(&g, &st->Gamepad, sizeof g)) g_packet[port]++;
    st->dwPacketNumber = g_packet[port];
    st->Gamepad = g;
    return ERROR_SUCCESS;
}

static XSTDCALL uint32_t hle_XInputSetState(uint32_t h, X_XINPUT_FEEDBACK *fb) {
    if ((h & ~3u) != kPadHandleBase) return ERROR_DEVICE_NOT_CONNECTED;
    if (pXInputSetState) {
        XINPUT_VIBRATION v = {fb->wLeftMotorSpeed, fb->wRightMotorSpeed};
        pXInputSetState(h & 3, &v);
    }
    fb->dwStatus = ERROR_SUCCESS;
    if (fb->hEvent) SetEvent(xptr<void>(fb->hEvent));
    return ERROR_IO_PENDING;
}

void hle_input_install() {
    const uint8_t app_getter[] = {0xa1,0x4c,0x22,0x36,0x00,0x8b,0x88,0xe4,0x05,0x00,0x00};
    const uint8_t ui_getter[] = {0xa1,0x4c,0x22,0x36,0x00,0x8b,0x88,0x04,0x02,0x00,0x00};
    g_context_supported = !memcmp(xptr<void>(0x99950), app_getter, sizeof app_getter) &&
                          !memcmp(xptr<void>(0x95a10), ui_getter, sizeof ui_getter);
    logf("desktop input: menu-state signatures %s; mouse sensitivity %.2f", g_context_supported ? "verified" : "unsupported (capture disabled)", g_mouse_sensitivity);
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    if (x) {
        pXInputGetState = (XInputGetState_t)GetProcAddress(x, "XInputGetState");
        pXInputSetState = (XInputSetState_t)GetProcAddress(x, "XInputSetState");
    }
    HOOK(SYM_XAPILIB_XInitDevices, hle_XInitDevices);
    HOOK(SYM_XAPILIB_XGetDevices, hle_XGetDevices);
    HOOK(SYM_XAPILIB_XGetDeviceChanges, hle_XGetDeviceChanges);
    HOOK(SYM_XAPILIB_XInputOpen, hle_XInputOpen);
    HOOK(SYM_XAPILIB_XInputClose, hle_XInputClose);
    HOOK(SYM_XAPILIB_XInputGetCapabilities, hle_XInputGetCapabilities);
    HOOK(SYM_XAPILIB_XInputGetState, hle_XInputGetState);
    HOOK(SYM_XAPILIB_XInputSetState, hle_XInputSetState);
}
