/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef HW_XBOX_MCPX_APU_INT_H
#define HW_XBOX_MCPX_APU_INT_H

#include "qemu/osdep.h"
#include <math.h>
#include <SDL3/SDL.h>
#include "qemu/thread.h"
#include "trace.h"
#include "apu.h"
#include "apu_regs.h"
#include "apu_debug.h"
#include "fpconv.h"
#include "vp/vp.h"
#include "dsp/gp_ep.h"

#define GET_MASK(v, mask) (((v) & (mask)) >> ctz32(mask))

#define SET_MASK(v, mask, val)                                       \
    do {                                                             \
        (v) &= ~(mask);                                              \
        (v) |= ((val) << ctz32(mask)) & (mask);                      \
    } while (0)

#define CASE_4(v, step)                                              \
    case (v):                                                        \
    case (v)+(step):                                                 \
    case (v)+(step)*2:                                               \
    case (v)+(step)*3

// #define DEBUG_MCPX

#ifdef DEBUG_MCPX
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

void mcpx_apu_host_set_irq(bool level);
void mcpx_apu_host_init(uint8_t *ram, uint32_t ram_size);

typedef struct MCPXAPUState {

    bool exiting;
    bool set_irq;

    QemuThread apu_thread;
    QemuMutex lock;
    QemuCond cond;
    QemuCond idle_cond;
    bool pause_requested;
    bool is_idle;

    MemoryRegion *ram;
    uint8_t *ram_ptr;
    MemoryRegion mmio;

    MCPXAPUVPState vp;
    MCPXAPUGPState gp;
    MCPXAPUEPState ep;

    uint32_t regs[0x20000];

    int ep_frame_div;
    int frame_work_acc_us;
    int frame_count;
    int64_t frame_count_time_us;
    int64_t next_frame_time_us;

    struct {
        struct {
            int backoff, ok, speedup;
        } pacing;
        struct {
            int64_t last_us;
            int64_t min_us, max_us, sum_us;
            int count;
        } deviation;
        int queued_bytes_min, queued_bytes_max;
        int64_t queued_bytes_sum;
        int queued_bytes_count;
    } throttle;

    struct {
        McpxApuDebugMonitorPoint point;
        int16_t frame_buf[256][2]; // 1 EP frame (0x400 bytes)
        SDL_AudioStream *stream;
        int queued_bytes_low, queued_bytes_high;
    } monitor;
} MCPXAPUState;

extern MCPXAPUState *g_state; // Used via debug handlers

/* Port change: CPU-side (MMIO) acquisitions of d->lock announce themselves so the frame thread,
 * which holds d->lock for whole frames and re-acquires it immediately, hands it over at the next
 * frame boundary. Without this a DSound DPC at DISPATCH_LEVEL could wait milliseconds. */
extern volatile long g_apu_cpu_waiters;
static inline void apu_lock_from_cpu(MCPXAPUState *d)
{
    __atomic_add_fetch(&g_apu_cpu_waiters, 1, __ATOMIC_SEQ_CST);
    qemu_mutex_lock(&d->lock);
    __atomic_sub_fetch(&g_apu_cpu_waiters, 1, __ATOMIC_SEQ_CST);
}
extern struct McpxApuDebug g_dbg, g_dbg_cache;
extern int g_dbg_voice_monitor;
extern uint64_t g_dbg_muted_voices[4];

void mcpx_debug_begin_frame(void);
void mcpx_debug_end_frame(void);

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp);
void mcpx_apu_monitor_finalize(MCPXAPUState *d);
void mcpx_apu_monitor_frame(MCPXAPUState *d);

#endif
