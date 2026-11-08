#include "patch.h"
#include "../kernel/kernel.h"
#include "../symbols.gen.h"
#include <windows.h>

void patch_bytes(uint32_t addr, const void *bytes, size_t n) {
    memcpy(xptr<void>(addr), bytes, n);
    FlushInstructionCache(GetCurrentProcess(), xptr<void>(addr), n);
}

void hook_function(uint32_t xbox_addr, const void *replacement, const char *name) {
    uint8_t jmp[5] = {0xE9};
    int32_t rel = (int32_t)(xaddr(replacement) - (xbox_addr + 5));
    memcpy(jmp + 1, &rel, 4);
    patch_bytes(xbox_addr, jmp, 5);
    LOGK("hook %08x -> %s", xbox_addr, name);
}

// ---------------------------------------------------------------------------
// fs: patching.
// Every site below was found by tools/find_fs_patches.py; we additionally verify the
// original bytes before patching. Each instruction is replaced by CALL stub + NOPs.
// Stubs read from the XThreadBlock whose pointer lives in TEB TlsSlots[g_xthread_tls_slot].
// ---------------------------------------------------------------------------
struct FsPattern {
    uint8_t bytes[8];
    uint8_t len;
    // stub body after "mov reg, fs:[slot]"
    uint8_t reg;      // 0=eax,1=ecx,7=edi (modrm reg field)
    uint8_t load[4];  // instruction loading [reg+off] into reg
    uint8_t load_len;
};

static const FsPattern kPatterns[] = {
    {{0x64, 0xA1, 0x20, 0, 0, 0}, 6, 0, {0x8B, 0x40, 0x20}, 3},             // mov eax, fs:[0x20]
    {{0x64, 0xA1, 0x28, 0, 0, 0}, 6, 0, {0x8B, 0x40, 0x28}, 3},             // mov eax, fs:[0x28]
    {{0x64, 0xA1, 0x58, 0, 0, 0}, 6, 0, {0x8B, 0x40, 0x58}, 3},             // mov eax, fs:[0x58]
    {{0x64, 0x0F, 0xB6, 0x05, 0x24, 0, 0, 0}, 8, 0, {0x0F, 0xB6, 0x40, 0x24}, 4}, // movzx eax, byte fs:[0x24]
    {{0x64, 0x8B, 0x0D, 0x04, 0, 0, 0}, 7, 1, {0x8B, 0x49, 0x04}, 3},       // mov ecx, fs:[4]
    {{0x64, 0x8B, 0x3D, 0x04, 0, 0, 0}, 7, 7, {0x8B, 0x7F, 0x04}, 3},       // mov edi, fs:[4]
};

static const uint32_t kFsSites[] = {
#include "fs_sites.gen.inc"
};

void patch_fs_accesses() {
    const int npat = sizeof kPatterns / sizeof kPatterns[0];
    uint8_t *stubs = (uint8_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    uint32_t stub_addr[npat];
    uint32_t slot_off = 0xE10 + 4 * g_xthread_tls_slot; // TEB.TlsSlots
    if (g_xthread_tls_slot >= 64) fatal("TLS slot %u not in TEB inline array", g_xthread_tls_slot);
    uint8_t *p = stubs;
    for (int i = 0; i < npat; i++) {
        stub_addr[i] = xaddr(p);
        // mov reg, fs:[slot_off]
        *p++ = 0x64;
        if (kPatterns[i].reg == 0) {
            *p++ = 0xA1;
        } else {
            *p++ = 0x8B;
            *p++ = (uint8_t)(0x05 | (kPatterns[i].reg << 3));
        }
        memcpy(p, &slot_off, 4);
        p += 4;
        memcpy(p, kPatterns[i].load, kPatterns[i].load_len);
        p += kPatterns[i].load_len;
        *p++ = 0xC3; // ret
        p = (uint8_t *)(((uintptr_t)p + 15) & ~15);
    }
    int patched = 0;
    for (uint32_t site : kFsSites) {
        uint8_t *code = xptr<uint8_t>(site);
        int which = -1;
        for (int i = 0; i < npat; i++)
            if (memcmp(code, kPatterns[i].bytes, kPatterns[i].len) == 0) which = i;
        if (which < 0) {
            logf("fs patch: unexpected bytes at %08x", site);
            continue;
        }
        uint8_t buf[8];
        memset(buf, 0x90, sizeof buf);
        buf[0] = 0xE8;
        int32_t rel = (int32_t)(stub_addr[which] - (site + 5));
        memcpy(buf + 1, &rel, 4);
        patch_bytes(site, buf, kPatterns[which].len);
        patched++;
    }
    logf("fs patch: %d sites patched", patched);
}

// ---------------------------------------------------------------------------
// Timing: the Xbox QueryPerformanceCounter is a bare RDTSC and the frequency is
// hardwired to 733333333. Feed it host QPC time scaled to that rate.
// ---------------------------------------------------------------------------
static LARGE_INTEGER g_qpf, g_qpc0;

uint64_t xbox_tsc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = (uint64_t)(now.QuadPart - g_qpc0.QuadPart);
    // d * 733333333 / freq without overflow
    uint64_t q = d / (uint64_t)g_qpf.QuadPart, r = d % (uint64_t)g_qpf.QuadPart;
    return q * 733333333ull + r * 733333333ull / (uint64_t)g_qpf.QuadPart;
}

static XSTDCALL uint32_t hle_QueryPerformanceCounter(uint64_t *out) {
    *out = xbox_tsc();
    return 1;
}

// D3D's helper: returns RDTSC in edx:eax (cdecl, no args)
static uint64_t hle_D3D_rdtsc() { return xbox_tsc(); }

void patch_timing() {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    HOOK(SYM_XAPILIB_QueryPerformanceCounter, hle_QueryPerformanceCounter);
    HOOK(0x0022FAA0, hle_D3D_rdtsc);
}
