"""Iron Phoenix additions to dsp_cpu.c, applied after tools/patch_dsp_cpu.py:
 - ring buffer of recently executed instructions (dumped on unimplemented opcodes)
 - out-of-range X/Y data accesses behave as unmapped (reads 0, writes ignored), matching the
   memory map xemu gives its JIT; the stock XDK EP code reads Y:0xFC9 on a 2K Y RAM."""
import sys

p = sys.argv[1] if len(sys.argv) > 1 else 'src/apu/dsp/interp/dsp_cpu.c'
s = open(p, encoding='utf-8').read()
assert 'xtrace_ring' not in s, 'run on a file patched only by patch_dsp_cpu.py'

helper = r'''
/* Iron Phoenix debug: ring buffer of recent instructions per DSP core */
#ifndef XTRACE_ENABLED
#define XTRACE_ENABLED 0 /* per-instruction cost; enable when chasing DSP bugs */
#endif
#define XTRACE_N 16384
typedef struct { uint32_t pc, inst, a2, a1, a0, b2, b1, b0, x0, x1, y0, y1, sr, r[8], n[8]; } xtrace_ent;
static xtrace_ent xtrace_ring[2][XTRACE_N];
static unsigned xtrace_pos[2];
#ifdef XTRACE_STREAM
static FILE *xstream;
static int xstream_lines;
static int xstream_stopped;
#endif
static void xtrace_record(dsp_core_t *dsp)
{
    int c = dsp->is_gp ? 0 : 1;
#ifdef XTRACE_STREAM
    if (c == 1 && !xstream_stopped && xstream_lines < 3000000) {
        if (!xstream) xstream = fopen("xstream_ep.txt", "w");
        xstream_lines++;
        fprintf(xstream, "%04x %06x a=%02x:%06x:%06x b=%02x:%06x:%06x x=%06x,%06x y=%06x,%06x sr=%04x r=%04x %04x %04x %04x %04x %04x %04x %04x n=%x %x %x %x %x %x\n",
                dsp->pc, dsp->cur_inst, dsp->registers[DSP_REG_A2], dsp->registers[DSP_REG_A1], dsp->registers[DSP_REG_A0],
                dsp->registers[DSP_REG_B2], dsp->registers[DSP_REG_B1], dsp->registers[DSP_REG_B0],
                dsp->registers[DSP_REG_X0], dsp->registers[DSP_REG_X1], dsp->registers[DSP_REG_Y0], dsp->registers[DSP_REG_Y1],
                dsp->registers[DSP_REG_SR], dsp->registers[DSP_REG_R0], dsp->registers[DSP_REG_R1], dsp->registers[DSP_REG_R2],
                dsp->registers[DSP_REG_R3], dsp->registers[DSP_REG_R4], dsp->registers[DSP_REG_R5], dsp->registers[DSP_REG_R6],
                dsp->registers[DSP_REG_R7], dsp->registers[DSP_REG_N0], dsp->registers[DSP_REG_N1], dsp->registers[DSP_REG_N2],
                dsp->registers[DSP_REG_N3], dsp->registers[DSP_REG_N4], dsp->registers[DSP_REG_N5]);
    }
#endif
    xtrace_ent *last = &xtrace_ring[c][(xtrace_pos[c] + XTRACE_N - 1) % XTRACE_N];
    if (xtrace_pos[c] && last->pc == dsp->pc) {
        return; /* collapse tight self-loops so the history leading into them survives */
    }
    xtrace_ent *e = &xtrace_ring[c][xtrace_pos[c]++ % XTRACE_N];
    e->pc = dsp->pc;
    e->inst = dsp->cur_inst;
    e->a2 = dsp->registers[DSP_REG_A2]; e->a1 = dsp->registers[DSP_REG_A1]; e->a0 = dsp->registers[DSP_REG_A0];
    e->b2 = dsp->registers[DSP_REG_B2]; e->b1 = dsp->registers[DSP_REG_B1]; e->b0 = dsp->registers[DSP_REG_B0];
    e->x0 = dsp->registers[DSP_REG_X0]; e->x1 = dsp->registers[DSP_REG_X1];
    e->y0 = dsp->registers[DSP_REG_Y0]; e->y1 = dsp->registers[DSP_REG_Y1];
    e->sr = dsp->registers[DSP_REG_SR];
    for (int i = 0; i < 8; i++) { e->r[i] = dsp->registers[DSP_REG_R0 + i]; e->n[i] = dsp->registers[DSP_REG_N0 + i]; }
}
void xtrace_dump(dsp_core_t *dsp, const char *why)
{
    int c = dsp->is_gp ? 0 : 1;
    FILE *out = fopen(c ? "xtrace_ep.txt" : "xtrace_gp.txt", "w");
    if (!out) out = stderr;
    fprintf(stderr, "DSP %s trace (%s) -> %s\n", c ? "EP" : "GP", why, c ? "xtrace_ep.txt" : "xtrace_gp.txt");
    for (unsigned i = 0; i < XTRACE_N; i++) {
        xtrace_ent *e = &xtrace_ring[c][(xtrace_pos[c] + i) % XTRACE_N];
        fprintf(out, "  pc=%04x %06x a=%02x:%06x:%06x b=%02x:%06x:%06x x=%06x,%06x y=%06x,%06x sr=%04x r=%04x %04x %04x %04x %04x %04x %04x %04x n=%x %x %x %x %x %x\n",
                e->pc, e->inst, e->a2, e->a1, e->a0, e->b2, e->b1, e->b0, e->x0, e->x1, e->y0, e->y1, e->sr,
                e->r[0], e->r[1], e->r[2], e->r[3], e->r[4], e->r[5], e->r[6], e->r[7], e->n[0], e->n[1], e->n[2], e->n[3], e->n[4], e->n[5]);
    }
    if (out != stderr) fclose(out);
}
'''

anchor = 'void dsp56k_execute_instruction(dsp_core_t* dsp)'
assert anchor in s
s = s.replace(anchor, helper + '\n' + anchor, 1)
fetch = '    dsp->cur_inst = read_memory_p(dsp, dsp->pc);\n'
assert s.count(fetch) == 1
s = s.replace(fetch, fetch + '#if XTRACE_ENABLED\n    xtrace_record(dsp);\n#endif\n')

unimpl = '            emu_undefined(dsp);'
assert s.count(unimpl) == 1
s = s.replace(unimpl, '            xtrace_dump(dsp, "unimplemented instruction");\n' + unimpl)

reads = '''    } else if (space == DSP_SPACE_Y) {
        assert(address < DSP_YRAM_SIZE);
        return dsp->yram[address];'''
assert reads in s
s = s.replace(reads, '''    } else if (space == DSP_SPACE_Y) {
        if (address >= DSP_YRAM_SIZE) {
            return 0; /* unmapped */
        }
        return dsp->yram[address];''')

wx = '''            assert(address < DSP_XRAM_SIZE);
            dsp->xram[address] = value;'''
assert wx in s
s = s.replace(wx, '''            if (address < DSP_XRAM_SIZE) { /* else unmapped */
                dsp->xram[address] = value;
            }''')

wy = '''        assert(address < DSP_YRAM_SIZE);
        dsp->yram[address] = value;'''
assert wy in s
s = s.replace(wy, '''        if (address < DSP_YRAM_SIZE) { /* else unmapped */
            dsp->yram[address] = value;
        }''')

oob = 'fprintf(stderr, "Out of bounds read at %x!' 
i = s.find(oob)
assert i >= 0
j = s.index(chr(10), i)
s = s[:i] + '/* unmapped (Iron Phoenix: no log, the EP reads 0x1000+ every frame) */' + s[j:]

probe_anchor = 'void dsp56k_write_memory(dsp_core_t* dsp, int space, uint32_t address, uint32_t value)' + chr(10) + '{'
assert probe_anchor in s
s = s.replace(probe_anchor, probe_anchor + '''
#ifdef XPROBE_VALUE
    {
        static int n;
        if (!dsp->is_gp && space == DSP_SPACE_X && value == XPROBE_VALUE && n++ == 0) {
            xtrace_dump(dsp, "probe value written");
#ifdef XTRACE_STREAM
            xstream_stopped = 1;
            if (xstream) fflush(xstream);
#endif
            FILE *f = fopen("ep_pram_early.bin", "wb");
            if (f) { fwrite(dsp->pram, 4, DSP_PRAM_SIZE, f); fclose(f); }
        }
    }
#endif''')

open(p, 'w', encoding='utf-8').write(s)
print('patched trace + unmapped memory')
