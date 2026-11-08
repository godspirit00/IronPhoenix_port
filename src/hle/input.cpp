// XAPI input (XInitDevices / XGetDevices / XInput*) on top of host XInput + keyboard
#include "hle.h"
#include "../kernel/kernel.h"
#include "../cpu/patch.h"
#include "../symbols.gen.h"
#include <windows.h>
#include <xinput.h>
#include <stdlib.h>

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

bool g_window_focused = true;

static bool key(int vk) { return g_window_focused && (GetAsyncKeyState(vk) & 0x8000); }

// Debug/test: scripted input "--input 120:start,125.5:a" (seconds since start : button)
struct ScriptedPress {
    uint32_t at_ms;
    char button[16];
};
static ScriptedPress g_script[64];
static int g_nscript;
static DWORD g_script_t0;

void hle_input_set_script(const char *spec) {
    g_script_t0 = GetTickCount();
    const char *p = spec;
    while (*p && g_nscript < 64) {
        double t = strtod(p, (char **)&p);
        if (*p != ':') break;
        p++;
        int n = 0;
        while (*p && *p != ',' && n < 15) g_script[g_nscript].button[n++] = *p++;
        g_script[g_nscript].button[n] = 0;
        g_script[g_nscript].at_ms = (uint32_t)(t * 1000);
        g_nscript++;
        if (*p == ',') p++;
    }
    logf("input script: %d presses", g_nscript);
}

static void apply_script(X_XINPUT_GAMEPAD *g) {
    uint32_t now = GetTickCount() - g_script_t0;
    for (int i = 0; i < g_nscript; i++) {
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
        if (key(VK_RETURN)) g->wButtons |= XB_START;
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
        if (key(VK_LSHIFT)) g->bAnalogButtons[AB_LT] = 255;
        if (key(VK_RSHIFT) || key('F')) g->bAnalogButtons[AB_RT] = 255;
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
