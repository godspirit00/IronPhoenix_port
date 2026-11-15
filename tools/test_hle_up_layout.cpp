#include "../src/hle/d3d8_up_layout.h"
#include <assert.h>
#include <stdio.h>

int main() {
    uint32_t data[32];
    for (uint32_t i = 0; i < 32; ++i) data[i] = 100 + i;
    std::vector<uint32_t> out;
    // Contiguous data: packed copy, no omitted attributes.
    assert(hle_up_pack(data, 2, 16, 0, 4, {{4, 0}}, out));
    assert((out == std::vector<uint32_t>{100,101,102,103,104,105,106,107}));
    // Omit a leading DWORD, skip an inactive attribute, preserve stride padding.
    assert(hle_up_pack(data, 2, 24, 4, 3, {{2,4},{1,8}}, out));
    assert((out == std::vector<uint32_t>{101,102,104,107,108,110}));
    // Last gap includes the original initial offset (the XDK loops on a
    // continuously advancing source pointer, rather than reloading each row).
    assert(hle_up_pack(data, 2, 16, 8, 2, {{2,8}}, out));
    assert((out == std::vector<uint32_t>{102,103,106,107}));
    auto before = out;
    assert(!hle_up_pack(data, 2, 16, 0, 4, {{4,4}}, out)); // bad next-row advance
    assert(!hle_up_pack(data, 2, 16, 8, 4, {{4,0}}, out)); // overreads stride
    assert(!hle_up_pack(data, 2, 16, 0, 3, {{4,0}}, out)); // wrong declared size
    assert(!hle_up_pack(data, 0, 16, 0, 4, {{4,0}}, out));
    assert(!hle_up_pack(data, 65537, 4, 0, 1, {{1,0}}, out));
    assert(!hle_up_pack(data, 2, 16, 0, 4, {}, out));
    assert(!hle_up_pack(nullptr, 2, 16, 0, 4, {{4,0}}, out));
    assert(out == before); // invalid layouts never partially publish output
    puts("HLE UP packing tests passed");
}
