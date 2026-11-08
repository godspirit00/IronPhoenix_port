/*
 * NV2A device glue for the Iron Phoenix native runtime.
 * Replaces xemu's hw/xbox/nv2a/nv2a.c (PCI/QEMU device model) with:
 *   - MMIO window 0xFD000000 trapped through cpu/mmio.cpp
 *   - RAMIN (instance memory) as plain RAM at 0xFD700000
 *   - VRAM = the Xbox physical memory window at 0x80000000
 *   - IRQ delivery thread calling the D3D interrupt service routine
 *   - vblank thread driving PCRTC
 *   - dirty page tracking via Win32 write watching
 *
 * Portions copyright (c) 2012 espes, (c) 2015 Jannik Vogel, (c) 2018-2025 Matt Borgerson
 * (GNU LGPL v2+, see original xemu sources).
 */
#include "nv2a_int.h"
int g_sp_rebind, g_sp_memdirty, g_sp_create;
extern int g_sp_park, g_sp_restore, g_sp_falsedirty;
#include <windows.h>

#define NV2A_MMIO_BASE 0xFD000000u
#define NV2A_MMIO_SIZE 0x01000000u
#define NV2A_RAMIN_OFFSET 0x700000u
#define NV2A_RAMIN_SIZE 0x100000u
#define XBOX_GPU_IRQ 3

/* provided by the C++ runtime */
extern bool ke_fire_interrupt_c(uint32_t irq);
extern void mmio_register_c(uint32_t base, uint32_t size, uint32_t (*rd)(void *, uint32_t, unsigned),
                            void (*wr)(void *, uint32_t, uint32_t, unsigned), void *opaque, const char *name);

static NV2AState *g_dev;
static MemoryRegion g_vram;
static CRITICAL_SECTION g_bql;
static HANDLE g_irq_event;
static volatile bool g_irq_asserted;

/* ---------------------------------------------------------------- BQL */
void nv2a_bql_lock(void) { EnterCriticalSection(&g_bql); }
void nv2a_bql_unlock(void) { LeaveCriticalSection(&g_bql); }

/* ---------------------------------------------------------------- IRQ */
void nv2a_update_irq(NV2AState *d)
{
    /* PFIFO */
    if (d->pfifo.pending_interrupts & d->pfifo.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PFIFO;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PFIFO;
    }

    /* PCRTC */
    if (d->pcrtc.pending_interrupts & d->pcrtc.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PCRTC;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PCRTC;
    }

    /* PGRAPH */
    if (d->pgraph.pending_interrupts & d->pgraph.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PGRAPH;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PGRAPH;
    }

    /* PTIMER */
    if (d->ptimer.pending_interrupts & d->ptimer.enabled_interrupts) {
        d->pmc.pending_interrupts |= NV_PMC_INTR_0_PTIMER;
    } else {
        d->pmc.pending_interrupts &= ~NV_PMC_INTR_0_PTIMER;
    }

    bool assert_irq = d->pmc.pending_interrupts && d->pmc.enabled_interrupts;
    g_irq_asserted = assert_irq;
    if (assert_irq) {
        SetEvent(g_irq_event);
    }
}

/* Level-triggered interrupt line: keep calling the ISR while the line is asserted. */
static DWORD WINAPI irq_thread(void *arg)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        WaitForSingleObject(g_irq_event, INFINITE);
        for (int spins = 0; g_irq_asserted; spins++) {
            if (!ke_fire_interrupt_c(XBOX_GPU_IRQ)) {
                break; /* no ISR connected yet */
            }
            if (spins > 4) {
                Sleep(0);
            }
            if (spins > 1000) {
                break;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- DMA objects */
DMAObject nv_dma_load(NV2AState *d, hwaddr dma_obj_address)
{
    assert(dma_obj_address < memory_region_size(&d->ramin));

    uint32_t *dma_obj = (uint32_t *)(d->ramin_ptr + dma_obj_address);
    uint32_t flags = ldl_le_p(dma_obj);
    uint32_t limit = ldl_le_p(dma_obj + 1);
    uint32_t frame = ldl_le_p(dma_obj + 2);

    return (DMAObject){
        .dma_class  = GET_MASK(flags, NV_DMA_CLASS),
        .dma_target = GET_MASK(flags, NV_DMA_TARGET),
        .address    = (frame & NV_DMA_ADDRESS) | GET_MASK(flags, NV_DMA_ADJUST),
        .limit      = limit,
    };
}

void *nv_dma_map(NV2AState *d, hwaddr dma_obj_address, hwaddr *len)
{
    DMAObject dma = nv_dma_load(d, dma_obj_address);

    /* TODO: Handle targets and classes properly */
    dma.address &= 0x07FFFFFF;

    assert(dma.address < memory_region_size(d->vram));
    *len = dma.limit;
    return d->vram_ptr + dma.address;
}

hwaddr nv_clip_gpu_tile_blit(NV2AState *d, hwaddr blit_base_address, hwaddr len)
{
    const uint32_t *regs = d->pfb.regs;
    hwaddr blit_end = blit_base_address + len;
    for (int i = 0; i < NV_NUM_GPU_TILES; ++i) {
        uint32_t base_and_flags = regs[NV_PFB_TILE_BASE_ADDRESS_AND_FLAGS(i)];
        if (!(base_and_flags & NV_PFB_TILE_FLAGS_VALID)) {
            continue;
        }

        uint32_t limit = regs[NV_PFB_TILE_LIMIT(i)];

        if (blit_base_address < limit && blit_end > limit) {
            return limit + 1 - blit_base_address;
        }
    }

    return len;
}

/* ---------------------------------------------------------------- MMIO */
const NV2ABlockInfo blocktable[NV_NUM_BLOCKS] = {
#define ENTRY(NAME, LNAME, OFFSET, SIZE) [NV_##NAME] = {            \
        .name   = #NAME,                                                \
        .offset = OFFSET,                                               \
        .size   = SIZE,                                                 \
        .ops    = { .read = LNAME ## _read, .write = LNAME ## _write }, \
    }
    ENTRY(PMC,      pmc,      0x000000, 0x001000),
    ENTRY(PBUS,     pbus,     0x001000, 0x001000),
    ENTRY(PFIFO,    pfifo,    0x002000, 0x002000),
    ENTRY(PRMA,     prma,     0x007000, 0x001000),
    ENTRY(PVIDEO,   pvideo,   0x008000, 0x001000),
    ENTRY(PTIMER,   ptimer,   0x009000, 0x001000),
    ENTRY(PCOUNTER, pcounter, 0x00a000, 0x001000),
    ENTRY(PVPE,     pvpe,     0x00b000, 0x001000),
    ENTRY(PTV,      ptv,      0x00d000, 0x001000),
    ENTRY(PRMFB,    prmfb,    0x0a0000, 0x020000),
    ENTRY(PRMVIO,   prmvio,   0x0c0000, 0x001000),
    ENTRY(PFB,      pfb,      0x100000, 0x001000),
    ENTRY(PSTRAPS,  pstraps,  0x101000, 0x001000),
    ENTRY(PGRAPH,   pgraph,   0x400000, 0x002000),
    ENTRY(PCRTC,    pcrtc,    0x600000, 0x001000),
    ENTRY(PRMCIO,   prmcio,   0x601000, 0x001000),
    ENTRY(PRAMDAC,  pramdac,  0x680000, 0x001000),
    ENTRY(PRMDIO,   prmdio,   0x681000, 0x001000),
    ENTRY(USER,     user,     0x800000, 0x800000),
#undef ENTRY
};

static int g_unknown_logged;

/* debug (XB_MMIO_SLOW=1): log NV2A register accesses that block for over 1 ms */
static void mmio_slow_check(LARGE_INTEGER t0, uint32_t addr, int write)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("XB_MMIO_SLOW") != NULL;
    }
    if (!enabled) {
        return;
    }
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    static volatile LONG n;
    if (ms > 1.0 && InterlockedIncrement(&n) < 300) {
        nv2a_log("mmio slow: %s %06x %.1f ms", write ? "write" : "read", addr, ms);
    }
}

static uint32_t nv2a_mmio_read(void *opaque, uint32_t addr, unsigned size)
{
    NV2AState *d = opaque;
    for (int i = 0; i < ARRAY_SIZE(blocktable); i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (b->name && addr >= b->offset && addr < b->offset + b->size) {
            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);
            nv2a_bql_lock();
            uint64_t v = b->ops.read(d, addr - b->offset, size);
            nv2a_bql_unlock();
            mmio_slow_check(t0, addr, 0);
            return (uint32_t)v;
        }
    }
    if (g_unknown_logged++ < 64) {
        nv2a_log("nv2a: read from unmapped MMIO offset %06x (size %u)", addr, size);
    }
    return 0;
}

static void nv2a_mmio_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    NV2AState *d = opaque;
    for (int i = 0; i < ARRAY_SIZE(blocktable); i++) {
        const NV2ABlockInfo *b = &blocktable[i];
        if (b->name && addr >= b->offset && addr < b->offset + b->size) {
            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);
            nv2a_bql_lock();
            b->ops.write(d, addr - b->offset, val, size);
            nv2a_bql_unlock();
            mmio_slow_check(t0, addr, 1);
            return;
        }
    }
    if (g_unknown_logged++ < 64) {
        nv2a_log("nv2a: write to unmapped MMIO offset %06x = %08x (size %u)", addr, val, size);
    }
}

/* ---------------------------------------------------------------- VGA stand-in */
static struct {
    uint32_t framebuffer;
    uint32_t pitch;
    uint32_t format;
    int width, height;
} g_display = {0, 2560, 0, 640, 480};

static void nv2a_get_resolution(VGACommonState *s, int *width, int *height)
{
    /* Mode set by the kernel's AvSetDisplayMode (we skip CRTC register programming) */
    *width = g_display.width;
    *height = g_display.height;
}

static void nv2a_get_params(VGACommonState *s, VGADisplayParams *params)
{
    NV2AState *d = container_of(s, NV2AState, vga);
    params->line_offset = g_display.pitch;
    params->start_addr = d->pcrtc.start / 4;
    params->line_compare = 0;
}

uint32_t vga_ioport_read(VGACommonState *s, uint32_t addr)
{
    switch (addr) {
    case VGA_ATT_W: return s->ar_index;
    case VGA_ATT_R: return s->ar[s->ar_index & 0x1F];
    case VGA_MIS_R: return s->msr;
    case VGA_SEQ_I: return s->sr_index;
    case VGA_SEQ_D: return s->sr[s->sr_index];
    case VGA_GFX_I: return s->gr_index;
    case VGA_GFX_D: return s->gr[s->gr_index];
    case VGA_CRT_IC: case 0x3B4: return s->cr_index;
    case VGA_CRT_DC: case 0x3B5: return s->cr[s->cr_index];
    case VGA_IS1_RC: case 0x3BA: {
        /* Input status 1: toggle retrace bits so polling loops make progress */
        static uint8_t toggle;
        s->ar_flip_flop = 0;
        toggle ^= 0x09;
        return toggle;
    }
    default: return 0xFF;
    }
}

void vga_ioport_write(VGACommonState *s, uint32_t addr, uint32_t val)
{
    switch (addr) {
    case VGA_ATT_W:
        if (s->ar_flip_flop == 0) {
            s->ar_index = val & 0x3F;
        } else {
            s->ar[s->ar_index & 0x1F] = val;
        }
        s->ar_flip_flop ^= 1;
        break;
    case VGA_MIS_W: s->msr = val; break;
    case VGA_SEQ_I: s->sr_index = val; break;
    case VGA_SEQ_D: s->sr[s->sr_index] = val; break;
    case VGA_GFX_I: s->gr_index = val; break;
    case VGA_GFX_D: s->gr[s->gr_index] = val; break;
    case VGA_CRT_IC: case 0x3B4: s->cr_index = val; break;
    case VGA_CRT_DC: case 0x3B5: s->cr[s->cr_index] = val; break;
    default: break;
    }
}

const uint8_t *nv2a_get_dac_palette(void) { return g_nv2a->puserdac.palette; }
int nv2a_get_screen_off(void) { return 0; }

/* Called by the kernel's AvSetDisplayMode */
void nv2a_av_set_display_mode(uint32_t framebuffer, uint32_t pitch, uint32_t format, int width, int height)
{
    g_display.width = width;
    g_display.height = height;
    if (g_dev) {
        /* 1080i: xemu doubles the field height itself, so report a field and flag interlace */
        g_dev->vga.cr[NV_PRMCIO_INTERLACE_MODE] = height == 1080 ? 0 : NV_PRMCIO_INTERLACE_MODE_DISABLED;
        if (height == 1080) g_display.height = 540;
    }
    g_display.framebuffer = framebuffer;
    g_display.pitch = pitch;
    g_display.format = format;
    if (g_dev) {
        nv2a_bql_lock();
        g_dev->pcrtc.start = framebuffer & 0x0FFFFFFF;
        nv2a_bql_unlock();
    }
}

/* ---------------------------------------------------------------- vblank */
static volatile int g_refresh_hz = 60;
volatile uint32_t g_nv2a_vblank_count;

static DWORD WINAPI vblank_thread(void *arg)
{
    NV2AState *d = arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    uint64_t frame = 0;
    for (;;) {
        frame++;
        int64_t target = start.QuadPart + (int64_t)(frame * (double)freq.QuadPart / g_refresh_hz);
        QueryPerformanceCounter(&now);
        int64_t wait_100ns = (target - now.QuadPart) * 10000000 / freq.QuadPart;
        if (wait_100ns > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -wait_100ns;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else if (wait_100ns < -10000000) {
            QueryPerformanceCounter(&start); /* fell far behind: resync */
            frame = 0;
        }
        nv2a_bql_lock();
        d->pcrtc.pending_interrupts |= NV_PCRTC_INTR_0_VBLANK;
        d->pcrtc.raster = 0;
        nv2a_update_irq(d);
        nv2a_bql_unlock();
        g_nv2a_vblank_count++;
        if (g_nv2a_vblank_count % 120 == 0) {
            char buf[1024];
            int n = 0;
            for (unsigned i = 0; i < NV2A_PROF__COUNT && n < (int)sizeof buf - 64; i++) {
                int v = nv2a_profile_get_counter_value(i);
                if (v) n += snprintf(buf + n, sizeof buf - n, "%s=%d ", nv2a_profile_get_counter_name(i), v);
            }
            extern volatile LONG g_ww_sync_calls, g_ww_discard_calls;
            extern volatile LONG64 g_ww_ticks;
            extern volatile LONG g_ww_queries;
            extern volatile uint32_t g_nv2a_kick_epoch;
            static uint32_t last_epoch;
            extern volatile LONG g_nop_count;
            extern volatile LONG64 g_nop_wait_ticks;
            {
                LARGE_INTEGER qf2;
                QueryPerformanceFrequency(&qf2);
                LONG nn = InterlockedExchange(&g_nop_count, 0);
                double wt = InterlockedExchange64(&g_nop_wait_ticks, 0) * 1000.0 / qf2.QuadPart;
                nv2a_log("dirty: epochs %u queries %ld | nop irqs %ld, total wait %.1f ms (avg %.3f ms)", g_nv2a_kick_epoch - last_epoch,
                         InterlockedExchange(&g_ww_queries, 0), nn, wt, nn ? wt / nn : 0.0);
            }
            last_epoch = g_nv2a_kick_epoch;
            extern volatile long g_reports_deferred, g_reports_blocking;
            extern void hle_vis_stats(char *buf, int n);
            char vis[160];
            hle_vis_stats(vis, sizeof vis);
            extern void ke_isr_stats(char *buf, int n);
            char isr[160];
            ke_isr_stats(isr, sizeof isr);
            nv2a_log("%s | %s", vis, isr);
            {
                static const char *names[PS__COUNT] = {"draw_begin", "draw_end", "flush_draw", "clear", "surf_update", "blit",
                                                       "textures", "shaders", "vtx_attrs", "pending", "reports", "sync", "PUSHER", "surf_memcheck", "surf_part", "surf_dl", "surf_ul", "surf_inval"};
                char pb[1200];
                int pn = 0;
                LARGE_INTEGER pf;
                QueryPerformanceFrequency(&pf);
                for (int i = 0; i < PS__COUNT; i++) {
                    pn += snprintf(pb + pn, sizeof pb - pn, "%s %.0fms/%d ", names[i], g_ps_ticks[i] * 1000.0 / pf.QuadPart, g_ps_calls[i]);
                    g_ps_ticks[i] = 0;
                    g_ps_calls[i] = 0;
                }
                nv2a_log("gpu time: %s | rebind %d memdirty %d (unchanged %d) create %d park %d restore %d", pb, g_sp_rebind, g_sp_memdirty, g_sp_falsedirty, g_sp_create, g_sp_park, g_sp_restore);
                g_sp_rebind = g_sp_memdirty = g_sp_create = g_sp_park = g_sp_restore = g_sp_falsedirty = 0;
                extern void debug_gect_report(void);
                debug_gect_report();
            }
            LARGE_INTEGER qf;
            QueryPerformanceFrequency(&qf);
            nv2a_log("perf: %d fps, last frame %d ms | ww sync %ld (%.1f ms) discard %ld | reports deferred %ld blocking %ld | %s", g_nv2a_stats.increment_fps,
                     g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1) % NV2A_PROF_NUM_FRAMES].mspf,
                     InterlockedExchange(&g_ww_sync_calls, 0), InterlockedExchange64(&g_ww_ticks, 0) * 1000.0 / qf.QuadPart,
                     InterlockedExchange(&g_ww_discard_calls, 0), InterlockedExchange(&g_reports_deferred, 0),
                     InterlockedExchange(&g_reports_blocking, 0), buf);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- dirty tracking */
#define VRAM_PAGES (128u * 1024 * 1024 / 4096)
static uint8_t g_dirty[DIRTY_MEMORY_NUM][VRAM_PAGES];
static SRWLOCK g_dirty_lock = SRWLOCK_INIT;
static void *g_watch_buf[VRAM_PAGES];

/* Pull CPU writes recorded by the OS into the per-client dirty maps, only for the range being
 * queried (scanning all of device RAM on every kick cost a third of the GPU thread). Any page
 * reported is marked dirty for every client, so per-client semantics match a global scan. */
volatile LONG g_ww_sync_calls, g_ww_discard_calls;
volatile LONG64 g_ww_ticks;
extern volatile uint32_t g_nv2a_kick_epoch;
static uint32_t g_page_epoch[VRAM_PAGES]; /* kick epoch (+1) in which the page was last scanned */

/* Note: the buffer size passed to GetWriteWatch matters a lot in a 32-bit process - the WOW64
 * thunk processes a temporary buffer of that size (count 32768: ~50 us per call, even for one
 * page). Pass exactly the number of pages in the range. */
static void sync_run(uint32_t first, uint32_t end_page)
{
    ULONG_PTR count = end_page - first;
    DWORD gran;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, g_vram.ptr + ((size_t)first << 12), (size_t)(end_page - first) << 12, g_watch_buf, &count,
                      &gran) == 0) {
        for (ULONG_PTR i = 0; i < count; i++) {
            uint32_t page = (uint32_t)(((uint8_t *)g_watch_buf[i] - g_vram.ptr) >> 12);

            for (int c = 0; c < DIRTY_MEMORY_NUM; c++) {
                g_dirty[c][page] = 1;
            }
        }
    }
}

/* Every CPU write the GPU must observe happened before the push buffer was kicked, so a page
 * only needs scanning once per kick epoch; ranges already scanned this epoch cost nothing. */
volatile LONG g_ww_queries, g_ww_pages;
int64_t g_ps_ticks[PS__COUNT];
int g_ps_calls[PS__COUNT];
int64_t ps_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
LARGE_INTEGER g_nop_raised;
volatile LONG g_nop_count;
volatile LONG64 g_nop_wait_ticks;
static void nv2a_dirty_sync_range(hwaddr addr, hwaddr size)
{
    InterlockedIncrement(&g_ww_queries);
    uint32_t stamp = __atomic_load_n(&g_nv2a_kick_epoch, __ATOMIC_SEQ_CST) + 1;
    uint32_t first = (uint32_t)(addr >> 12), end = (uint32_t)((addr + size + 0xFFF) >> 12);
    if (end > VRAM_PAGES) {
        end = VRAM_PAGES;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    AcquireSRWLockExclusive(&g_dirty_lock);
    uint32_t run = UINT32_MAX;
    for (uint32_t p = first; p < end; p++) {
        if (g_page_epoch[p] != stamp) {
            g_page_epoch[p] = stamp;
            if (run == UINT32_MAX) {
                run = p;
            }
        } else if (run != UINT32_MAX) {
            sync_run(run, p);
            InterlockedIncrement(&g_ww_sync_calls);
            run = UINT32_MAX;
        }
    }
    if (run != UINT32_MAX) {
        sync_run(run, end);
        InterlockedIncrement(&g_ww_sync_calls);
    }
    ReleaseSRWLockExclusive(&g_dirty_lock);
    QueryPerformanceCounter(&t1);
    InterlockedAdd64(&g_ww_ticks, t1.QuadPart - t0.QuadPart);
}

/* Forget OS write records for a range the GPU just wrote itself. */
void nv2a_dirty_discard(hwaddr addr, hwaddr size)
{
    DWORD gran;
    uintptr_t start = (uintptr_t)g_vram.ptr + (addr & ~0xFFFull);
    uintptr_t end = (uintptr_t)g_vram.ptr + ((addr + size + 0xFFF) & ~0xFFFull);
    ULONG_PTR count = (end - start) >> 12;
    InterlockedIncrement(&g_ww_discard_calls);
    AcquireSRWLockExclusive(&g_dirty_lock); /* g_watch_buf is shared */
    GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)start, end - start, g_watch_buf, &count, &gran);
    ReleaseSRWLockExclusive(&g_dirty_lock);
}

bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    if (mr != &g_vram || !size) {
        return false;
    }
    nv2a_dirty_sync_range(addr, size);
    bool dirty = false;
    uint32_t first = (uint32_t)(addr >> 12), last = (uint32_t)((addr + size - 1) >> 12);
    AcquireSRWLockExclusive(&g_dirty_lock);
    for (uint32_t p = first; p <= last && p < VRAM_PAGES; p++) {
        if (g_dirty[client][p]) {
            dirty = true;
            g_dirty[client][p] = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_dirty_lock);
    return dirty;
}

bool memory_region_get_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    if (mr != &g_vram || !size) {
        return false;
    }
    nv2a_dirty_sync_range(addr, size);
    uint32_t first = (uint32_t)(addr >> 12), last = (uint32_t)((addr + size - 1) >> 12);
    for (uint32_t p = first; p <= last && p < VRAM_PAGES; p++) {
        if (g_dirty[client][p]) {
            return true;
        }
    }
    return false;
}

void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client)
{
    if (mr != &g_vram || !size) {
        return;
    }
    /* GPU-side write (surface download, blit): the renderer has just written this range itself.
     * Drop the OS write records instead of merging them - merging counted the GPU's own data as
     * CPU writes, marking the surface dirty for the GPU too, so render targets that alias each
     * other were downloaded, re-uploaded and recreated every frame. Whatever the CPU wrote here
     * before is superseded by the GPU's data. */
    nv2a_dirty_discard(addr, size);
    uint32_t first = (uint32_t)(addr >> 12), last = (uint32_t)((addr + size - 1) >> 12);
    AcquireSRWLockExclusive(&g_dirty_lock);
    for (uint32_t p = first; p <= last && p < VRAM_PAGES; p++) {
        g_dirty[client][p] = 1;
    }
    ReleaseSRWLockExclusive(&g_dirty_lock);
}

void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size)
{
    if (mr != &g_vram || !size) {
        return;
    }

    uint32_t first = (uint32_t)(addr >> 12), last = (uint32_t)((addr + size - 1) >> 12);
    AcquireSRWLockExclusive(&g_dirty_lock);
    for (uint32_t p = first; p <= last && p < VRAM_PAGES; p++) {
        for (int c = 0; c < DIRTY_MEMORY_NUM; c++) {
            g_dirty[c][p] = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_dirty_lock);
}

/* ---------------------------------------------------------------- CPU access callbacks
 * xemu (under TCG) gets a callback whenever the CPU touches memory backing a GPU surface, so it
 * downloads render targets lazily. Native code is not instrumented, so after every push buffer
 * batch the surface pages are made PAGE_NOACCESS; the first Xbox-code access faults, we run the
 * callback (which downloads the surface if the GPU drew into it) and unprotect. Faults from our
 * own threads (GPU/APU emulation) just unprotect. */
struct MemAccessCallback {
    hwaddr addr, len;
    MemAccessCallbackFunc *cb;
    void *opaque;
    MemoryRegion *mr;
    bool armed;
    struct MemAccessCallback *next;
};
static struct MemAccessCallback *g_access_cbs;
static SRWLOCK g_access_lock = SRWLOCK_INIT;
static volatile bool g_access_handler_installed;

static void protect_range(hwaddr addr, hwaddr len, DWORD prot)
{
    uintptr_t start = (uintptr_t)g_vram.ptr + (addr & ~0xFFFull);
    uintptr_t end = (uintptr_t)g_vram.ptr + ((addr + len + 0xFFF) & ~0xFFFull);
    DWORD old;
    VirtualProtect((void *)start, end - start, prot, &old);
}

/* Disarm every callback overlapping [addr, addr+len). Returns callbacks to invoke (Xbox access).
 * A read only re-enables reading: pages that only ever held zeros (GPU-only render targets) are
 * reported by GetWriteWatch as *written* when they are read after a NOACCESS -> READWRITE change,
 * which made every such surface look CPU-modified and re-uploaded on each bind. Pages go
 * READONLY -> READWRITE on the first real write (a second, cheap fault). */
static int disarm_overlapping(hwaddr addr, hwaddr len, struct MemAccessCallback **hits, int max, bool write)
{
    int n = 0;
    AcquireSRWLockExclusive(&g_access_lock);
    for (struct MemAccessCallback *c = g_access_cbs; c; c = c->next) {
        if (c->addr < addr + len && addr < c->addr + c->len) {
            if (c->armed) {
                c->armed = false;
                protect_range(c->addr, c->len, write ? PAGE_READWRITE : PAGE_READONLY);
            }
            if (n < max) hits[n++] = c;
        }
    }
    if (write && n) {
        protect_range(addr, len, PAGE_READWRITE); /* also pages left READONLY by an earlier read */
    }
    ReleaseSRWLockExclusive(&g_access_lock);
    return n;
}

static void run_callbacks(hwaddr addr, hwaddr len, bool write)
{
    struct MemAccessCallback *hits[16];
    int n = disarm_overlapping(addr, len, hits, 16, write);
    for (int i = 0; i < n; i++) {
        hits[i]->cb(hits[i]->opaque, hits[i]->mr, addr, len, write);
    }
}

static LONG CALLBACK access_fault_handler(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    if (a < (uintptr_t)g_vram.ptr || a >= (uintptr_t)g_vram.ptr + g_vram.size) return EXCEPTION_CONTINUE_SEARCH;
    hwaddr addr = (a - (uintptr_t)g_vram.ptr) & ~0xFFFull;
    bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    uint32_t eip = ep->ContextRecord->Eip;
    {
        static int logged = -1;
        if (logged < 0) logged = getenv("XB_FAULT_LOG") ? 0 : 1000000;
        if (logged < 400) {
            logged++;
            char mod[MAX_PATH] = "";
            HMODULE hm;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)(uintptr_t)eip, &hm)) {
                GetModuleFileNameA(hm, mod, sizeof mod);
            }
            uint32_t *sp = (uint32_t *)ep->ContextRecord->Esp;
            nv2a_log("surface fault: %s addr %08llx eip %08x tid %lu %s ret %08x %08x %08x %08x", write ? "W" : "R",
                     (unsigned long long)addr, eip, GetCurrentThreadId(), mod, sp[0], sp[1], sp[2], sp[3]);
        }
    }
    MEMORY_BASIC_INFORMATION mbi;
    VirtualQuery((void *)a, &mbi, sizeof mbi);
    if ((mbi.Protect & PAGE_READONLY) && write) {
        /* write to a page a read disarmed: let it write; Xbox writes still notify the surfaces */
        if (eip >= 0x11000 && eip < 0x4B0000) {
            run_callbacks(addr, 0x1000, true);
        }
        protect_range(addr, 0x1000, PAGE_READWRITE);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (eip >= 0x11000 && eip < 0x4B0000) {
        run_callbacks(addr, 0x1000, write);          /* Xbox code touched a surface */
    } else {
        struct MemAccessCallback *hits[1];
        disarm_overlapping(addr, 0x1000, hits, 0, write);   /* our own emulation threads */
        if (write) {
            protect_range(addr, 0x1000, PAGE_READWRITE);
        }
    }
    /* If nothing was armed here the fault is not ours */
    VirtualQuery((void *)a, &mbi, sizeof mbi);
    return (mbi.Protect & PAGE_NOACCESS) ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_CONTINUE_EXECUTION;
}

MemAccessCallback *mem_access_callback_insert(void *cpu, MemoryRegion *mr, hwaddr addr, hwaddr len,
                                              MemAccessCallbackFunc *cb, void *opaque)
{
    if (!g_access_handler_installed) {
        g_access_handler_installed = true;
        AddVectoredExceptionHandler(1, access_fault_handler);
    }
    struct MemAccessCallback *c = g_malloc0(sizeof(*c));
    c->addr = addr;
    c->len = len;
    c->cb = cb;
    c->opaque = opaque;
    c->mr = mr;
    AcquireSRWLockExclusive(&g_access_lock);
    c->next = g_access_cbs;
    g_access_cbs = c;
    ReleaseSRWLockExclusive(&g_access_lock);
    return c;
}

void mem_access_callback_remove_by_ref(void *cpu, MemAccessCallback *cb)
{
    if (!cb) return;
    AcquireSRWLockExclusive(&g_access_lock);
    for (struct MemAccessCallback **pp = &g_access_cbs; *pp; pp = &(*pp)->next) {
        if (*pp == cb) {
            *pp = cb->next;
            protect_range(cb->addr, cb->len, PAGE_READWRITE); /* armed, or READONLY after a read */
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_access_lock);
    g_free(cb);
}

void nv2a_arm_access_callbacks(void)
{
    AcquireSRWLockExclusive(&g_access_lock);
    for (struct MemAccessCallback *c = g_access_cbs; c; c = c->next) {
        if (!c->armed) {
            c->armed = true;
            protect_range(c->addr, c->len, PAGE_NOACCESS);
        }
    }
    ReleaseSRWLockExclusive(&g_access_lock);
}

/* Called by kernel I/O before host APIs touch Xbox memory (syscalls fail instead of faulting). */
void nv2a_host_cpu_access(uint32_t va, uint32_t len, bool write)
{
    uintptr_t base = (uintptr_t)g_vram.ptr;
    if (!len || va + len <= base || va >= base + g_vram.size) return;
    hwaddr lo = va < base ? 0 : va - base;
    hwaddr hi = (va + len - base) > g_vram.size ? g_vram.size : va + len - base;
    run_callbacks(lo, hi - lo, write);
}

/* ---------------------------------------------------------------- misc hooks */
void nv2a_trace_unhandled_method(uint32_t subchannel, uint32_t graphics_class, uint32_t method, uint32_t parameter)
{
    static int n;
    if (n++ < 200) {
        nv2a_log("nv2a: unhandled method subch %u class %x method %x param %x", subchannel, graphics_class, method, parameter);
    }
}

extern void *pfifo_thread(void *arg);

/* Must run on the thread that owns the presentation window (creates shared GL contexts). */
void nv2a_host_context_init(void)
{
    nv2a_context_init();
}

void nv2a_host_init(uint8_t *ram, uint32_t ram_size)
{
    InitializeCriticalSection(&g_bql);
    g_irq_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    NV2AState *d = g_malloc0(sizeof(NV2AState));
    g_dev = d;

    g_vram.ptr = ram;
    g_vram.size = ram_size;
    d->vram = &g_vram;
    d->vram_ptr = ram;

    /* MMIO window; RAMIN is ordinary memory inside it */
    mmio_register_c(NV2A_MMIO_BASE, NV2A_MMIO_SIZE, nv2a_mmio_read, nv2a_mmio_write, d, "NV2A");
    d->ramin_ptr = VirtualAlloc((void *)(uintptr_t)(NV2A_MMIO_BASE + NV2A_RAMIN_OFFSET), NV2A_RAMIN_SIZE,
                                MEM_COMMIT, PAGE_READWRITE);
    if (!d->ramin_ptr) {
        nv2a_log("nv2a: cannot commit RAMIN (%lu)", GetLastError());
        abort();
    }
    d->ramin.ptr = d->ramin_ptr;
    d->ramin.size = NV2A_RAMIN_SIZE;

    memory_region_set_dirty(d->vram, 0, ram_size);

    d->vga.get_resolution = nv2a_get_resolution;
    d->vga.get_params = nv2a_get_params;
    d->vga.msr = VGA_MIS_COLOR;
    /* progressive scan unless the mode is 1080i (the kernel's mode set would program this) */
    d->vga.cr[NV_PRMCIO_INTERLACE_MODE] = NV_PRMCIO_INTERLACE_MODE_DISABLED;

    qemu_mutex_init(&d->pfifo.lock);
    qemu_cond_init(&d->pfifo.fifo_cond);
    qemu_cond_init(&d->pfifo.fifo_idle_cond);

    pgraph_init(d);

    /* reset state (from xemu nv2a_reset) */
    d->pcrtc.start = 0;
    d->pramdac.core_clock_coeff = 0x00011C01;
    d->pramdac.core_clock_freq = 233333324;
    d->pramdac.memory_clock_coeff = 0;
    d->pramdac.video_clock_coeff = 0x0003C20D;
    d->pfifo.regs[NV_PFIFO_CACHE1_STATUS] |= NV_PFIFO_CACHE1_STATUS_LOW_MARK;
    for (int i = 0; i < 256; i++) {
        d->puserdac.palette[i * 3] = i;
        d->puserdac.palette[i * 3 + 1] = i;
        d->puserdac.palette[i * 3 + 2] = i;
    }

    ptimer_init(d);
    ptimer_reset(d);

    qemu_thread_create(&d->pfifo.thread, "nv2a.pfifo_thread", pfifo_thread, d, QEMU_THREAD_JOINABLE);
    CloseHandle(CreateThread(NULL, 0, irq_thread, d, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, vblank_thread, d, 0, NULL));
    nv2a_log("nv2a: initialized (vram %p, ramin %p)", ram, d->ramin_ptr);
}

void nv2a_config_set_surface_scale(int scale) { g_config.display.quality.surface_scale = scale < 1 ? 1 : scale; }

void nv2a_set_refresh_rate(int hz) { g_refresh_hz = hz > 0 ? hz : 60; }

/* Horizontal squash for screen-space (w == 1) draws on the 640x480 display surface, so 2D UI
 * keeps 4:3 proportions when the game renders anamorphic 16:9. 1.0 = off. */
float g_nv2a_ui_squash = 1.0f;
void nv2a_set_ui_squash(float s) { g_nv2a_ui_squash = s > 0.f ? s : 1.0f; }

/* debug: one-line NV2A state for the stall watchdog */
void nv2a_debug_state(char *buf, int n)
{
    NV2AState *d = g_dev;
    if (!d) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, n,
             "get %08x put %08x flipwait %d nopwait %d ctxwait %d fifo_access %d | pmc pend %08x en %08x | pgraph pend %08x en %08x | "
             "pfifo pend %08x | pcrtc pend %08x en %08x | irq_asserted %d | surface %08x",
             d->pfifo.regs[NV_PFIFO_CACHE1_DMA_GET], d->pfifo.regs[NV_PFIFO_CACHE1_DMA_PUT], d->pgraph.waiting_for_flip,
             d->pgraph.waiting_for_nop, d->pgraph.waiting_for_context_switch,
             (d->pgraph.regs_[NV_PGRAPH_FIFO] & NV_PGRAPH_FIFO_ACCESS) != 0, d->pmc.pending_interrupts, d->pmc.enabled_interrupts,
             d->pgraph.pending_interrupts, d->pgraph.enabled_interrupts, d->pfifo.pending_interrupts, d->pcrtc.pending_interrupts,
             d->pcrtc.enabled_interrupts, (int)g_irq_asserted, d->pgraph.regs_[NV_PGRAPH_SURFACE]);
}
