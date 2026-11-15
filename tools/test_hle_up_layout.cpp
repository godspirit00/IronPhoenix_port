#include "../src/hle/d3d8_up_layout.h"
#include "../src/hle/d3d8_index_layout.h"
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
    // Odd index counts, unaligned caller data and unsigned high values.
    uint8_t raw[] = {0xaa,0,0,0xff,0xff,0,0x80,1,0};
    assert(hle_index_pack(raw + 1, 3, out));
    assert((out == std::vector<uint32_t>{0,65535,32768}));
    raw[1] = 99;
    assert(out[0] == 0); // snapshot remains owned after caller overwrites source
    before = out;
    assert(!hle_index_pack(raw, 0, out));
    assert(!hle_index_pack(raw, 65537, out));
    assert(!hle_index_pack(nullptr, 1, out));
    assert(out == before);
    std::vector<uint16_t> indices(1025);
    for (size_t i = 0; i < indices.size(); ++i) indices[i] = uint16_t(i * 13);
    assert(hle_index_pack(indices.data(), indices.size(), out));
    for (size_t i = 0; i < indices.size(); ++i) assert(out[i] == indices[i]);
    puts("HLE UP and indexed packing tests passed");
}
