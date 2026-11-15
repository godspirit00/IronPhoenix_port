#pragma once
#include "../cpu/patch.h"
#include <windows.h>

// Callers supply verified whole instructions containing no relative operands.
// Trampolines live for the process lifetime, like the installed guest hooks.
inline void *hle_d3d8_trampoline(uint32_t address, const uint8_t *expected,
                                 size_t size, const void *replacement, const char *name) {
    if (memcmp(xptr<void>(address), expected, size)) fatal("HLE %s: unsupported prologue", name);
    auto *code = static_cast<uint8_t *>(VirtualAlloc(nullptr, size + 5,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!code) fatal("HLE %s: trampoline allocation failed", name);
    memcpy(code, expected, size);
    code[size] = 0xe9;
    int32_t delta = int32_t(address + size - xaddr(code + size + 5));
    memcpy(code + size + 1, &delta, 4);
    FlushInstructionCache(GetCurrentProcess(), code, size + 5);
    hook_function(address, replacement, name);
    return code;
}
