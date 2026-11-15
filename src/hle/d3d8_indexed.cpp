// Game-specific indexed drawing / reusable pushbuffer coverage audit.
#include "hle.h"
#include "d3d8_hook.h"
#include <atomic>
#include <stdlib.h>

extern "C" uint32_t hle_d3d8_queue_indexed(uint32_t, uint32_t, const void *, bool, uint32_t *);
extern "C" void hle_d3d8_emit_marker(void *, uint32_t);
extern "C" void hle_d3d8_prepare_vertex(void *, uint32_t, uint32_t);

namespace {
using Indexed = uint32_t (__stdcall *)(uint32_t, uint32_t, const uint16_t *);
using Run = uint32_t (__stdcall *)(void *, void *);
Indexed original_indexed;
Run original_run;
bool enabled;
enum Mode { Observe, Verify, Direct } packet_mode;
std::atomic<uint32_t> indexed_calls, run_calls;
std::atomic<uint64_t> index_count;
std::atomic<uint32_t> submitted, consumed, fallback, verified;
std::atomic<uint64_t> consumed_indices, verified_indices;
uint32_t& field(void *device, uint32_t offset) {
    return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(device) + offset);
}
uint32_t __stdcall indexed(uint32_t primitive, uint32_t count, const uint16_t *indices) {
    ++indexed_calls; index_count += count;
    if (packet_mode == Observe) return original_indexed(primitive, count, indices);
    void *device = *xptr<void *>(0x239198);
    if (primitive < 1 || primitive > 10 || !count || count > 65536 ||
        !indices || (field(device, 8) & 0x84)) {
        ++fallback; return original_indexed(primitive, count, indices);
    }
    // Reserve before committing native state: capacity fallback must leave
    // the original helper's dirty flags and cached base index untouched.
    uint32_t end = 0;
    uint32_t token = hle_d3d8_queue_indexed(primitive, count, indices, packet_mode == Verify, &end);
    if (!token) { ++fallback; return original_indexed(primitive, count, indices); }
    hle_d3d8_prepare_vertex(device, field(device, 0x1c), token);
    ++submitted;
    if (packet_mode == Verify) {
        hle_d3d8_emit_marker(device, token);
        uint32_t result = original_indexed(primitive, count, indices);
        hle_d3d8_emit_marker(device, end);
        return result;
    }
    field(device, 8) |= 0x800;
    hle_d3d8_emit_marker(device, token);
    uint32_t flags = field(device, 8);
    field(device, 8) = flags & ~0x1800u;
    if (flags & 0x1000) {
        using Kick = void (__thiscall *)(void *, uint32_t);
        reinterpret_cast<Kick>(0x22ce60)(device, 1);
    }
    // The XDK drawing API is void; observe/verify preserve EAX for diagnostics.
    return 0;
}
uint32_t __stdcall run(void *pushbuffer, void *fixup) {
    ++run_calls;
    return original_run(pushbuffer, fixup);
}
}
extern "C" void hle_d3d8_indexed_stats(void) {
    if (!enabled) return;
    logf("HLE indexed audit: indexed_calls %u indices %llu run_calls %u",
        indexed_calls.load(), static_cast<unsigned long long>(index_count.load()), run_calls.load());
    logf("HLE indexed packets: submitted %u consumed %u fallback %u verified %u consumed_indices %llu verified_indices %llu",
        submitted.load(), consumed.load(), fallback.load(), verified.load(),
        static_cast<unsigned long long>(consumed_indices.load()),
        static_cast<unsigned long long>(verified_indices.load()));
}
extern "C" void hle_d3d8_indexed_complete(bool checking, uint32_t count) {
    if (checking) {
        uint32_t n = ++verified; verified_indices += count;
        if (n <= 32) logf("HLE indexed verification %u: %u indices MATCH", n, count);
    } else { ++consumed; consumed_indices += count; }
}
void hle_d3d8_indexed_install() {
    const char *mode = getenv("XB_HLE_D3D8_INDEXED");
    if (!mode || !*mode || !strcmp(mode, "off")) return;
    if (!strcmp(mode, "observe")) packet_mode = Observe;
    else if (!strcmp(mode, "verify")) packet_mode = Verify;
    else if (!strcmp(mode, "1")) packet_mode = Direct;
    else fatal("XB_HLE_D3D8_INDEXED must be observe, verify or 1");
    const uint8_t state[] = {0x83,0xec,0x0c,0x53,0x8b,0x1d,0xf8,0x8c,0x23,0x00};
    const uint8_t reserve[] = {0x8b,0x0d,0x98,0x91,0x23,0x00};
    const uint8_t kick[] = {0x56,0x8b,0x35,0x98,0x91,0x23,0x00};
    if (memcmp(xptr<void>(0x232930), state, sizeof state) ||
        memcmp(xptr<void>(0x22d1e0), reserve, sizeof reserve) ||
        memcmp(xptr<void>(0x22ce60), kick, sizeof kick)) fatal("HLE indexed: unsupported XDK helpers");
    const uint8_t draw[] = {0x55,0x8b,0xec,0x83,0xec,0x08};
    const uint8_t push[] = {0x83,0xec,0x2c,0x53,0x55,0x56};
    original_indexed = reinterpret_cast<Indexed>(hle_d3d8_trampoline(0x22c210,
        draw, sizeof draw, reinterpret_cast<void *>(indexed), "DrawIndexedVertices"));
    original_run = reinterpret_cast<Run>(hle_d3d8_trampoline(0x232cf0,
        push, sizeof push, reinterpret_cast<void *>(run), "RunPushBuffer"));
    enabled = true;
    logf("HLE indexed mode: %s", mode);
}
