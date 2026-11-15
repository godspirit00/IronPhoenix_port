#include "../src/nv2a/compat/range_versions.h"
#include "../src/nv2a/compat/vertex_coverage.h"
#include "../src/nv2a/compat/hle_buffer_budget.h"
#include <cassert>
#include <cstdio>

int main() {
    uint64_t pages[8]{};
    NvRangeVersions v{0, pages, 8};
    assert(nv_range_version(&v, 0, 8) == 0);
    nv_range_mark(&v, 2, 5);
    uint64_t alias_a = nv_range_version(&v, 1, 4);
    uint64_t alias_b = nv_range_version(&v, 3, 6);
    assert(alias_a && alias_a == alias_b);
    // Reading one alias cannot consume the other alias's invalidation.
    assert(nv_range_version(&v, 1, 4) == alias_a);
    assert(nv_range_version(&v, 3, 6) == alias_b);
    nv_range_mark(&v, 3, 4);
    assert(nv_range_version(&v, 1, 4) > alias_a);
    assert(nv_range_version(&v, 3, 6) > alias_b);
    uint64_t newer = nv_range_version(&v, 1, 6);
    // A later write to an earlier page must change the range maximum.
    nv_range_mark(&v, 1, 2);
    assert(nv_range_version(&v, 1, 6) > newer);
    assert(nv_range_version(&v, 6, 8) == 0);
    nv_range_mark(&v, 7, 100);
    assert(nv_range_version(&v, 7, 100) == v.serial);
    uint64_t serial = v.serial;
    nv_range_mark(&v, 9, 100);
    nv_range_mark(&v, 4, 4);
    assert(v.serial == serial);
    assert(nv_range_version(&v, 9, 100) == 0);
    NvVertexCoverage coverage{};
    NvVertexSpan spans[2];
    assert(nv_vertex_uncovered(&coverage, 1, 100, 200, spans) == 1);
    assert(spans[0].begin == 100 && spans[0].end == 200);
    assert(nv_vertex_uncovered(&coverage, 1, 110, 190, spans) == 0);
    assert(nv_vertex_uncovered(&coverage, 1, 104, 204, spans) == 1);
    assert(spans[0].begin == 200 && spans[0].end == 204);
    assert(nv_vertex_uncovered(&coverage, 1, 90, 220, spans) == 2);
    assert(spans[0].begin == 90 && spans[0].end == 100);
    assert(spans[1].begin == 204 && spans[1].end == 220);
    // Disjoint attributes cannot leave a falsely checked gap.
    assert(nv_vertex_uncovered(&coverage, 1, 300, 310, spans) == 1);
    assert(spans[0].begin == 220 && spans[0].end == 310);
    assert(nv_vertex_uncovered(&coverage, 1, 250, 260, spans) == 0);
    assert(nv_vertex_uncovered(&coverage, 1, 20, 30, spans) == 1);
    assert(spans[0].begin == 20 && spans[0].end == 90);
    // A later draw must observe the same bytes again.
    assert(nv_vertex_uncovered(&coverage, 2, 110, 190, spans) == 1);
    assert(spans[0].begin == 110 && spans[0].end == 190);
    assert(nv_vertex_uncovered(&coverage, 2, 0, 0, spans) == 0);
    assert(hle_buffer_capacity(4097) == 8192);
    assert(hle_buffer_capacity(6144) == hle_buffer_capacity(8192));
    assert(hle_buffer_capacity(8193) == 16384);
    assert(hle_buffer_capacity(SIZE_MAX) == 0);
    size_t admitted = 0;
    unsigned accepted = 0;
    // Replay a cache-thrashing frame: 720 draws with 2 MiB cache misses.
    // Only two misses may allocate/upload; the rest must use legacy buffers.
    for (unsigned i = 0; i < 720; ++i)
        accepted += hle_buffer_reserve(&admitted, 2 * 1024 * 1024, HLE_BUFFER_ADMIT_LIMIT);
    assert(accepted == 2 && admitted == HLE_BUFFER_ADMIT_LIMIT);
    assert(!hle_buffer_reserve(&admitted, SIZE_MAX, HLE_BUFFER_ADMIT_LIMIT));
    assert(admitted == HLE_BUFFER_ADMIT_LIMIT);
    size_t compared = 0, updated = 0;
    assert(hle_buffer_reserve(&compared, HLE_BUFFER_COMPARE_LIMIT, HLE_BUFFER_COMPARE_LIMIT));
    assert(!hle_buffer_reserve(&compared, 1, HLE_BUFFER_COMPARE_LIMIT));
    assert(hle_buffer_reserve(&updated, HLE_BUFFER_UPDATE_LIMIT, HLE_BUFFER_UPDATE_LIMIT));
    assert(!hle_buffer_reserve(&updated, 1, HLE_BUFFER_UPDATE_LIMIT));
    assert(hle_buffer_recent(100, 100) && hle_buffer_recent(100, 99));
    assert(!hle_buffer_recent(100, 98));
    assert(hle_buffer_recent(0, UINT32_MAX));
    puts("HLE buffer versions, coverage and cache-admission tests passed");
}
