#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bounds apply to a presented game frame, including very slow frames.
#define HLE_BUFFER_ADMIT_LIMIT (4u * 1024 * 1024)
#define HLE_BUFFER_COMPARE_LIMIT (16u * 1024 * 1024)
#define HLE_BUFFER_UPDATE_LIMIT (4u * 1024 * 1024)
static inline bool hle_buffer_reserve(size_t *used, size_t bytes, size_t limit) {
    if (*used > limit || bytes > limit - *used) return false;
    *used += bytes;
    return true;
}
static inline bool hle_buffer_recent(uint32_t frame, uint32_t used_frame) {
    return (uint32_t)(frame - used_frame) <= 1;
}
static inline size_t hle_buffer_capacity(size_t size) {
    size_t capacity = 4096;
    while (capacity < size && capacity <= SIZE_MAX / 2) capacity *= 2;
    return capacity >= size ? capacity : 0;
}
