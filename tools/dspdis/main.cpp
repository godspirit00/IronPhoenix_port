// dspdis: disassemble a DSP56300 P-memory dump (little-endian uint32 words) using the
// dsp56300 reference disassembler. Usage: dspdis <dump.bin> [start] [count]
#include "dsp56kEmu/disasm.h"
#include "dsp56kEmu/opcodes.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: dspdis dump.bin [start] [count]\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    std::vector<uint32_t> mem;
    uint32_t w;
    while (fread(&w, 4, 1, f) == 1) mem.push_back(w & 0xffffff);
    fclose(f);
    uint32_t start = argc > 2 ? strtoul(argv[2], nullptr, 16) : 0;
    uint32_t count = argc > 3 ? strtoul(argv[3], nullptr, 16) : (uint32_t)mem.size();
    dsp56k::Opcodes opcodes;
    dsp56k::Disassembler dis(opcodes);
    for (uint32_t pc = start; pc < start + count && pc < mem.size();) {
        std::string line;
        uint32_t opB = pc + 1 < mem.size() ? mem[pc + 1] : 0;
        uint32_t len = dis.disassemble(line, mem[pc], opB, 0, 0, pc);
        if (!len) len = 1;
        printf("%04x: %06x %s %s\n", pc, mem[pc], len > 1 ? "+" : " ", line.c_str());
        pc += len;
    }
    return 0;
}
