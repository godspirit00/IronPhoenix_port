// Experimental, game-specific high-level DrawVerticesUP submission bridge.
// State/shaders/resources still use the existing frontend in this milestone.
#include "hle.h"
#include "../cpu/patch.h"
#include "d3d8_up_layout.h"
#include <windows.h>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <stdlib.h>

namespace {
constexpr uint32_t device_ptr = 0x239198, draw_up = 0x22bec0;
constexpr uint32_t marker_tag = 0xd8000000;
enum Kind { Draw, VerifyBegin, VerifyEnd };
struct Packet {
    Kind kind;
    uint32_t primitive, vertices;
    std::vector<uint32_t> words;
};
using Original = void (__stdcall *)(uint32_t, uint32_t, const void *, uint32_t);
Original original;
bool enabled, verify;
SRWLOCK packet_lock = SRWLOCK_INIT;
std::unordered_map<uint32_t, std::unique_ptr<Packet>> packets;
size_t queued_bytes;
uint32_t next_id;
std::atomic<uint32_t> calls, submitted, consumed, fallback, checked;
std::unique_ptr<Packet> verifying; // only accessed by the graphics worker
size_t verify_offset;

uint32_t& field(void *device, uint32_t offset) {
    return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(device) + offset);
}
uint32_t publish(std::unique_ptr<Packet> packet) {
    AcquireSRWLockExclusive(&packet_lock);
    size_t bytes = packet->words.size() * 4;
    if (packets.size() >= 128 || queued_bytes + bytes > 16 * 1024 * 1024) {
        ReleaseSRWLockExclusive(&packet_lock);
        return 0;
    }
    uint32_t token;
    do { token = marker_tag | (++next_id & 0xffffff); } while (packets.count(token));
    queued_bytes += bytes;
    packets.emplace(token, std::move(packet));
    ReleaseSRWLockExclusive(&packet_lock);
    return token;
}
void emit(void *device, uint32_t token) {
    using Reserve = uint32_t *(__stdcall *)(uint32_t);
    uint32_t *dst = reinterpret_cast<Reserve>(0x22d1e0)(2);
    dst[0] = 0x00040100; // one parameter, subchannel 0, NV097_NO_OPERATION
    dst[1] = token;
    field(device, 0) = xaddr(dst + 2);
}
void __stdcall replacement(uint32_t primitive, uint32_t vertices,
                           const void *source, uint32_t stride) {
    ++calls;
    if (primitive < 1 || primitive > 10 || !vertices) {
        ++fallback; original(primitive, vertices, source, stride); return;
    }
    void *device = *xptr<void *>(device_ptr);
    // Preserve original deferred state/declaration setup and its side effects.
    using SetStateUP = void (__thiscall *)(void *);
    reinterpret_cast<SetStateUP>(0x232ad0)(device);
    uint32_t num_runs = field(device, 0xc54);
    auto packet = std::make_unique<Packet>();
    packet->kind = verify ? VerifyBegin : Draw;
    packet->primitive = primitive; packet->vertices = vertices;
    std::vector<HleUpRun> runs;
    if (num_runs && num_runs <= 16) {
        for (uint32_t i = 0; i < num_runs; ++i) {
            runs.push_back({field(device, 0xbd4 + i * 8),
                i + 1 == num_runs ? int32_t(field(device, 0xbd0) + stride)
                                 : int32_t(field(device, 0xbd8 + i * 8))});
        }
    }
    if (!hle_up_pack(source, vertices, stride, field(device, 0xbcc),
                     field(device, 0xbc8), runs, packet->words)) {
        ++fallback; original(primitive, vertices, source, stride); return;
    }
    uint32_t token = publish(std::move(packet));
    if (!token) { ++fallback; original(primitive, vertices, source, stride); return; }
    if (verify) {
        // Reserve the end marker before publishing either command. Queue
        // exhaustion must never leave a validation scope open on the worker.
        auto end = std::make_unique<Packet>(); end->kind = VerifyEnd;
        uint32_t end_token = publish(std::move(end));
        if (!end_token) fatal("HLE UP: verification queue exhausted");
        emit(device, token);
        original(primitive, vertices, source, stride);
        emit(device, end_token);
    } else {
        // Match DrawVerticesUP's descriptor/flag mutations and optional kick.
        field(device, 0xbd0 + num_runs * 8) = field(device, 0xbd0) + stride;
        field(device, 8) |= 0x800;
        emit(device, token);
        uint32_t flags = field(device, 8);
        field(device, 8) = flags & ~0x1800u;
        if (flags & 0x1000) {
            using Kick = void (__thiscall *)(void *, uint32_t);
            reinterpret_cast<Kick>(0x22ce60)(device, 1);
        }
    }
    ++submitted;
}
}

extern "C" bool g_hle_up_verify_active = false;
extern "C" bool nv2a_hle_draw_up(void *, uint32_t, const uint32_t *, size_t);

extern "C" bool hle_d3d8_consume_up(void *device, uint32_t token) {
    if ((token & 0xff000000) != marker_tag || !enabled) return false;
    AcquireSRWLockExclusive(&packet_lock);
    auto it = packets.find(token);
    if (it == packets.end()) { ReleaseSRWLockExclusive(&packet_lock); return false; }
    auto packet = std::move(it->second); packets.erase(it);
    queued_bytes -= packet->words.size() * 4;
    ReleaseSRWLockExclusive(&packet_lock);
    if (packet->kind == Draw) {
        if (!nv2a_hle_draw_up(device, packet->primitive, packet->words.data(), packet->words.size()))
            fatal("HLE UP: invalid graphics-worker draw state");
        ++consumed;
    } else if (packet->kind == VerifyBegin) {
        if (verifying) fatal("HLE UP: nested verification");
        verifying = std::move(packet); verify_offset = 0; g_hle_up_verify_active = true;
    } else {
        if (!verifying || verify_offset != verifying->words.size())
            fatal("HLE UP: verification length MISMATCH");
        uint32_t n = ++checked;
        if (n <= 32) logf("HLE UP verification %u: %u vertices, %zu DWORDs MATCH", n,
                          verifying->vertices, verifying->words.size());
        verifying.reset(); g_hle_up_verify_active = false;
    }
    return true;
}
extern "C" void hle_d3d8_verify_up_word(uint32_t word) {
    if (!verifying || verify_offset >= verifying->words.size() ||
        verifying->words[verify_offset++] != word) fatal("HLE UP: vertex data MISMATCH");
}
extern "C" void hle_d3d8_up_stats(void) {
    if (enabled) logf("HLE UP: calls %u submitted %u consumed %u fallback %u verified %u",
        calls.load(), submitted.load(), consumed.load(), fallback.load(), checked.load());
}
void hle_d3d8_up_install() {
    const char *mode = getenv("XB_HLE_D3D8_UP");
    if (!mode || !*mode || !strcmp(mode, "0")) return;
    if (strcmp(mode, "1") && strcmp(mode, "verify")) fatal("XB_HLE_D3D8_UP must be 1 or verify");
    // Exactly six whole instructions bytes, no PC-relative instruction stolen.
    const uint8_t expected[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14};
    if (memcmp(xptr<void>(draw_up), expected, sizeof expected))
        fatal("HLE UP: unsupported DrawVerticesUP prologue");
    uint8_t *trampoline = static_cast<uint8_t *>(VirtualAlloc(nullptr, 16,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) fatal("HLE UP: cannot allocate original trampoline");
    memcpy(trampoline, expected, sizeof expected);
    trampoline[6] = 0xe9;
    int32_t delta = int32_t(draw_up + 6 - xaddr(trampoline + 11));
    memcpy(trampoline + 7, &delta, 4);
    FlushInstructionCache(GetCurrentProcess(), trampoline, 11);
    original = reinterpret_cast<Original>(trampoline);
    enabled = true; verify = !strcmp(mode, "verify");
    HOOK(draw_up, replacement);
    logf("HLE UP bridge: %s (state/resources remain on existing frontend)", verify ? "verify original packing" : "host draw packets");
}
