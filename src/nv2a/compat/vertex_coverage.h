#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct NvVertexSpan { size_t begin, end; } NvVertexSpan;
typedef struct NvVertexCoverage { uint64_t draw; size_t begin, end; } NvVertexCoverage;

// Return at most two unchecked intervals and extend a draw's checked coverage.
// Disjoint requests include the intervening gap, so no unchecked holes become
// implicitly valid. Each byte receives one coherent observation per draw.
static inline unsigned nv_vertex_uncovered(NvVertexCoverage *c, uint64_t draw,
                                           size_t begin, size_t end, NvVertexSpan out[2]) {
    if (begin >= end) return 0;
    if (c->draw != draw) {
        *c = (NvVertexCoverage){draw, begin, end};
        out[0] = (NvVertexSpan){begin, end};
        return 1;
    }
    unsigned count = 0;
    if (begin < c->begin) {
        out[count++] = (NvVertexSpan){begin, c->begin};
        c->begin = begin;
    }
    if (end > c->end) {
        out[count++] = (NvVertexSpan){c->end, end};
        c->end = end;
    }
    return count;
}
