/* Minimal VGA register file stand-in for the NV2A PRMCIO/PRMVIO blocks. */
#pragma once
#include "nv2a_compat.h"

typedef struct VGADisplayParams {
    uint32_t line_offset;
    uint32_t start_addr;
    uint32_t line_compare;
} VGADisplayParams;

typedef struct VGACommonState {
    uint8_t msr;
    uint8_t sr_index, sr[256];
    uint8_t gr_index, gr[256];
    uint8_t ar_index, ar[32], ar_flip_flop;
    uint8_t cr_index, cr[256];
    uint8_t dac_state;
    void (*get_resolution)(struct VGACommonState *s, int *width, int *height);
    void (*get_params)(struct VGACommonState *s, VGADisplayParams *params);
} VGACommonState;

#define VGA_ATT_W 0x3C0
#define VGA_ATT_R 0x3C1
#define VGA_MIS_W 0x3C2
#define VGA_SEQ_I 0x3C4
#define VGA_SEQ_D 0x3C5
#define VGA_MIS_R 0x3CC
#define VGA_GFX_I 0x3CE
#define VGA_GFX_D 0x3CF
#define VGA_CRT_IC 0x3D4
#define VGA_CRT_DC 0x3D5
#define VGA_IS1_RC 0x3DA
#define VGA_AR_ENABLE_DISPLAY 0x20
#define VGA_MIS_COLOR 0x01
#define VGA_SEQ_CLOCK_MODE 0x01
#define VGA_SR01_SCREEN_OFF 0x20
#define VGA_CRTC_LINE_COMPARE 0x18
#define VGA_CRTC_OVERFLOW 0x07
#define VGA_CRTC_MAX_SCAN 0x09

uint32_t vga_ioport_read(VGACommonState *s, uint32_t addr);
void vga_ioport_write(VGACommonState *s, uint32_t addr, uint32_t val);
static inline void vga_invalidate_scanlines(VGACommonState *s, int y1, int y2) {}
