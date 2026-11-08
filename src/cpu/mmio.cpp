// MMIO trap: decode the faulting x86 instruction and emulate it against a device.
#include "mmio.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct MmioRegion {
    uint32_t base, size;
    mmio_read_fn rd;
    mmio_write_fn wr;
    void *opaque;
    const char *name;
};
static MmioRegion g_regions[8];
static int g_nregions;
volatile uint32_t g_mmio_reads, g_mmio_writes;
uint32_t g_mmio_debug_lo = 0xFE850000, g_mmio_debug_hi = 0xFE854000; // EP X memory
static int g_mmio_debug_count;

void mmio_register(uint32_t base, uint32_t size, mmio_read_fn rd, mmio_write_fn wr, void *opaque, const char *name) {
    void *p = VirtualAlloc((void *)base, size, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void *)base) fatal("mmio_register(%s): cannot reserve %08x+%x (err %lu)", name, base, size, GetLastError());
    g_regions[g_nregions++] = {base, size, rd, wr, opaque, name};
    logf("MMIO region %s at %08x-%08x", name, base, base + size);
}

static MmioRegion *find_region(uint32_t a) {
    for (int i = 0; i < g_nregions; i++)
        if (a - g_regions[i].base < g_regions[i].size) return &g_regions[i];
    return nullptr;
}

// --- register access helpers ---------------------------------------------------
static DWORD *reg32(CONTEXT *c, int r) {
    switch (r & 7) {
    case 0: return &c->Eax;
    case 1: return &c->Ecx;
    case 2: return &c->Edx;
    case 3: return &c->Ebx;
    case 4: return &c->Esp;
    case 5: return &c->Ebp;
    case 6: return &c->Esi;
    default: return &c->Edi;
    }
}
static uint32_t get_reg(CONTEXT *c, int r, int size) {
    if (size == 1) {
        DWORD v = *reg32(c, r & 3);
        return (r & 4) ? (v >> 8) & 0xFF : v & 0xFF;
    }
    uint32_t v = *reg32(c, r);
    return size == 2 ? v & 0xFFFF : v;
}
static void set_reg(CONTEXT *c, int r, int size, uint32_t val) {
    if (size == 1) {
        DWORD *p = reg32(c, r & 3);
        if (r & 4) *p = (*p & ~0xFF00u) | ((val & 0xFF) << 8);
        else *p = (*p & ~0xFFu) | (val & 0xFF);
    } else if (size == 2) {
        DWORD *p = reg32(c, r);
        *p = (*p & ~0xFFFFu) | (val & 0xFFFF);
    } else {
        *reg32(c, r) = val;
    }
}

// --- flags ---------------------------------------------------------------------
enum { CF = 1, PF = 4, AF = 0x10, ZF = 0x40, SF = 0x80, OF = 0x800 };
static uint32_t mask_of(int size) { return size == 4 ? 0xFFFFFFFFu : (1u << (size * 8)) - 1; }
static uint32_t sign_of(int size) { return 1u << (size * 8 - 1); }
static uint32_t parity(uint32_t v) { return __builtin_parity(v & 0xFF) ? 0 : PF; }

static void set_flags_logic(CONTEXT *c, uint32_t r, int size) {
    r &= mask_of(size);
    uint32_t f = c->EFlags & ~(CF | PF | AF | ZF | SF | OF);
    if (!r) f |= ZF;
    if (r & sign_of(size)) f |= SF;
    f |= parity(r);
    c->EFlags = f;
}
static void set_flags_add(CONTEXT *c, uint32_t a, uint32_t b, uint32_t r, int size) {
    uint32_t m = mask_of(size), s = sign_of(size);
    a &= m, b &= m, r &= m;
    uint32_t f = c->EFlags & ~(CF | PF | AF | ZF | SF | OF);
    if (r < a) f |= CF;
    if (!r) f |= ZF;
    if (r & s) f |= SF;
    if (~(a ^ b) & (a ^ r) & s) f |= OF;
    if ((a ^ b ^ r) & 0x10) f |= AF;
    f |= parity(r);
    c->EFlags = f;
}
static void set_flags_sub(CONTEXT *c, uint32_t a, uint32_t b, uint32_t r, int size) {
    uint32_t m = mask_of(size), s = sign_of(size);
    a &= m, b &= m, r &= m;
    uint32_t f = c->EFlags & ~(CF | PF | AF | ZF | SF | OF);
    if (a < b) f |= CF;
    if (!r) f |= ZF;
    if (r & s) f |= SF;
    if ((a ^ b) & (a ^ r) & s) f |= OF;
    if ((a ^ b ^ r) & 0x10) f |= AF;
    f |= parity(r);
    c->EFlags = f;
}

// ALU op numbers follow the x86 /digit encoding: 0 add 1 or 2 adc 3 sbb 4 and 5 sub 6 xor 7 cmp
// Returns true if the result must be written back.
static bool alu(CONTEXT *c, int op, uint32_t a, uint32_t b, int size, uint32_t *out) {
    uint32_t m = mask_of(size), r;
    switch (op) {
    case 0: r = a + b; set_flags_add(c, a, b, r, size); break;
    case 1: r = a | b; set_flags_logic(c, r, size); break;
    case 4: r = a & b; set_flags_logic(c, r, size); break;
    case 5: r = a - b; set_flags_sub(c, a, b, r, size); break;
    case 6: r = a ^ b; set_flags_logic(c, r, size); break;
    case 7: r = a - b; set_flags_sub(c, a, b, r, size); *out = a; return false;
    case 8: r = a & b; set_flags_logic(c, r, size); *out = a; return false; // test
    default: return false;
    }
    *out = r & m;
    return true;
}

// --- decoder -------------------------------------------------------------------
// Returns length of ModRM + SIB + displacement bytes, and the reg field.
static int modrm_len(const uint8_t *p, int *reg) {
    uint8_t m = p[0];
    int mod = m >> 6, rm = m & 7, len = 1;
    *reg = (m >> 3) & 7;
    if (mod == 3) return 1;
    if (rm == 4) { // SIB
        uint8_t sib = p[1];
        len++;
        if (mod == 0 && (sib & 7) == 5) len += 4;
    } else if (mod == 0 && rm == 5) {
        len += 4;
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

static uint32_t dev_read(MmioRegion *r, uint32_t addr, int size) {
    g_mmio_reads++;
    return r->rd(r->opaque, addr - r->base, size) & mask_of(size);
}
static CONTEXT *g_cur_ctx;
static void dev_write(MmioRegion *r, uint32_t addr, uint32_t v, int size) {
    g_mmio_writes++;
    if (g_mmio_debug_lo <= addr && addr < g_mmio_debug_hi && g_mmio_debug_count < 40) {
        g_mmio_debug_count++;
        const uint8_t *ip = (const uint8_t *)g_cur_ctx->Eip;
        logf("MMIO dbg write %08x = %08x (size %d) eip=%08lx [%02x %02x %02x %02x %02x %02x] eax=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx",
             addr, v, size, g_cur_ctx->Eip, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], g_cur_ctx->Eax, g_cur_ctx->Ecx,
             g_cur_ctx->Edx, g_cur_ctx->Esi, g_cur_ctx->Edi);
    }
    r->wr(r->opaque, addr - r->base, v & mask_of(size), size);
}

static bool emulate(CONTEXT *c, MmioRegion *rg, uint32_t addr) {
    const uint8_t *ip = (const uint8_t *)c->Eip;
    const uint8_t *p = ip;
    int opsize = 4;
    for (;;) { // prefixes
        if (*p == 0x66) opsize = 2, p++;
        else if (*p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) p++;
        else break;
    }
    uint8_t op = *p++;
    int reg, n;
    uint32_t v, res;
    switch (op) {
    case 0x8B: // mov r, r/m
        n = modrm_len(p, &reg);
        set_reg(c, reg, opsize, dev_read(rg, addr, opsize));
        p += n;
        break;
    case 0x8A: // mov r8, r/m8
        n = modrm_len(p, &reg);
        set_reg(c, reg, 1, dev_read(rg, addr, 1));
        p += n;
        break;
    case 0x89: // mov r/m, r
        n = modrm_len(p, &reg);
        dev_write(rg, addr, get_reg(c, reg, opsize), opsize);
        p += n;
        break;
    case 0x88: // mov r/m8, r8
        n = modrm_len(p, &reg);
        dev_write(rg, addr, get_reg(c, reg, 1), 1);
        p += n;
        break;
    case 0xC7: // mov r/m, imm
        n = modrm_len(p, &reg);
        p += n;
        v = opsize == 2 ? *(const uint16_t *)p : *(const uint32_t *)p;
        dev_write(rg, addr, v, opsize);
        p += opsize;
        break;
    case 0xC6: // mov r/m8, imm8
        n = modrm_len(p, &reg);
        p += n;
        dev_write(rg, addr, *p++, 1);
        break;
    case 0xA1: // mov eax, moffs
        set_reg(c, 0, opsize, dev_read(rg, addr, opsize));
        p += 4;
        break;
    case 0xA0:
        set_reg(c, 0, 1, dev_read(rg, addr, 1));
        p += 4;
        break;
    case 0xA3: // mov moffs, eax
        dev_write(rg, addr, get_reg(c, 0, opsize), opsize);
        p += 4;
        break;
    case 0xA2:
        dev_write(rg, addr, get_reg(c, 0, 1), 1);
        p += 4;
        break;
    case 0x0F: {
        uint8_t op2 = *p++;
        n = modrm_len(p, &reg);
        p += n;
        if (op2 == 0xB6) set_reg(c, reg, 4, dev_read(rg, addr, 1));
        else if (op2 == 0xB7) set_reg(c, reg, 4, dev_read(rg, addr, 2));
        else if (op2 == 0xBE) set_reg(c, reg, 4, (uint32_t)(int8_t)dev_read(rg, addr, 1));
        else if (op2 == 0xBF) set_reg(c, reg, 4, (uint32_t)(int16_t)dev_read(rg, addr, 2));
        else return false;
        break;
    }
    // ALU r/m, r  (00 add, 08 or, 20 and, 28 sub, 30 xor, 38 cmp) and r, r/m variants
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39:
    case 0x00: case 0x08: case 0x20: case 0x28: case 0x30: case 0x38: {
        int size = (op & 1) ? opsize : 1;
        n = modrm_len(p, &reg);
        p += n;
        v = dev_read(rg, addr, size);
        if (alu(c, op >> 3, v, get_reg(c, reg, size), size, &res)) dev_write(rg, addr, res, size);
        break;
    }
    case 0x03: case 0x0B: case 0x23: case 0x2B: case 0x33: case 0x3B:
    case 0x02: case 0x0A: case 0x22: case 0x2A: case 0x32: case 0x3A: {
        int size = (op & 1) ? opsize : 1;
        n = modrm_len(p, &reg);
        p += n;
        v = dev_read(rg, addr, size);
        if (alu(c, op >> 3, get_reg(c, reg, size), v, size, &res)) set_reg(c, reg, size, res);
        break;
    }
    case 0x85: case 0x84: { // test r/m, r
        int size = op == 0x85 ? opsize : 1;
        n = modrm_len(p, &reg);
        p += n;
        alu(c, 8, dev_read(rg, addr, size), get_reg(c, reg, size), size, &res);
        break;
    }
    case 0x81: case 0x83: case 0x80: { // ALU r/m, imm
        int size = op == 0x80 ? 1 : opsize;
        n = modrm_len(p, &reg);
        p += n;
        uint32_t imm;
        if (op == 0x81) {
            imm = opsize == 2 ? *(const uint16_t *)p : *(const uint32_t *)p;
            p += opsize;
        } else {
            imm = (uint32_t)(int32_t)(int8_t)*p++;
        }
        v = dev_read(rg, addr, size);
        if (alu(c, reg, v, imm, size, &res)) dev_write(rg, addr, res, size);
        break;
    }
    case 0xF7: case 0xF6: { // test r/m, imm
        int size = op == 0xF7 ? opsize : 1;
        n = modrm_len(p, &reg);
        p += n;
        if (reg != 0) return false;
        uint32_t imm = size == 1 ? *p : (size == 2 ? *(const uint16_t *)p : *(const uint32_t *)p);
        p += size;
        alu(c, 8, dev_read(rg, addr, size), imm, size, &res);
        break;
    }
    case 0xFF: // push r/m (only /6)
        n = modrm_len(p, &reg);
        if (reg != 6) return false;
        p += n;
        v = dev_read(rg, addr, 4);
        c->Esp -= 4;
        *(uint32_t *)c->Esp = v;
        break;
    default:
        return false;
    }
    c->Eip = (DWORD)(uintptr_t)p;
    return true;
}

// String instructions (movs/stos/lods, optionally rep) where either side may be MMIO.
static bool emulate_string(CONTEXT *c) {
    const uint8_t *p = (const uint8_t *)c->Eip;
    int size = 4;
    bool rep = false;
    for (;;) {
        if (*p == 0xF3 || *p == 0xF2) rep = true, p++;
        else if (*p == 0x66) size = 2, p++;
        else break;
    }
    uint8_t op = *p++;
    if (op == 0xA4 || op == 0xAA || op == 0xAC) size = 1;
    else if (op != 0xA5 && op != 0xAB && op != 0xAD) return false;
    int step = (c->EFlags & 0x400) ? -size : size; // DF
    uint32_t count = rep ? c->Ecx : 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = 0;
        if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) { // read [esi]
            MmioRegion *r = find_region(c->Esi);
            if (r) v = dev_read(r, c->Esi, size);
            else memcpy(&v, (void *)(uintptr_t)c->Esi, size);
            c->Esi += step;
        } else {
            v = c->Eax & mask_of(size);
        }
        if (op == 0xAC || op == 0xAD) {
            set_reg(c, 0, size, v);
        } else { // write [edi]
            MmioRegion *r = find_region(c->Edi);
            if (r) dev_write(r, c->Edi, v, size);
            else memcpy((void *)(uintptr_t)c->Edi, &v, size);
            c->Edi += step;
        }
    }
    if (rep) c->Ecx = 0;
    c->Eip = (DWORD)(uintptr_t)p;
    return true;
}

// --- port I/O (in/out are privileged in user mode) -----------------------------
static uint32_t g_port_logged[64];
static int g_nport_logged;

static void log_port_once(uint16_t port, bool write, uint32_t val) {
    uint32_t key = port | (write ? 0x10000u : 0);
    for (int i = 0; i < g_nport_logged; i++)
        if (g_port_logged[i] == key) return;
    if (g_nport_logged < 64) g_port_logged[g_nport_logged++] = key;
    logf("port I/O: %s port %04x%s%08x", write ? "out" : "in", port, write ? " = " : "", write ? val : 0);
}

static uint32_t port_read(uint16_t port, int size) {
    log_port_once(port, false, 0);
    return 0;
}
static void port_write(uint16_t port, uint32_t val, int size) { log_port_once(port, true, val); }

static bool emulate_port_io(CONTEXT *c) {
    const uint8_t *p = (const uint8_t *)c->Eip;
    int size = 4;
    if (*p == 0x66) size = 2, p++;
    uint8_t op = *p++;
    uint16_t port;
    switch (op) {
    case 0xEE: port_write((uint16_t)c->Edx, c->Eax & 0xFF, 1); break;                    // out dx, al
    case 0xEF: port_write((uint16_t)c->Edx, c->Eax & mask_of(size), size); break;        // out dx, ax/eax
    case 0xE6: port = *p++; port_write(port, c->Eax & 0xFF, 1); break;                    // out imm8, al
    case 0xE7: port = *p++; port_write(port, c->Eax & mask_of(size), size); break;
    case 0xEC: set_reg(c, 0, 1, port_read((uint16_t)c->Edx, 1)); break;                   // in al, dx
    case 0xED: set_reg(c, 0, size, port_read((uint16_t)c->Edx, size)); break;            // in ax/eax, dx
    case 0xE4: port = *p++; set_reg(c, 0, 1, port_read(port, 1)); break;
    case 0xE5: port = *p++; set_reg(c, 0, size, port_read(port, size)); break;
    case 0xFA: case 0xFB: break;                                                          // cli / sti
    case 0x0F:
        if (*p == 0x09) { p++; break; }                                                   // wbinvd
        return false;
    default: return false;
    }
    c->Eip = (DWORD)(uintptr_t)p;
    return true;
}

// --- trap-and-patch -------------------------------------------------------------
// A trapped access costs ~9 us (exception dispatch). After the first fault at an instruction
// in Xbox code, the instruction is replaced by a jump to a per-site stub that calls the same
// emulator directly (~0.1 us). The stub keeps a copy of the original instruction bytes.
struct PatchSite {
    uint8_t insn[16]; // original instruction
    int len;
};
static PatchSite *g_sites;
static int g_nsites;
static uint8_t *g_stubs;
static const int kMaxSites = 4096, kStubSize = 32;
static CRITICAL_SECTION g_patch_lock;
static bool g_patch_enabled;

// plain memory as a pseudo-device, for patched instructions that sometimes address RAM
static uint32_t ram_rd(void *, uint32_t addr, unsigned size) {
    uint32_t v = 0;
    memcpy(&v, (void *)(uintptr_t)addr, size);
    return v;
}
static void ram_wr(void *, uint32_t addr, uint32_t v, unsigned size) { memcpy((void *)(uintptr_t)addr, &v, size); }
static MmioRegion g_ram_region = {0, 0xFFFFFFFFu, ram_rd, ram_wr, nullptr, "RAM"};

// effective address of the memory operand of an instruction emulate() understands
static uint32_t effective_address(const CONTEXT *c, const uint8_t *p) {
    for (;;) {
        if (*p == 0x66 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) p++;
        else break;
    }
    uint8_t op = *p++;
    if (op >= 0xA0 && op <= 0xA3) return *(const uint32_t *)p;
    if (op == 0x0F) p++;
    uint8_t m = *p++;
    int mod = m >> 6, rm = m & 7;
    uint32_t ea = 0;
    if (rm == 4) {
        uint8_t sib = *p++;
        int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4) ea += *reg32((CONTEXT *)c, index) << scale;
        if (base == 5 && mod == 0) {
            ea += *(const uint32_t *)p;
            p += 4;
        } else {
            ea += *reg32((CONTEXT *)c, base);
        }
    } else if (rm == 5 && mod == 0) {
        ea = *(const uint32_t *)p;
        p += 4;
    } else {
        ea = *reg32((CONTEXT *)c, rm);
    }
    if (mod == 1) ea += (uint32_t)(int32_t)(int8_t)*p;
    else if (mod == 2) ea += *(const uint32_t *)p;
    return ea;
}

// frame pushed by the stub: pushfd; pushad -> [edi esi ebp esp ebx edx ecx eax eflags]
struct StubFrame {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax, eflags;
};
extern "C" void __cdecl mmio_site_dispatch(int idx, StubFrame *f) {
    PatchSite *site = &g_sites[idx];
    CONTEXT c = {};
    c.Eax = f->eax, c.Ecx = f->ecx, c.Edx = f->edx, c.Ebx = f->ebx;
    c.Esp = f->esp + 4; // value before pushfd
    c.Ebp = f->ebp, c.Esi = f->esi, c.Edi = f->edi, c.EFlags = f->eflags;
    c.Eip = (DWORD)(uintptr_t)site->insn;
    uint32_t addr = effective_address(&c, site->insn);
    MmioRegion *rg = find_region(addr);
    g_cur_ctx = &c;
    if (!emulate(&c, rg ? rg : &g_ram_region, addr)) fatal("mmio patch site %d: emulate failed", idx);
    f->eax = c.Eax, f->ecx = c.Ecx, f->edx = c.Edx, f->ebx = c.Ebx;
    f->ebp = c.Ebp, f->esi = c.Esi, f->edi = c.Edi, f->eflags = c.EFlags;
}

static void try_patch_site(uint32_t eip, int len) {
    const uint8_t *ip = (const uint8_t *)(uintptr_t)eip;
    if (!g_patch_enabled || len < 5 || len > 16 || eip < 0x11000 || eip + len > 0x4B0000) return;
    const uint8_t *p = ip;
    while (*p == 0x66 || *p == 0x2E || *p == 0x3E || *p == 0x26 || *p == 0x36) p++;
    if (*p == 0xFF) return; // push r/m changes ESP
    EnterCriticalSection(&g_patch_lock);
    if (ip[0] == 0xE9 || g_nsites >= kMaxSites) { // already patched by another thread / full
        LeaveCriticalSection(&g_patch_lock);
        return;
    }
    int idx = g_nsites++;
    PatchSite *site = &g_sites[idx];
    memcpy(site->insn, ip, len);
    site->len = len;
    uint8_t *st = g_stubs + idx * kStubSize, *q = st;
    *q++ = 0x9C;                                                            // pushfd
    *q++ = 0x60;                                                            // pushad
    *q++ = 0x54;                                                            // push esp
    *q++ = 0x68; memcpy(q, &idx, 4); q += 4;                                // push idx
    int32_t rel = (int32_t)((uintptr_t)mmio_site_dispatch - (uintptr_t)(q + 5));
    *q++ = 0xE8; memcpy(q, &rel, 4); q += 4;                                // call dispatch
    *q++ = 0x83; *q++ = 0xC4; *q++ = 0x08;                                  // add esp, 8
    *q++ = 0x61;                                                            // popad
    *q++ = 0x9D;                                                            // popfd
    rel = (int32_t)(eip + len - (uint32_t)(uintptr_t)(q + 5));
    *q++ = 0xE9; memcpy(q, &rel, 4); q += 4;                                // jmp back
    FlushInstructionCache(GetCurrentProcess(), st, kStubSize);
    // site: jmp stub; nop padding. Write the tail first and the opcode byte last.
    uint8_t patch[16];
    patch[0] = 0xE9;
    rel = (int32_t)((uint32_t)(uintptr_t)st - (eip + 5));
    memcpy(patch + 1, &rel, 4);
    memset(patch + 5, 0x90, len - 5);
    DWORD old;
    VirtualProtect((void *)ip, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void *)(ip + 1), patch + 1, len - 1);
    __atomic_store_n((uint8_t *)ip, (uint8_t)0xE9, __ATOMIC_SEQ_CST);
    VirtualProtect((void *)ip, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)ip, len);
    LeaveCriticalSection(&g_patch_lock);
}

static LONG CALLBACK mmio_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_PRIV_INSTRUCTION) {
        CONTEXT *c = ep->ContextRecord;
        if (c->Eip >= 0x11000 && c->Eip < 0x4B0000 && emulate_port_io(c)) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uint32_t addr = (uint32_t)ep->ExceptionRecord->ExceptionInformation[1];
    MmioRegion *rg = find_region(addr);
    if (!rg) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = ep->ContextRecord;
    g_cur_ctx = c;
    uint32_t eip0 = c->Eip;
    if (emulate(c, rg, addr)) {
        try_patch_site(eip0, (int)(c->Eip - eip0));
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (!emulate_string(c)) {
        const uint8_t *ip = (const uint8_t *)c->Eip;
        fatal("MMIO %s: cannot emulate instruction at %08lx (%02x %02x %02x %02x %02x %02x) addr %08x", rg->name, c->Eip,
              ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], addr);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void mmio_init() {
    static bool done;
    if (done) return;
    done = true;
    InitializeCriticalSection(&g_patch_lock);
    g_patch_enabled = getenv("XB_NO_MMIO_PATCH") == nullptr;
    g_sites = (PatchSite *)calloc(kMaxSites, sizeof(PatchSite));
    g_stubs = (uint8_t *)VirtualAlloc(nullptr, kMaxSites * kStubSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    // First in the chain so MMIO faults never reach the crash logger
    AddVectoredExceptionHandler(1, mmio_handler);
}

extern "C" void mmio_register_c(uint32_t base, uint32_t size, mmio_read_fn rd, mmio_write_fn wr, void *opaque, const char *name) {
    mmio_init();
    mmio_register(base, size, rd, wr, opaque, name);
}
