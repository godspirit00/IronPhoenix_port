// Resource identity audit and bounded native vertex-input preparation.
#include "hle.h"
#include "d3d8_hook.h"
#include "d3d8_vertex_state.h"
#include "d3d8_resource_registry.h"
#include <atomic>
#include <stdlib.h>

extern "C" void hle_d3d8_attach_vertex_patch(uint32_t, const HleVertexPatch *);
namespace {
enum Mode { Off, Observe, Verify, Native } mode;
SRWLOCK registry_lock = SRWLOCK_INIT;
HleResourceRegistry registry;
std::atomic<uint64_t> registrations, frees, stream_sets, texture_sets, target_sets, locks;
std::atomic<uint64_t> preparations, native_preparations, delegated, snapshots, gpu_checked, gpu_applied;
using Register = uint32_t (__stdcall *)(void *, void *);
using Free = uint32_t (__stdcall *)(void *);
using Stream = uint32_t (__stdcall *)(uint32_t, void *, uint32_t);
using Texture = uint32_t (__stdcall *)(uint32_t, void *);
using Target = uint32_t (__stdcall *)(void *, void *, uint32_t);
using TargetNormal = uint32_t (__stdcall *)(void *, void *);
using Lock = uint32_t (__stdcall *)(void *, uint32_t, uint32_t, void *, void *, uint32_t);
Register original_register; Free original_free; Stream original_stream;
Texture original_texture; Target original_target; Lock original_lock;
TargetNormal original_target_normal;
uint32_t& word(void *base, size_t offset) {
    return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(base) + offset);
}
uint64_t identity(void *resource, bool renew = false) {
    if (!resource) return 0;
    uint32_t common = word(resource, 0), data = word(resource, 4), type = common & 0x70000;
    uint32_t format = 0, size = 0;
    if (type == 0x40000 || type == 0x50000) { format = word(resource, 12); size = word(resource, 16); }
    auto key = hle_resource_key(common, data, format, size);
    AcquireSRWLockExclusive(&registry_lock);
    uint64_t id = registry.identify(xaddr(resource), key, renew);
    ReleaseSRWLockExclusive(&registry_lock);
    return id;
}
uint32_t __stdcall resource_register(void *resource, void *base) {
    uint32_t result = original_register(resource, base);
    ++registrations; identity(resource, true); return result;
}
uint32_t __stdcall resource_free(void *resource) {
    ++frees;
    AcquireSRWLockExclusive(&registry_lock);
    registry.retire(xaddr(resource));
    ReleaseSRWLockExclusive(&registry_lock);
    return original_free(resource); // may recurse; never hold registry lock here
}
uint32_t __stdcall set_stream(uint32_t slot, void *resource, uint32_t stride) {
    uint32_t result = original_stream(slot, resource, stride);
    ++stream_sets; identity(resource); return result;
}
uint32_t __stdcall set_texture(uint32_t slot, void *resource) {
    uint32_t result = original_texture(slot, resource);
    ++texture_sets; identity(resource); return result;
}
uint32_t __stdcall set_target(void *color, void *depth, uint32_t flags) {
    uint32_t result = original_target(color, depth, flags);
    ++target_sets; identity(color); identity(depth); return result;
}
uint32_t __stdcall set_target_normal(void *color, void *depth) {
    uint32_t result = original_target_normal(color, depth);
    ++target_sets; identity(color); identity(depth); return result;
}
uint32_t __stdcall lock_surface(void *resource, uint32_t face, uint32_t level,
                                 void *locked, void *rect, uint32_t flags) {
    ++locks; identity(resource);
    return original_lock(resource, face, level, locked, rect, flags);
}
bool capture(void *device, uint32_t flags, uint32_t old_base, uint32_t base, HleVertexPatch& patch) {
    if (flags & 0x40000000) return false;
    if (!(flags & 0x40) && old_base == base) { patch = {}; return true; }
    void *shader = *reinterpret_cast<void **>(static_cast<uint8_t *>(device) + 0x794);
    if (!shader) return false;
    auto *map = xptr<uint8_t>(0x354cb8 + (word(shader, 4) & 0x10));
    HleDeclInput decl[16]{};
    HleStreamInput streams[16]{};
    uint32_t used = 0;
    for (unsigned i = 0; i < 16; ++i) {
        unsigned reg = map[i];
        if (reg >= 16) return false;
        decl[i] = {word(shader, reg * 16 + 0x14), word(shader, reg * 16 + 0x18), word(shader, reg * 16 + 0x1c)};
        if (decl[i].stream >= 16) return false;
        used |= 1u << decl[i].stream;
    }
    for (unsigned i = 0; i < 16; ++i) if (used & (1u << i)) {
        void *entry = xptr<void>(0x23b658 + i * 12);
        void *resource = *reinterpret_cast<void **>(static_cast<uint8_t *>(entry) + 8);
        streams[i] = {resource ? word(resource, 4) : 0, word(entry, 4), word(entry, 0),
                      resource ? 1u : 0u, identity(resource)};
    }
    return hle_build_vertex_patch(flags, old_base, base, streams, decl, patch);
}
}

extern "C" void hle_d3d8_prepare_vertex(void *device, uint32_t base, uint32_t token) {
    using State = void (__thiscall *)(void *, uint32_t);
    auto original = reinterpret_cast<State>(0x232930);
    if (mode == Off) { original(device, base); return; }
    ++preparations;
    uint32_t flags = *xptr<uint32_t>(0x238cf8), old_base = word(device, 0x20);
    HleVertexPatch patch{};
    // No replay of general deferred state: this first native case is limited
    // to clean general state, where only vertex input/base-index work remains.
    if (mode == Native && !(flags & 0x7fffff8f) && capture(device, flags, old_base, base, patch)) {
        *xptr<uint32_t>(0x238cf8) = flags & ~0x50u;
        if ((flags & 0x40) || old_base != base) word(device, 0x20) = base;
        patch.mode = 2; ++native_preparations;
    } else {
        original(device, base); ++delegated;
        if (mode == Observe) return;
        if (!capture(device, flags, old_base, base, patch)) return;
        patch.mode = 1;
    }
    if (!(patch.formats_mask | patch.offsets_mask)) return;
    ++snapshots;
    hle_d3d8_attach_vertex_patch(token, &patch);
}
extern "C" void hle_d3d8_vertex_patch_complete(bool applied) {
    if (applied) ++gpu_applied; else ++gpu_checked;
}
extern "C" void hle_d3d8_resources_stats(void) {
    if (mode == Off) return;
    logf("HLE resources audit: register %llu free %llu streams %llu textures %llu targets %llu locks %llu",
        (unsigned long long)registrations.load(), (unsigned long long)frees.load(),
        (unsigned long long)stream_sets.load(), (unsigned long long)texture_sets.load(),
        (unsigned long long)target_sets.load(), (unsigned long long)locks.load());
    logf("HLE vertex state: prepares %llu native %llu delegated %llu snapshots %llu checked %llu applied %llu",
        (unsigned long long)preparations.load(), (unsigned long long)native_preparations.load(),
        (unsigned long long)delegated.load(), (unsigned long long)snapshots.load(),
        (unsigned long long)gpu_checked.load(), (unsigned long long)gpu_applied.load());
    AcquireSRWLockShared(&registry_lock);
    logf("HLE resource identities: live %zu created %llu retired %llu evicted %llu", registry.live(),
        (unsigned long long)registry.created(), (unsigned long long)registry.retired, (unsigned long long)registry.evicted);
    ReleaseSRWLockShared(&registry_lock);
}
void hle_d3d8_resources_install() {
    const char *setting = getenv("XB_HLE_D3D8_RESOURCES");
    if (!setting || !*setting || !strcmp(setting, "off")) return;
    if (!strcmp(setting, "observe")) mode = Observe;
    else if (!strcmp(setting, "verify")) mode = Verify;
    else if (!strcmp(setting, "1")) mode = Native;
    else fatal("XB_HLE_D3D8_RESOURCES must be observe, verify or 1");
    const uint8_t reg[] = {0x8b,0x4c,0x24,0x04,0x8b,0x54,0x24,0x08};
    const uint8_t free[] = {0x56,0x57,0x8b,0x7c,0x24,0x0c};
    const uint8_t stream[] = {0x8b,0x0d,0x98,0x91,0x23,0x00};
    const uint8_t texture[] = {0x83,0xec,0x08,0x53,0x55};
    const uint8_t target[] = {0x55,0x8b,0x6c,0x24,0x08,0x56};
    const uint8_t target_normal[] = {0x83,0xec,0x0c,0x53,0x55};
    const uint8_t lock[] = {0x51,0x53,0x8a,0x5c,0x24,0x20};
#define INSTALL(type, address, bytes, fn, name) reinterpret_cast<type>(hle_d3d8_trampoline(address, bytes, sizeof bytes, reinterpret_cast<void *>(fn), name))
    original_register = INSTALL(Register, 0x22c880, reg, resource_register, "Resource_Register");
    original_free = INSTALL(Free, 0x22c640, free, resource_free, "FreeResource");
    original_stream = INSTALL(Stream, 0x22b340, stream, set_stream, "SetStreamSource");
    original_texture = INSTALL(Texture, 0x229b50, texture, set_texture, "SetTexture");
    original_target = INSTALL(Target, 0x228ff0, target, set_target, "SetRenderTargetFast");
    original_target_normal = INSTALL(TargetNormal, 0x228d70, target_normal, set_target_normal, "SetRenderTarget");
    original_lock = INSTALL(Lock, 0x22f4a0, lock, lock_surface, "Lock2DSurface");
#undef INSTALL
    logf("HLE resources mode: %s", setting);
}
