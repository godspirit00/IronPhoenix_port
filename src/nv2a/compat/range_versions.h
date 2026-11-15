#pragma once
#include <stdint.h>
#include <stddef.h>

// Monotonic observations, independent of client dirty-bit consumption.
typedef struct NvRangeVersions { uint64_t serial; uint64_t *pages; size_t count; } NvRangeVersions;
static inline void nv_range_mark(NvRangeVersions *v, size_t first, size_t end) {
    if (end > v->count) end = v->count;
    if (first >= end) return;
    uint64_t stamp = ++v->serial;
    for (size_t p = first; p < end; ++p) v->pages[p] = stamp;
}
static inline uint64_t nv_range_version(const NvRangeVersions *v, size_t first, size_t end) {
    if (end > v->count) end = v->count;
    uint64_t stamp = 0;
    for (size_t p = first; p < end; ++p) if (v->pages[p] > stamp) stamp = v->pages[p];
    return stamp;
}
