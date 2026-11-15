#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>

// DrawIndexedVertices consumes unsigned 16-bit indices. An odd last index is
// emitted by the XDK as ARRAY_ELEMENT32, but has exactly the same value.
inline bool hle_index_pack(const void *source, uint32_t count,
                           std::vector<uint32_t>& output) {
    if (!source || !count || count > 65536 ||
        uint64_t(count) * 2 > UINTPTR_MAX - uintptr_t(source)) return false;
    output.resize(count);
    const auto *bytes = static_cast<const uint8_t *>(source);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t value;
        memcpy(&value, bytes + size_t(i) * 2, 2);
        output[i] = value;
    }
    return true;
}
