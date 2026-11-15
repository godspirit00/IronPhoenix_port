#pragma once
#include <stdint.h>

// Shared, pointer-free CPU/graphics-worker packet. Resource IDs describe
// identity/header revisions, not content versions or GPU completion fences.
typedef struct HleVertexPatch {
    uint32_t mode; // 0 absent, 1 compare original state, 2 apply native state
    uint32_t formats_mask, offsets_mask;
    uint32_t formats[16], offsets[16];
    uint64_t resources[16];
} HleVertexPatch;

#ifdef __cplusplus
struct HleStreamInput { uint32_t data, offset, stride, present; uint64_t identity; };
struct HleDeclInput { uint32_t stream, offset, format; };
inline bool hle_build_vertex_patch(uint32_t flags, uint32_t previous_base, uint32_t base,
                                   const HleStreamInput (&streams)[16],
                                   const HleDeclInput (&decl)[16], HleVertexPatch& output) {
    HleVertexPatch result{};
    if (flags & 0x40000000) return false;
    if (!(flags & 0x40) && previous_base == base) { output = result; return true; }
    for (unsigned i = 0; i < 16; ++i) {
        if (decl[i].stream >= 16) return false;
        const auto& stream = streams[decl[i].stream];
        result.resources[i] = stream.identity;
        if (flags & 0x10) {
            result.formats_mask |= 1u << i;
            result.formats[i] = (stream.stride << 8) + decl[i].format;
        }
        if (decl[i].format != 2 && stream.present) {
            result.offsets_mask |= 1u << i;
            // XDK arithmetic wraps in 32 bits, including negative base indices.
            result.offsets[i] = stream.data + stream.offset + decl[i].offset + base * stream.stride;
        }
    }
    output = result;
    return true;
}
#endif
