#include "../src/nv2a/compat/range_versions.h"
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
    puts("HLE buffer page-version tests passed");
}
