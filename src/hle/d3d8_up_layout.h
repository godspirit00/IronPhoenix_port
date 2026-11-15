#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>

// XDK 5849 SetStateUP builds runs of active declaration DWORDs, with byte
// gaps between runs. The last gap is adjusted by the caller's vertex stride.
// Validate the whole layout before touching caller memory or publishing work.
struct HleUpRun { uint32_t words; int32_t gap; };
inline bool hle_up_pack(const void *source, uint32_t vertices, uint32_t stride,
                        uint32_t first, uint32_t words_per_vertex,
                        const std::vector<HleUpRun>& runs,
                        std::vector<uint32_t>& output)
{
    if (!source || !vertices || !stride || !words_per_vertex ||
        words_per_vertex > 64 || runs.empty() || runs.size() > 16 ||
        uint64_t(vertices) * words_per_vertex > 65536) return false;
    int64_t cursor = first;
    uint64_t words = 0;
    for (const auto& run : runs) {
        if (!run.words || run.words > 64 || cursor < 0 ||
            uint64_t(cursor) + uint64_t(run.words) * 4 > stride) return false;
        words += run.words;
        cursor += int64_t(run.words) * 4 + run.gap;
    }
    if (words != words_per_vertex || cursor != int64_t(first) + stride ||
        uint64_t(vertices) * stride > 256 * 1024 * 1024 ||
        uint64_t(uintptr_t(source)) + uint64_t(vertices) * stride > UINTPTR_MAX) return false;
    output.resize(size_t(vertices) * words_per_vertex);
    uint32_t *dest = output.data();
    for (uint32_t v = 0; v < vertices; ++v) {
        cursor = first;
        const auto *base = static_cast<const uint8_t *>(source) + size_t(v) * stride;
        for (const auto& run : runs) {
            memcpy(dest, base + cursor, size_t(run.words) * 4);
            dest += run.words;
            cursor += int64_t(run.words) * 4 + run.gap;
        }
    }
    return true;
}
