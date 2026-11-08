/*
 * DSP56300 instructions missing from xemu's C interpreter (xemu relies on its x86-64-only JIT
 * for these). Semantics follow the dsp56300 emulator by The Usual Suspects (gearmulator),
 * which is validated against Motorola's sim56300. Sixteen-bit arithmetic mode is not handled.
 *
 * Iron Phoenix native runtime addition.
 */

/* 56-bit accumulator access: A2:A1:A0 (8:24:24). b selects A (0) or B (1). */
static uint64_t xacc_get(dsp_core_t *dsp, int b)
{
    return ((uint64_t)(dsp->registers[DSP_REG_A2 + b] & 0xff) << 48) |
           ((uint64_t)(dsp->registers[DSP_REG_A1 + b] & 0xffffff) << 24) |
           (uint64_t)(dsp->registers[DSP_REG_A0 + b] & 0xffffff);
}

static void xacc_set(dsp_core_t *dsp, int b, uint64_t v)
{
    dsp->registers[DSP_REG_A2 + b] = (uint32_t)(v >> 48) & 0xff;
    dsp->registers[DSP_REG_A1 + b] = (uint32_t)(v >> 24) & 0xffffff;
    dsp->registers[DSP_REG_A0 + b] = (uint32_t)v & 0xffffff;
}

static void xccr_set(dsp_core_t *dsp, int bit, bool on)
{
    if (on) {
        dsp->registers[DSP_REG_SR] |= 1 << bit;
    } else {
        dsp->registers[DSP_REG_SR] &= ~(1u << bit);
    }
}

/* E, U, N, Z from a full accumulator */
static void xccr_update_acc(dsp_core_t *dsp, int b)
{
    emu_ccr_update_e_u_n_z(dsp, dsp->registers[DSP_REG_A2 + b], dsp->registers[DSP_REG_A1 + b],
                           dsp->registers[DSP_REG_A0 + b]);
}

/* "sss" source field: A1, B1, X0, Y0, X1, Y1 */
static uint32_t xread_sss(dsp_core_t *dsp, uint32_t sss)
{
    static const int map[8] = {-1, -1, DSP_REG_A1, DSP_REG_B1, DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    int r = map[sss & 7];
    return r < 0 ? 0 : dsp->registers[r] & 0xffffff;
}

/* "qqq" source field: A0, B0, X0, Y0, X1, Y1 */
static uint32_t xread_qqq(dsp_core_t *dsp, uint32_t qqq)
{
    static const int map[8] = {-1, -1, DSP_REG_A0, DSP_REG_B0, DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    int r = map[qqq & 7];
    return r < 0 ? 0 : dsp->registers[r] & 0xffffff;
}

static uint32_t xfetch_ext(dsp_core_t *dsp)
{
    uint32_t w = read_memory_p(dsp, dsp->pc + 1);
    dsp->cur_inst_len++;
    return w;
}

/* ------------------------------------------------------------------ bit-field ops */
static void xbitfield_extract(dsp_core_t *dsp, uint32_t control, int src, int dst, bool is_signed)
{
    uint32_t width = (control >> 12) & 0x3f;
    uint32_t offset = control & 0x3f;
    uint64_t s = xacc_get(dsp, src);
    uint64_t result = 0;
    if (width) {
        uint64_t mask = ((uint64_t)1 << 56) - 1;
        mask >>= (56 - (width > 56 ? 56 : width));
        uint64_t field = offset >= 56 ? 0 : (s >> offset) & mask;
        if (is_signed && (field & ((uint64_t)1 << (width - 1)))) {
            field |= (((uint64_t)1 << 56) - 1) ^ mask;
        }
        result = field;
    }
    xacc_set(dsp, dst, result);
    xccr_set(dsp, DSP_SR_C, false);
    xccr_set(dsp, DSP_SR_V, false);
    xccr_update_acc(dsp, dst);
}

static void emu_extract_reg(dsp_core_t *dsp)
{
    xbitfield_extract(dsp, xread_sss(dsp, (dsp->cur_inst >> 1) & 7), (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, true);
}
static void emu_extract_imm(dsp_core_t *dsp)
{
    xbitfield_extract(dsp, xfetch_ext(dsp), (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, true);
}
static void emu_extractu_reg(dsp_core_t *dsp)
{
    xbitfield_extract(dsp, xread_sss(dsp, (dsp->cur_inst >> 1) & 7), (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, false);
}
static void emu_extractu_imm(dsp_core_t *dsp)
{
    xbitfield_extract(dsp, xfetch_ext(dsp), (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, false);
}

static void xbitfield_insert(dsp_core_t *dsp, uint32_t control, uint32_t src, int dst)
{
    uint32_t width = (control >> 12) & 0x3f;
    uint32_t offset = control & 0x3f;
    uint64_t d = xacc_get(dsp, dst);
    uint64_t mask = width ? (((uint64_t)1 << width) - 1) : 0;
    if (offset < 56) {
        d &= ~(mask << offset);
        d |= ((uint64_t)src & mask) << offset;
    }
    xacc_set(dsp, dst, d);
    xccr_set(dsp, DSP_SR_C, false);
    xccr_set(dsp, DSP_SR_V, false);
    xccr_update_acc(dsp, dst);
}

static void emu_insert_reg(dsp_core_t *dsp)
{
    xbitfield_insert(dsp, xread_sss(dsp, (dsp->cur_inst >> 1) & 7), xread_qqq(dsp, (dsp->cur_inst >> 4) & 7), dsp->cur_inst & 1);
}
static void emu_insert_imm(dsp_core_t *dsp)
{
    uint32_t control = xfetch_ext(dsp);
    xbitfield_insert(dsp, control, xread_qqq(dsp, (dsp->cur_inst >> 4) & 7), dsp->cur_inst & 1);
}

/* MERGE S,D: {S[11-0], D[35-24]} -> D[47-24] */
static void emu_merge(dsp_core_t *dsp)
{
    int d = dsp->cur_inst & 1;
    uint64_t dv = xacc_get(dsp, d);
    uint64_t src = xread_sss(dsp, (dsp->cur_inst >> 1) & 7);
    uint64_t merged = ((src & 0xfff) << 12) | ((dv >> 24) & 0xfff);
    dv = (dv & ~((uint64_t)0xffffff << 24)) | (merged << 24);
    xacc_set(dsp, d, dv);
    xccr_set(dsp, DSP_SR_N, (merged >> 23) & 1);
    xccr_set(dsp, DSP_SR_Z, merged == 0);
    xccr_set(dsp, DSP_SR_V, false);
}

/* CLB S,D: count leading bits (sign-aware), result in D1 */
static void emu_clb(dsp_core_t *dsp)
{
    int s = (dsp->cur_inst >> 1) & 1, d = dsp->cur_inst & 1;
    uint64_t v = xacc_get(dsp, s);
    int count = 0;
    if (v != 0) {
        int64_t shifted = (int64_t)(v << 8);   /* bit 55 to bit 63 */
        uint64_t x = (uint64_t)(shifted < 0 ? ~shifted : shifted);
        x |= 0xff;
        int bsr = 63 - __builtin_clzll(x);
        count = bsr - (64 - 9 - 1);
    }
    uint64_t res = ((uint64_t)(int64_t)count << 24) & (((uint64_t)1 << 56) - 1);
    xacc_set(dsp, d, res);
    xccr_set(dsp, DSP_SR_N, (res >> 47) & 1);
    xccr_set(dsp, DSP_SR_Z, count == 0);
    xccr_set(dsp, DSP_SR_V, false);
}

/* ------------------------------------------------------------------ shifts */
static void xlsl(dsp_core_t *dsp, int d, uint32_t n)
{
    uint32_t v = dsp->registers[DSP_REG_A1 + d] & 0xffffff;
    xccr_set(dsp, DSP_SR_C, n && n <= 24 && ((v >> (24 - n)) & 1));
    uint32_t res = n >= 24 ? 0 : (v << n) & 0xffffff;
    dsp->registers[DSP_REG_A1 + d] = res;
    xccr_set(dsp, DSP_SR_N, (res >> 23) & 1);
    xccr_set(dsp, DSP_SR_Z, res == 0);
    xccr_set(dsp, DSP_SR_V, false);
}

static void xlsr(dsp_core_t *dsp, int d, uint32_t n)
{
    uint32_t v = dsp->registers[DSP_REG_A1 + d] & 0xffffff;
    xccr_set(dsp, DSP_SR_C, n && n <= 24 && ((v >> (n - 1)) & 1));
    uint32_t res = n >= 24 ? 0 : v >> n;
    dsp->registers[DSP_REG_A1 + d] = res;
    xccr_set(dsp, DSP_SR_N, (res >> 23) & 1);
    xccr_set(dsp, DSP_SR_Z, res == 0);
    xccr_set(dsp, DSP_SR_V, false);
}

static void emu_lsl_reg(dsp_core_t *dsp) { xlsl(dsp, dsp->cur_inst & 1, xread_sss(dsp, (dsp->cur_inst >> 1) & 7) & 0x3f); }
static void emu_lsr_reg(dsp_core_t *dsp) { xlsr(dsp, dsp->cur_inst & 1, xread_sss(dsp, (dsp->cur_inst >> 1) & 7) & 0x3f); }
static void emu_lsr_imm(dsp_core_t *dsp) { xlsr(dsp, dsp->cur_inst & 1, (dsp->cur_inst >> 1) & 0x1f); }

static void xasl_asr(dsp_core_t *dsp, int src, int dst, uint32_t n, bool left)
{
    uint32_t v[3];
    v[0] = dsp->registers[DSP_REG_A2 + src];
    v[1] = dsp->registers[DSP_REG_A1 + src];
    v[2] = dsp->registers[DSP_REG_A0 + src];
    uint16_t newsr = left ? dsp_asl56(v, n) : dsp_asr56(v, n);
    dsp->registers[DSP_REG_A2 + dst] = v[0];
    dsp->registers[DSP_REG_A1 + dst] = v[1];
    dsp->registers[DSP_REG_A0 + dst] = v[2];
    dsp->registers[DSP_REG_SR] &= BITMASK(16) - ((1 << DSP_SR_C) | (1 << DSP_SR_V));
    dsp->registers[DSP_REG_SR] |= newsr;
    emu_ccr_update_e_u_n_z(dsp, v[0], v[1], v[2]);
}

static void emu_asl_reg(dsp_core_t *dsp)
{
    xasl_asr(dsp, (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, xread_sss(dsp, (dsp->cur_inst >> 1) & 7) & 0x3f, true);
}
static void emu_asr_reg(dsp_core_t *dsp)
{
    xasl_asr(dsp, (dsp->cur_inst >> 4) & 1, dsp->cur_inst & 1, xread_sss(dsp, (dsp->cur_inst >> 1) & 7) & 0x3f, false);
}

/* NORMF S,D: S[23]==0 -> ASR D by S, else ASL D by -S. C unchanged. */
static void emu_normf(dsp_core_t *dsp)
{
    int d = dsp->cur_inst & 1;
    uint32_t s = xread_sss(dsp, (dsp->cur_inst >> 1) & 7);
    uint32_t carry = dsp->registers[DSP_REG_SR] & (1 << DSP_SR_C);
    if (!(s & 0x800000)) {
        xasl_asr(dsp, d, d, s > 56 ? 56 : s, false);
    } else {
        int32_t neg = -(int32_t)((s ^ 0x800000) - 0x800000); /* sign-extend 24 bit and negate */
        xasl_asr(dsp, d, d, neg > 56 ? 56 : (uint32_t)neg, true);
    }
    dsp->registers[DSP_REG_SR] = (dsp->registers[DSP_REG_SR] & ~(1u << DSP_SR_C)) | carry;
}

/* ------------------------------------------------------------------ logic immediates */
static void xlogic(dsp_core_t *dsp, int d, uint32_t value, int op)
{
    uint32_t *r = &dsp->registers[DSP_REG_A1 + d];
    if (op == 0) {
        *r |= value;
    } else {
        *r ^= value;
    }
    *r &= 0xffffff;
    xccr_set(dsp, DSP_SR_N, (*r >> 23) & 1);
    xccr_set(dsp, DSP_SR_Z, *r == 0);
    xccr_set(dsp, DSP_SR_V, false);
}

static void emu_or_imm(dsp_core_t *dsp) { xlogic(dsp, (dsp->cur_inst >> 3) & 1, (dsp->cur_inst >> 8) & 0x3f, 0); }
static void emu_eor_imm(dsp_core_t *dsp) { xlogic(dsp, (dsp->cur_inst >> 3) & 1, (dsp->cur_inst >> 8) & 0x3f, 1); }
static void emu_eor_long(dsp_core_t *dsp)
{
    uint32_t v = xfetch_ext(dsp);
    xlogic(dsp, (dsp->cur_inst >> 3) & 1, v, 1);
}

/* ------------------------------------------------------------------ no-op class */
static void emu_nop_extra(dsp_core_t *dsp) {}

/* ------------------------------------------------------------------ branches */
static void xbranch_sub(dsp_core_t *dsp, uint32_t target)
{
    if (dsp->interrupt_state != DSP_INTERRUPT_LONG) {
        dsp_stack_push(dsp, dsp->pc + dsp->cur_inst_len, dsp->registers[DSP_REG_SR], 0);
    } else {
        dsp->interrupt_state = DSP_INTERRUPT_DISABLED;
    }
    dsp->pc = target & BITMASK(24);
    dsp->cur_inst_len = 0;
}

static uint32_t xshort_disp(dsp_core_t *dsp)
{
    uint32_t xxx = (dsp->cur_inst & BITMASK(5)) + ((dsp->cur_inst & (BITMASK(4) << 6)) >> 1);
    return dsp_signextend(9, xxx);
}

static uint32_t xrn(dsp_core_t *dsp) { return dsp->registers[DSP_REG_R0 + ((dsp->cur_inst >> 8) & 7)]; }

static void emu_bscc_long(dsp_core_t *dsp)
{
    uint32_t xxxx = xfetch_ext(dsp);
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) xbranch_sub(dsp, dsp->pc + xxxx);
    dsp->instr_cycle += 4;
}
static void emu_bscc_imm(dsp_core_t *dsp)
{
    if (emu_calc_cc(dsp, (dsp->cur_inst >> 12) & BITMASK(4))) xbranch_sub(dsp, dsp->pc + xshort_disp(dsp));
    dsp->instr_cycle += 2;
}
static void emu_bscc_rn(dsp_core_t *dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) xbranch_sub(dsp, dsp->pc + xrn(dsp));
    dsp->instr_cycle += 2;
}
static void emu_bsr_rn(dsp_core_t *dsp)
{
    xbranch_sub(dsp, dsp->pc + xrn(dsp));
    dsp->instr_cycle += 2;
}
static void emu_bra_rn(dsp_core_t *dsp)
{
    dsp->pc = (dsp->pc + xrn(dsp)) & BITMASK(24);
    dsp->cur_inst_len = 0;
}
static void emu_bcc_rn(dsp_core_t *dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        dsp->pc = (dsp->pc + xrn(dsp)) & BITMASK(24);
        dsp->cur_inst_len = 0;
    }
}
/* BRKcc: leave the current DO loop (continue after LA) */
static void emu_brkcc(dsp_core_t *dsp)
{
    if (emu_calc_cc(dsp, dsp->cur_inst & BITMASK(4))) {
        uint32_t target = dsp->registers[DSP_REG_LA] + 1;
        emu_enddo(dsp);
        dsp->pc = target & BITMASK(24);
        dsp->cur_inst_len = 0;
    }
}

/* ------------------------------------------------------------------ qq-addressed bit ops / jumps */
static uint32_t xqq_addr(dsp_core_t *dsp) { return 0xffff80 + ((dsp->cur_inst >> 8) & BITMASK(6)); }
static int xqq_space(dsp_core_t *dsp) { return (dsp->cur_inst >> 6) & 1; }
static bool xqq_bit(dsp_core_t *dsp)
{
    uint32_t v = dsp56k_read_memory(dsp, xqq_space(dsp), xqq_addr(dsp));
    return (v >> (dsp->cur_inst & BITMASK(5))) & 1;
}

static void xjump_if(dsp_core_t *dsp, bool cond, bool sub, bool relative)
{
    uint32_t ext = read_memory_p(dsp, dsp->pc + 1);
    dsp->cur_inst_len++;
    dsp->instr_cycle += 4;
    if (!cond) return;
    uint32_t target = relative ? dsp->pc + ext : ext;
    if (sub) {
        xbranch_sub(dsp, target);
    } else {
        dsp->pc = target & BITMASK(24);
        dsp->cur_inst_len = 0;
    }
}

static void emu_jclr_qq(dsp_core_t *dsp) { xjump_if(dsp, !xqq_bit(dsp), false, false); }
static void emu_jset_qq(dsp_core_t *dsp) { xjump_if(dsp, xqq_bit(dsp), false, false); }
static void emu_jsclr_qq(dsp_core_t *dsp) { xjump_if(dsp, !xqq_bit(dsp), true, false); }
static void emu_jsset_qq(dsp_core_t *dsp) { xjump_if(dsp, xqq_bit(dsp), true, false); }
static void emu_brclr_qq(dsp_core_t *dsp) { xjump_if(dsp, !xqq_bit(dsp), false, true); }
static void emu_brset_qq(dsp_core_t *dsp) { xjump_if(dsp, xqq_bit(dsp), false, true); }
static void emu_bsclr_qq(dsp_core_t *dsp) { xjump_if(dsp, !xqq_bit(dsp), true, true); }
static void emu_bsset_qq(dsp_core_t *dsp) { xjump_if(dsp, xqq_bit(dsp), true, true); }

/* op: 0 clear, 1 set, 2 change, 3 test. C = original bit. */
static void xbitop_qq(dsp_core_t *dsp, int op)
{
    uint32_t addr = xqq_addr(dsp);
    int space = xqq_space(dsp);
    uint32_t bit = dsp->cur_inst & BITMASK(5);
    uint32_t v = dsp56k_read_memory(dsp, space, addr);
    xccr_set(dsp, DSP_SR_C, (v >> bit) & 1);
    if (op == 0) v &= ~(1u << bit);
    else if (op == 1) v |= 1u << bit;
    else if (op == 2) v ^= 1u << bit;
    if (op != 3) dsp56k_write_memory(dsp, space, addr, v);
    dsp->instr_cycle += 2;
}
static void emu_bclr_qq(dsp_core_t *dsp) { xbitop_qq(dsp, 0); }
static void emu_bset_qq(dsp_core_t *dsp) { xbitop_qq(dsp, 1); }
static void emu_bchg_qq(dsp_core_t *dsp) { xbitop_qq(dsp, 2); }
static void emu_btst_qq(dsp_core_t *dsp) { xbitop_qq(dsp, 3); }

/* ------------------------------------------------------------------ move Y:(Rn + xxxx) <-> R */
static void emu_move_y_long(dsp_core_t *dsp)
{
    uint32_t xxxx = read_memory_p(dsp, dsp->pc + 1);
    dsp->cur_inst_len++;

    int W = (dsp->cur_inst >> 6) & 1;
    uint32_t offreg = DSP_REG_R0 + ((dsp->cur_inst >> 8) & BITMASK(3));
    uint32_t numreg = dsp->cur_inst & BITMASK(6);
    uint32_t y_addr = (dsp->registers[offreg] + xxxx) & BITMASK(24);

    if (!W) {
        uint32_t value;
        if (numreg == DSP_REG_A || numreg == DSP_REG_B) {
            emu_pm_read_accu24(dsp, numreg, &value);
        } else {
            value = dsp->registers[numreg];
        }
        dsp56k_write_memory(dsp, DSP_SPACE_Y, y_addr, value);
    } else {
        dsp_write_reg(dsp, numreg, dsp56k_read_memory(dsp, DSP_SPACE_Y, y_addr));
    }
}
