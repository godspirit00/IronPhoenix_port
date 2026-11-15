#include "../src/hle/d3d8_vertex_state.h"
#include "../src/hle/d3d8_resource_registry.h"
#include <assert.h>
#include <stdio.h>

int main() {
    // The fallback must emit required input state without repeating general
    // preparation, including when it just dirtied a special shader-state bit.
    uint32_t remainder = hle_vertex_remainder_flags(0x50,0xc0000070);
    assert(remainder == 0x80000070 && !(remainder & 0x3fffff8f) && !(remainder & 0x40000000));
    assert(hle_vertex_remainder_flags(0x40,0x50) == 0x40); // newly dirty format is retained later, not emitted now
    HleResourceRegistry registry(8);
    auto vb = hle_resource_key(1, 0x1000);
    uint64_t id = registry.identify(0x200, vb);
    assert(id && registry.identify(0x200, hle_resource_key(0x100002,0x1000)) == id);
    uint64_t changed = registry.identify(0x200, hle_resource_key(1,0x2000));
    assert(changed != id);
    registry.retire(0x200);
    assert(registry.identify(0x200, vb) != id); // address reuse is a new identity
    uint64_t registered = registry.identify(0x200, vb, true);
    assert(registry.identify(0x200, vb, true) != registered);
    uint64_t texture = registry.identify(0x300, hle_resource_key(0x40001,0x1000,0x1234,20));
    assert(registry.identify(0x300, hle_resource_key(0x40002,0x1000,0x1235,20)) != texture);
    HleResourceRegistry bounded(2);
    bounded.identify(1,vb); bounded.identify(2,vb); bounded.identify(3,vb);
    assert(bounded.live() == 2 && bounded.evicted == 1 && bounded.created() == 3);
    assert(!bounded.identify(0,vb));

    HleStreamInput streams[16]{};
    HleDeclInput decl[16]{};
    for (auto& d : decl) d.format = 2;
    streams[1] = {0x1000,8,32,1,77};
    decl[0] = {1,12,0x43};
    HleVertexPatch patch{};
    assert(hle_build_vertex_patch(0x50,1,2,streams,decl,patch));
    assert(patch.formats_mask == 0xffff && patch.formats[0] == 0x2043);
    assert(patch.formats[1] == 2 && patch.offsets_mask == 1 && patch.offsets[0] == 0x1054);
    assert(patch.resources[0] == 77);
    auto snapshot = patch;
    assert(hle_build_vertex_patch(0,1,2,streams,decl,patch));
    assert(patch.formats_mask == 0 && patch.offsets[0] == 0x1054);
    assert(hle_build_vertex_patch(0x40,0,0xffffffffu,streams,decl,patch));
    assert(patch.offsets[0] == 0xff4); // signed base vertex, modulo-32 arithmetic
    assert(hle_build_vertex_patch(0,2,2,streams,decl,patch));
    assert(!patch.formats_mask && !patch.offsets_mask);
    auto before = patch;
    assert(!hle_build_vertex_patch(0x40000000,0,0,streams,decl,patch));
    assert(patch.offsets_mask == before.offsets_mask);
    decl[0].stream = 16;
    assert(!hle_build_vertex_patch(0x50,0,1,streams,decl,patch));
    decl[0].stream = 1; streams[1].present = 0;
    assert(hle_build_vertex_patch(0x50,0,1,streams,decl,patch));
    assert(!patch.offsets_mask && patch.formats[0] == 0x2043);
    streams[1].data = 0xf000;
    assert(snapshot.offsets[0] == 0x1054 && snapshot.resources[0] == 77);
    puts("HLE resource identity and vertex-state tests passed");
}
