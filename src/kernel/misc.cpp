// Rtl*, Xc* (crypto), Hal*, Av*, Ex*, Dbg*, Xe*, kernel data exports, unimplemented-call logging
#include "kernel.h"
#include "../video/video.h"
#include "../xbe.h"
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <map>
#include <string>

// --------------------------------------------------------------------------- stub logging
extern "C" void kstub_log(int ordinal, const char *name, int nargs, const uint32_t *args) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, "UNIMPLEMENTED kernel #%d %s(", ordinal, name);
    for (int i = 0; i < nargs && n < (int)sizeof buf - 16; i++) n += snprintf(buf + n, sizeof buf - n, "%s%08x", i ? ", " : "", args[i]);
    snprintf(buf + n, sizeof buf - n, ") ret=%p", __builtin_return_address(1));
    logf("%s", buf);
}

// --------------------------------------------------------------------------- critical sections (Xbox layout)
static CRITICAL_SECTION g_cs_map_lock;
static std::map<uint32_t, HANDLE> g_cs_events;

static HANDLE cs_event(X_RTL_CRITICAL_SECTION *cs) {
    EnterCriticalSection(&g_cs_map_lock);
    HANDLE &e = g_cs_events[xaddr(cs)];
    if (!e) e = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE r = e;
    LeaveCriticalSection(&g_cs_map_lock);
    return r;
}

KEXPORT void xk_RtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION *cs) {
    memset(cs, 0, sizeof *cs);
    cs->Event.Type = X_SynchronizationEvent;
    cs->Event.Size = 4;
    cs->LockCount = -1;
}

KEXPORT void xk_RtlEnterCriticalSection(X_RTL_CRITICAL_SECTION *cs) {
    uint32_t me = xthread_current()->CurrentThread;
    if (InterlockedIncrement((volatile LONG *)&cs->LockCount) == 0) {
        cs->OwningThread = me;
        cs->RecursionCount = 1;
        return;
    }
    if (cs->OwningThread == me) {
        cs->RecursionCount++;
        return;
    }
    WaitForSingleObject(cs_event(cs), INFINITE);
    cs->OwningThread = me;
    cs->RecursionCount = 1;
}

KEXPORT void xk_RtlLeaveCriticalSection(X_RTL_CRITICAL_SECTION *cs) {
    if (--cs->RecursionCount != 0) {
        InterlockedDecrement((volatile LONG *)&cs->LockCount);
        return;
    }
    cs->OwningThread = 0;
    if (InterlockedDecrement((volatile LONG *)&cs->LockCount) >= 0) SetEvent(cs_event(cs));
}

// --------------------------------------------------------------------------- crypto (SHA-1, HMAC-SHA1, RC4)
struct Sha1 {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    uint32_t n;
};
static inline uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static void sha1_block(Sha1 *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) f = (b & cc) | (~b & d), k = 0x5A827999;
        else if (i < 40) f = b ^ cc ^ d, k = 0x6ED9EBA1;
        else if (i < 60) f = (b & cc) | (b & d) | (cc & d), k = 0x8F1BBCDC;
        else f = b ^ cc ^ d, k = 0xCA62C1D6;
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d, d = cc, cc = rol(b, 30), b = a, a = t;
    }
    c->h[0] += a, c->h[1] += b, c->h[2] += cc, c->h[3] += d, c->h[4] += e;
}
static void sha1_init(Sha1 *c) {
    c->h[0] = 0x67452301, c->h[1] = 0xEFCDAB89, c->h[2] = 0x98BADCFE, c->h[3] = 0x10325476, c->h[4] = 0xC3D2E1F0;
    c->len = 0, c->n = 0;
}
static void sha1_update(Sha1 *c, const uint8_t *p, uint32_t n) {
    c->len += n;
    while (n) {
        uint32_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k);
        c->n += k, p += k, n -= k;
        if (c->n == 64) sha1_block(c, c->buf), c->n = 0;
    }
}
static void sha1_final(Sha1 *c, uint8_t *out) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_update(c, &pad, 1);
    while (c->n != 56) sha1_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_update(c, lb, 8);
    for (int i = 0; i < 5; i++) out[4 * i] = c->h[i] >> 24, out[4 * i + 1] = c->h[i] >> 16, out[4 * i + 2] = c->h[i] >> 8, out[4 * i + 3] = c->h[i];
}
static_assert(sizeof(Sha1) <= 116, "XcSHA context is 116 bytes");

KEXPORT void xk_XcSHAInit(uint8_t *ctx) { sha1_init((Sha1 *)ctx); }
KEXPORT void xk_XcSHAUpdate(uint8_t *ctx, uint8_t *in, uint32_t n) { sha1_update((Sha1 *)ctx, in, n); }
KEXPORT void xk_XcSHAFinal(uint8_t *ctx, uint8_t *digest) { sha1_final((Sha1 *)ctx, digest); }

KEXPORT void xk_XcHMAC(uint8_t *key, uint32_t keylen, uint8_t *d1, uint32_t n1, uint8_t *d2, uint32_t n2, uint8_t *out) {
    uint8_t k[64] = {0}, ipad[64], opad[64], inner[20];
    if (keylen > 64) {
        Sha1 c;
        sha1_init(&c);
        sha1_update(&c, key, keylen);
        sha1_final(&c, k);
    } else {
        memcpy(k, key, keylen);
    }
    for (int i = 0; i < 64; i++) ipad[i] = k[i] ^ 0x36, opad[i] = k[i] ^ 0x5C;
    Sha1 c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    if (d1) sha1_update(&c, d1, n1);
    if (d2) sha1_update(&c, d2, n2);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

KEXPORT void xk_XcRC4Key(uint8_t *ks, uint32_t keylen, uint8_t *key) {
    uint8_t *S = ks;
    for (int i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + S[i] + key[i % keylen]) & 255;
        uint8_t t = S[i];
        S[i] = S[j], S[j] = t;
    }
    ks[256] = ks[257] = 0;
}
KEXPORT void xk_XcRC4Crypt(uint8_t *ks, uint32_t n, uint8_t *data) {
    uint8_t *S = ks, i = ks[256], j = ks[257];
    for (uint32_t k = 0; k < n; k++) {
        i++;
        j += S[i];
        uint8_t t = S[i];
        S[i] = S[j], S[j] = t;
        data[k] ^= S[(uint8_t)(S[i] + S[j])];
    }
    ks[256] = i, ks[257] = j;
}

// --------------------------------------------------------------------------- HAL / AV
KEXPORT void xk_HalReturnToFirmware(uint32_t Routine) {
    logf("HalReturnToFirmware(%u) - game exited", Routine);
    log_backtrace("HalReturnToFirmware");
    ExitProcess(0);
}
KEXPORT void xk_HalInitiateShutdown() {
    logf("HalInitiateShutdown");
    ExitProcess(0);
}
KEXPORT uint32_t xk_HalIsResetOrShutdownPending() { return 0; }
KEXPORT void xk_HalRegisterShutdownNotification(uint32_t Reg, uint32_t Register) {}
KEXPORT void xk_HalReadWritePCISpace(uint32_t Bus, uint32_t Slot, uint32_t Reg, uint8_t *Buffer, uint32_t Length, uint32_t Write) {
    if (!(Write & 0xFF)) memset(Buffer, 0, Length);
}

#define AV_PACK_HDTV 0x00000004
#define AV_STANDARD_NTSC_M 0x00000100
#define AV_FLAGS_60Hz 0x00400000
#define AV_USER_FLAGS ((g_video.aspect > 1.5f ? 0x00010000 : 0) /*widescreen*/ | 0x00080000 /*480p*/ | 0x00020000 /*720p*/ | 0x00040000 /*1080i*/)

KEXPORT void xk_AvSendTVEncoderOption(uint32_t RegisterBase, uint32_t Option, uint32_t Param, uint32_t *Result) {
    LOGK("AvSendTVEncoderOption(%u, %x)", Option, Param);
    if (!Result) return;
    switch (Option) {
    case 6: *Result = AV_PACK_HDTV | AV_STANDARD_NTSC_M | AV_FLAGS_60Hz | AV_USER_FLAGS; break; // AV_QUERY_AV_CAPABILITIES
    default: *Result = 0; break;
    }
}
extern "C" void nv2a_av_set_display_mode(uint32_t framebuffer, uint32_t pitch, uint32_t format, int width, int height);

// AV mode word: byte 1 indexes the kernel's CRTC timing table. Resolutions per index
// (derived from the AV_MODE_* list: 640x480 / 720x480 / 640x576 / 720x576 / 720p / 1080i).
static void av_mode_resolution(uint32_t mode, int *w, int *h) {
    switch ((mode >> 8) & 0xFF) {
    case 2: case 4: case 8: *w = 720; *h = 480; break;
    case 5: case 16: *w = 640; *h = 576; break;
    case 6: *w = 720; *h = 576; break;
    case 10: *w = 1280; *h = 720; break;
    case 12: *w = 1920; *h = 1080; break;
    default: *w = 640; *h = 480; break;
    }
}
KEXPORT uint32_t xk_AvSetDisplayMode(uint32_t RegisterBase, uint32_t Step, uint32_t Mode, uint32_t Format, uint32_t Pitch, uint32_t FrameBuffer) {
    logf("AvSetDisplayMode(step %u, mode %08x, fmt %x, pitch %u, fb %08x)", Step, Mode, Format, Pitch, FrameBuffer);
    int w, h;
    av_mode_resolution(Mode, &w, &h);
    nv2a_av_set_display_mode(FrameBuffer, Pitch, Format, w, h);
    return 0;
}
static uint32_t g_av_saved_data;
KEXPORT uint32_t xk_AvGetSavedDataAddress() { return g_av_saved_data; }
KEXPORT void xk_AvSetSavedDataAddress(uint32_t Address) { g_av_saved_data = Address; }

// --------------------------------------------------------------------------- EEPROM settings
KEXPORT uint32_t xk_ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t *Type, uint8_t *Value, uint32_t ValueLength, uint32_t *ResultLength) {
    uint32_t v = 0;
    switch (ValueIndex) {
    case 0x7: v = 1; break;                                       // XC_LANGUAGE: English
    case 0x8: v = AV_USER_FLAGS; break;                           // XC_VIDEO
    case 0x9: v = 0; break;                                       // XC_AUDIO: stereo (AC-3 encoder needs EP ROM tables nobody emulates)
    case 0xA: v = 0; break;                                       // XC_P_CONTROL_GAMES
    case 0xB: v = 0; break;                                       // XC_P_CONTROL_PASSWORD
    case 0xC: v = 0; break;                                       // XC_P_CONTROL_MOVIES
    case 0x2: v = 0; break;                                       // XC_TIMEZONE_BIAS
    case 0x103: v = AV_STANDARD_NTSC_M | AV_FLAGS_60Hz; break;    // XC_FACTORY_AV_REGION
    case 0x104: v = 0; break;                                     // XC_FACTORY_GAME_REGION
    case 0x105: v = 1; break;                                     // XC_FACTORY_DVD_REGION
    case 0x101: v = 0; break;                                     // XC_FACTORY_SERIAL_NUMBER (partial)
    case 0xFFFF: // XC_MAX_ALL - full EEPROM image
        if (Value) memset(Value, 0, ValueLength);
        if (Type) *Type = 3;
        if (ResultLength) *ResultLength = ValueLength;
        return X_STATUS_SUCCESS;
    default:
        logf("ExQueryNonVolatileSetting(%x) unknown -> 0", ValueIndex);
        break;
    }
    if (Type) *Type = 4; // REG_DWORD
    if (Value && ValueLength >= 4) memcpy(Value, &v, 4);
    if (ResultLength) *ResultLength = 4;
    return X_STATUS_SUCCESS;
}

// --------------------------------------------------------------------------- misc
extern "C" __attribute__((cdecl)) uint32_t xk_DbgPrint(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    logf("DbgPrint: %s", buf);
    return 0;
}

KEXPORT uint32_t xk_XeLoadSection(XbeSectionHeader *Section) {
    Section->SectionReferenceCount++;
    LOGK("XeLoadSection(%s) refs=%u", xbe_section_name(Section), Section->SectionReferenceCount);
    return X_STATUS_SUCCESS;
}
KEXPORT uint32_t xk_XeUnloadSection(XbeSectionHeader *Section) {
    if (Section->SectionReferenceCount) Section->SectionReferenceCount--;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_PhyGetLinkState(uint32_t Mode) { return 0; }
KEXPORT uint32_t xk_PhyInitialize(uint32_t forceReset, uint32_t p2) { return X_STATUS_SUCCESS; }
KEXPORT uint32_t xk_FscSetCacheSize(uint32_t pages) { return X_STATUS_SUCCESS; }
KEXPORT uint32_t xk_NtSetSystemTime(int64_t *NewTime, int64_t *OldTime) {
    if (OldTime) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        *OldTime = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    return X_STATUS_SUCCESS; // never touch the host clock
}

// --------------------------------------------------------------------------- data exports
extern "C" {
volatile uint32_t xv_KeTickCount[1];
uint8_t xv_ExEventObjectType[64];
uint8_t xv_IoFileObjectType[64];
uint32_t xv_LaunchDataPage[1] = {0};
uint32_t xv_HalDiskCachePartitionCount[1] = {3};
uint8_t xv_XboxHDKey[16];
uint8_t xv_XboxSignatureKey[16];
uint8_t xv_XboxAlternateSignatureKeys[16 * 16];
uint8_t xv_XboxLANKey[16];
uint8_t xv_XePublicKeyData[284];
uint8_t xv_IdexChannelObject[256];
uint32_t xv_HalBootSMCVideoMode[1] = {0x01}; // HDTV pack
// XBOX_HARDWARE_INFO { ULONG Flags; UCHAR GpuRevision, McpRevision, unknown3, unknown4; }
uint32_t xv_XboxHardwareInfo[2] = {0x00000020, 0x0000D4A2};
// XBOX_KRNL_VERSION {Major, Minor, Build, Qfe}
uint16_t xv_XboxKrnlVersion[4] = {1, 0, 5838, 1};
static char s_image_name[] = "\\Device\\CdRom0\\default.xbe";
X_ANSI_STRING xv_XeImageFileName[1] = {{sizeof(s_image_name) - 1, sizeof(s_image_name), 0}};
static char s_disk_model[] = "IronPhoenix Virtual HDD";
static char s_disk_serial[] = "0000000000000001";
X_ANSI_STRING s_model = {sizeof(s_disk_model) - 1, sizeof(s_disk_model), 0};
X_ANSI_STRING s_serial = {sizeof(s_disk_serial) - 1, sizeof(s_disk_serial), 0};
uint32_t xv_HalDiskModelNumber[1];
uint32_t xv_HalDiskSerialNumber[1];
}

// Kernel functions that are binary compatible with the Windows ntdll versions
static const char *kNtdllForwards[] = {
    "RtlUnwind", "RtlRaiseException", "RtlNtStatusToDosError", "RtlInitAnsiString", "RtlCompareString",
    "RtlEqualString", "RtlCompareMemoryUlong", "RtlTimeToTimeFields", "RtlTimeFieldsToTime",
    "RtlAnsiStringToUnicodeString", "RtlUnicodeStringToAnsiString", nullptr,
};

void misc_init() {
    InitializeCriticalSection(&g_cs_map_lock);
    xv_XeImageFileName[0].Buffer = xaddr(s_image_name);
    s_model.Buffer = xaddr(s_disk_model);
    s_serial.Buffer = xaddr(s_disk_serial);
    xv_HalDiskModelNumber[0] = xaddr(&s_model);
    xv_HalDiskSerialNumber[0] = xaddr(&s_serial);
}

// Called by the import resolver for names we route straight to ntdll
void *misc_ntdll_forward(const char *name) {
    for (const char **p = kNtdllForwards; *p; p++)
        if (!strcmp(*p, name)) return (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), name);
    return nullptr;
}
