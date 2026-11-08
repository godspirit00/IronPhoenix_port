// MCPX ACI (AC'97 controller) register model at 0xFEC00000.
// Audio itself is captured from the APU (GP/EP DSP output), so the AC'97 link only has to
// respond like real hardware: codec ready, mixer registers, bus-master DMA state.
#include "../common.h"
#include "../cpu/mmio.h"
#include <windows.h>
#include <initializer_list>

static uint16_t g_nam[0x80];     // codec mixer registers (16-bit, offset/2)
static uint8_t g_nabm[0x80];     // bus master registers
static CRITICAL_SECTION g_lock;
static int g_logged;

// Bus master channel layout (per AC'97 spec), Xbox adds S/PDIF at 0x70
enum { BM_BDBAR = 0x00, BM_CIV = 0x04, BM_LVI = 0x05, BM_SR = 0x06, BM_PICB = 0x08, BM_PIV = 0x0A, BM_CR = 0x0B };
enum { GLOB_CNT = 0x2C, GLOB_STA = 0x30, CAS = 0x34 };
enum { SR_DCH = 1, SR_CELV = 2, SR_LVBCI = 4, SR_BCIS = 8, SR_FIFOE = 16 };

static bool is_channel_base(uint32_t off) { return off == 0x00 || off == 0x10 || off == 0x20 || off == 0x70; }

static uint32_t aci_read(void *, uint32_t off, unsigned size) {
    uint32_t v = 0;
    EnterCriticalSection(&g_lock);
    if (off < 0x100) {
        v = g_nam[(off >> 1) & 0x7F];
        if (size == 4) v |= (uint32_t)g_nam[((off >> 1) + 1) & 0x7F] << 16;
    } else if (off < 0x180) {
        uint32_t o = off - 0x100;
        if (o == GLOB_STA) {
            v = 0x00000300; // primary + secondary codec ready
        } else {
            for (unsigned i = 0; i < size && o + i < 0x80; i++) v |= (uint32_t)g_nabm[o + i] << (8 * i);
        }
    }
    LeaveCriticalSection(&g_lock);
    if (g_logged < 40) {
        g_logged++;
        logf("ACI read  %03x/%u -> %08x", off, size, v);
    }
    return v;
}

static void aci_write(void *, uint32_t off, uint32_t val, unsigned size) {
    if (g_logged < 40) {
        g_logged++;
        logf("ACI write %03x/%u = %08x", off, size, val);
    }
    EnterCriticalSection(&g_lock);
    if (off < 0x100) {
        g_nam[(off >> 1) & 0x7F] = (uint16_t)val;
        if (size == 4) g_nam[((off >> 1) + 1) & 0x7F] = (uint16_t)(val >> 16);
    } else if (off < 0x180) {
        uint32_t o = off - 0x100;
        uint32_t base = o & ~0xFu;
        if (o < 0x80 && (o & 0xF) == BM_SR && is_channel_base(base)) {
            // status bits are write-1-to-clear
            g_nabm[o] &= ~(uint8_t)(val & (SR_LVBCI | SR_BCIS | SR_FIFOE));
        } else if (o < 0x80 && (o & 0xF) == BM_CR && is_channel_base(base)) {
            g_nabm[o] = (uint8_t)val;
            if (val & 2) { // reset registers
                memset(&g_nabm[base], 0, 0x0B);
                g_nabm[base + BM_SR] = SR_DCH;
                g_nabm[o] = 0;
            } else if (val & 1) {
                g_nabm[base + BM_SR] &= ~SR_DCH; // DMA running
            } else {
                g_nabm[base + BM_SR] |= SR_DCH;
            }
        } else {
            for (unsigned i = 0; i < size && o + i < 0x80; i++) g_nabm[o + i] = (uint8_t)(val >> (8 * i));
        }
    }
    LeaveCriticalSection(&g_lock);
}

void aci_init() {
    InitializeCriticalSection(&g_lock);
    for (uint32_t base : {0x00u, 0x10u, 0x20u, 0x70u}) g_nabm[base + BM_SR] = SR_DCH;
    g_nam[0x26 / 2] = 0x000F;   // powerdown status: all sections ready
    g_nam[0x7C / 2] = 0x8384;   // vendor id (SigmaTel-like)
    g_nam[0x7E / 2] = 0x7652;
    mmio_init();
    mmio_register(0xFEC00000, 0x10000, aci_read, aci_write, nullptr, "ACI");
}
