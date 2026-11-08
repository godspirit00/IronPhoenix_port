#pragma once
#include "../common.h"

// Redirect an Xbox function to a host replacement (writes JMP rel32 at its entry).
// The replacement must use the same calling convention as the original.
void hook_function(uint32_t xbox_addr, const void *replacement, const char *name);
#define HOOK(addr, fn) hook_function((addr), (const void *)(fn), #fn)

// Write raw bytes into Xbox code.
void patch_bytes(uint32_t addr, const void *bytes, size_t n);

// Rewrite the Xbox-specific fs:[...] reads (KPCR/KPRCB/TLS) into calls to stubs.
void patch_fs_accesses();

// Install hooks for timing primitives (rdtsc users).
void patch_timing();

// Host time scaled to the Xbox CPU clock (733.33 MHz) - the game's QueryPerformanceFrequency.
uint64_t xbox_tsc();
