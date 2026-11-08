"""Apply Iron Phoenix fixes to xemu's DSP interpreter (src/apu/dsp/interp/dsp_cpu.c).
Run after copying a pristine dsp_cpu.c from xemu-src. Idempotent checks via asserts."""
import re
import sys

p = sys.argv[1] if len(sys.argv) > 1 else 'src/apu/dsp/interp/dsp_cpu.c'
s = open(p, encoding='utf-8').read()

s = s.replace('#include "debug.h"', '#include "../debug.h"')


def body(name):
    i = s.index('static uint16_t %s(uint32_t *dest, int n)\n{' % name)
    j = s.index('\n}\n', i) + 3
    return i, j


ASL = r'''static uint16_t dsp_asl56(uint32_t *dest, int n)
{
    /* Shift left dest n bits: D<<=n
     * Iron Phoenix: n == 0 and n >= 56 handled (shift by 64 is undefined in C). */

    uint64_t dest_v = dest[2] | ((uint64_t)dest[1] << 24) | ((uint64_t)dest[0] << 48);

    if (n <= 0) {
        return 0;
    }

    uint32_t carry = n > 56 ? 0 : (dest_v >> (56-n)) & 1;

    uint64_t dest_s = n >= 64 ? 0 : dest_v << n;
    dest[2] = dest_s & BITMASK(24);
    dest[1] = (dest_s >> 24) & BITMASK(24);
    dest[0] = (dest_s >> 48) & BITMASK(8);

    uint32_t overflow = n >= 56 ? dest_v != 0 : (dest_v >> (56-n)) != 0;
    uint32_t v = ((dest_v >> 55) & 1) != ((dest_s >> 55) & 1);

    return (overflow<<DSP_SR_L)|(v<<DSP_SR_V)|(carry<<DSP_SR_C);
}
'''

ASR = r'''static uint16_t dsp_asr56(uint32_t *dest, int n)
{
    /* Arithmetic shift right dest n bits: D>>=n (sign bit 55 replicated)
     * Iron Phoenix: was a logical shift; n == 0 is now defined. */

    uint64_t dest_v = dest[2] | ((uint64_t)dest[1] << 24) | ((uint64_t)dest[0] << 48);

    if (n <= 0) {
        return 0;
    }

    int64_t sv = (int64_t)(dest_v << 8) >> 8;
    uint16_t carry = n > 56 ? (uint16_t)((dest_v >> 55) & 1) : (uint16_t)((dest_v >> (n-1)) & 1);

    sv >>= (n > 63 ? 63 : n);
    dest_v = (uint64_t)sv & ((1ULL << 56) - 1);
    dest[2] = dest_v & BITMASK(24);
    dest[1] = (dest_v >> 24) & BITMASK(24);
    dest[0] = (dest_v >> 48) & BITMASK(8);

    return (carry<<DSP_SR_C);
}
'''

i, j = body('dsp_asl56')
s = s[:i] + ASL + s[j:]
i, j = body('dsp_asr56')
s = s[:i] + ASR + s[j:]

assert s.count('#include "dsp_emu.c.inc"') == 1
s = s.replace('#include "dsp_emu.c.inc"', '#include "dsp_emu.c.inc"\n#include "dsp_emu_extra.c.inc"')

names = {
    'extract S1, S2, D': 'emu_extract_reg', 'extract #CO, S2, D': 'emu_extract_imm',
    'extractu S1, S2, D': 'emu_extractu_reg', 'extractu #CO, S2, D': 'emu_extractu_imm',
    'insert S1, S2, D': 'emu_insert_reg', 'insert #CO, S2, D': 'emu_insert_imm',
    'merge S, D': 'emu_merge', 'clb S, D': 'emu_clb', 'normf S, D': 'emu_normf',
    'lsl S, D': 'emu_lsl_reg', 'lsr S, D': 'emu_lsr_reg', 'lsr #ii, D': 'emu_lsr_imm',
    'asl S1, S2, D': 'emu_asl_reg', 'asr S1, S2, D': 'emu_asr_reg',
    'or #xx, D': 'emu_or_imm', 'eor #xx, D': 'emu_eor_imm', 'eor #xxxx, D': 'emu_eor_long',
    'pflush': 'emu_nop_extra', 'pflushun': 'emu_nop_extra', 'pfree': 'emu_nop_extra',
    'bscc xxxx': 'emu_bscc_long', 'bscc xxx': 'emu_bscc_imm', 'bscc Rn': 'emu_bscc_rn',
    'bsr Rn': 'emu_bsr_rn', 'bra Rn': 'emu_bra_rn', 'bcc Rn': 'emu_bcc_rn', 'brkcc': 'emu_brkcc',
    'jclr #n, [X or Y]:qq, xxxx': 'emu_jclr_qq', 'jset #n, [X or Y]:qq, xxxx': 'emu_jset_qq',
    'jsclr #n, [X or Y]:qq, xxxx': 'emu_jsclr_qq', 'jsset #n, [X or Y]:qq, xxxx': 'emu_jsset_qq',
    'brclr #n, [X or Y]:qq, xxxx': 'emu_brclr_qq', 'brset #n, [X or Y]:qq, xxxx': 'emu_brset_qq',
    'bsclr #n, [X or Y]:qq, xxxx': 'emu_bsclr_qq', 'bsset #n, [X or Y]:qq, xxxx': 'emu_bsset_qq',
    'bclr #n, [X or Y]:qq': 'emu_bclr_qq', 'bset #n, [X or Y]:qq': 'emu_bset_qq',
    'bchg #n, [X or Y]:qq': 'emu_bchg_qq', 'btst #n, [X or Y]:qq': 'emu_btst_qq',
    'move Y:(Rn + xxxx) <-> R': 'emu_move_y_long',
}
for name, fn in names.items():
    pat = re.compile(r'("%s",\s*)NULL(,\s*)NULL' % re.escape(name))
    s, c = pat.subn(lambda m: m.group(1) + 'NULL' + m.group(2) + fn, s)
    assert c == 1, (name, c)

old = '''            DPRINTF("%x - %s\\n", dsp->cur_inst, op->name);
            emu_undefined(dsp);'''
assert old in s, 'dispatch site'
s = s.replace(old, '''            fprintf(stderr, "DSP: unimplemented instruction %06x '%s' at pc %04x (%s)\\n",
                    dsp->cur_inst, op->name, dsp->pc, dsp->is_gp ? "GP" : "EP");
            emu_undefined(dsp);''')

# xemu bug: match_MMMRRR accepts the immediate mode (MMMRRR=110100) for every instruction, so
# bit-test/jump/loop instructions with an ea operand shadow e.g. 'move Y:(Rn + xxxx)'
# (0x0b74c4 decoded as jsclr). Those instructions take memory operands only.
strict = '''static bool match_MMMRRR_mem(uint32_t op)
{
    /* Iron Phoenix: memory operand only - immediate (MMMRRR=110100) is not an address */
    uint32_t RRR = (op >> 8) & BITMASK(3);
    uint32_t MMM = (op >> 11) & BITMASK(3);
    if (MMM == 0x6) {
        return RRR == 0x0;
    }
    return true;
}

static const OpcodeEntry nonparallel_opcodes[] = {'''
assert s.count('static const OpcodeEntry nonparallel_opcodes[] = {') == 1
s = s.replace('static const OpcodeEntry nonparallel_opcodes[] = {', strict)
nstrict = 0
out = []
for line in s.split(chr(10)):
    if 'match_MMMRRR }' in line:
        m = re.search(r'"([^"]*)",\s*"([^"]*)"', line)
        name = m.group(2) if m else ''
        if '#n' in name or re.match(r'(jcc|jmp|jscc|jsr|do |dor |rep |plock|punlock)', name):
            line = line.replace('match_MMMRRR }', 'match_MMMRRR_mem }')
            nstrict += 1
    out.append(line)
s = chr(10).join(out)
assert nstrict >= 20, nstrict
print('strict ea matcher on', nstrict, 'entries')

open(p, 'w', encoding='utf-8').write(s)
print('patched', p)
