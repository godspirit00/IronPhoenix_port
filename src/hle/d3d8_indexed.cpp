// Game-specific indexed drawing / reusable pushbuffer coverage audit.
#include "hle.h"
#include "d3d8_hook.h"
#include <atomic>
#include <stdlib.h>

namespace {
using Indexed = uint32_t (__stdcall *)(uint32_t, uint32_t, const uint16_t *);
using Run = uint32_t (__stdcall *)(void *, void *);
Indexed original_indexed;
Run original_run;
bool enabled;
std::atomic<uint32_t> indexed_calls, run_calls;
std::atomic<uint64_t> index_count;
uint32_t __stdcall indexed(uint32_t primitive, uint32_t count, const uint16_t *indices) {
    ++indexed_calls; index_count += count;
    return original_indexed(primitive, count, indices);
}
uint32_t __stdcall run(void *pushbuffer, void *fixup) {
    ++run_calls;
    return original_run(pushbuffer, fixup);
}
}
extern "C" void hle_d3d8_indexed_stats(void) {
    if (enabled) logf("HLE indexed audit: indexed_calls %u indices %llu run_calls %u",
        indexed_calls.load(), static_cast<unsigned long long>(index_count.load()), run_calls.load());
}
void hle_d3d8_indexed_install() {
    const char *mode = getenv("XB_HLE_D3D8_INDEXED");
    if (!mode || !*mode || !strcmp(mode, "off")) return;
    if (strcmp(mode, "observe")) fatal("XB_HLE_D3D8_INDEXED must be observe");
    const uint8_t draw[] = {0x55,0x8b,0xec,0x83,0xec,0x08};
    const uint8_t push[] = {0x83,0xec,0x2c,0x53,0x55,0x56};
    original_indexed = reinterpret_cast<Indexed>(hle_d3d8_trampoline(0x22c210,
        draw, sizeof draw, reinterpret_cast<void *>(indexed), "DrawIndexedVertices"));
    original_run = reinterpret_cast<Run>(hle_d3d8_trampoline(0x232cf0,
        push, sizeof push, reinterpret_cast<void *>(run), "RunPushBuffer"));
    enabled = true;
    logf("HLE indexed mode: observe");
}
