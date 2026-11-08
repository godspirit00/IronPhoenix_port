/*
 * Minimal QEMU / glib compatibility layer so xemu's NV2A emulation can be built
 * standalone inside the Iron Phoenix runtime (Win32, i686).
 */
#ifndef NV2A_COMPAT_H
#define NV2A_COMPAT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <inttypes.h>
#include <limits.h>
#include <float.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ basics */
typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define TARGET_PAGE_SIZE 4096
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_MASK (~(TARGET_PAGE_SIZE - 1))
#define TARGET_PAGE_ALIGN(addr) (((addr) + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK)
#define tostring(s) #s
#define stringify(s) tostring(s)

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define ROUND_UP(n, d) (((n) + (d) - 1) & -(0 ? (n) : (d)))
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define QEMU_ALIGN_UP(n, m) ((((n) + (m) - 1) / (m)) * (m))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define QEMU_BUILD_BUG_MSG(x, msg) _Static_assert(!(x), msg)
#define QEMU_BUILD_BUG_ON(x) _Static_assert(!(x), "not expecting: " #x)
#define QEMU_PACKED __attribute__((packed))
#define QEMU_NORETURN __attribute__((noreturn))
#define G_GNUC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#define G_GNUC_UNUSED __attribute__((unused))
#define KiB (1024ULL)
#define MiB (1024ULL * 1024)
#define GiB (1024ULL * 1024 * 1024)

static inline int ctz32(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
static inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline int ctz64(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
static inline int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline int ctpop32(uint32_t v) { return __builtin_popcount(v); }
static inline uint32_t extract32(uint32_t value, int start, int length) { return (value >> start) & (~0U >> (32 - length)); }

static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint16_t lduw_le_p(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint64_t ldq_le_p(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void stl_le_p(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void stw_le_p(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void stq_le_p(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* ------------------------------------------------------------------ bitmaps */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BITS_TO_LONGS(n) (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
static inline void bitmap_set(unsigned long *map, long start, long nr) {
    for (long i = start; i < start + nr; i++) map[i / BITS_PER_LONG] |= 1UL << (i % BITS_PER_LONG);
}
static inline void bitmap_clear(unsigned long *map, long start, long nr) {
    for (long i = start; i < start + nr; i++) map[i / BITS_PER_LONG] &= ~(1UL << (i % BITS_PER_LONG));
}
static inline int test_bit(long nr, const unsigned long *addr) { return 1UL & (addr[nr / BITS_PER_LONG] >> (nr & (BITS_PER_LONG - 1))); }

/* glib autoptr: not needed (mstring declares one) */
#define G_DEFINE_AUTOPTR_CLEANUP_FUNC(T, fn)
#define g_autoptr(T) T *

/* CPU memory access callbacks only exist under TCG; tcg_enabled() is false here */
struct MemoryRegion;
typedef struct MemAccessCallback MemAccessCallback;
typedef void MemAccessCallbackFunc(void *opaque, struct MemoryRegion *mr, hwaddr addr, hwaddr len, bool write);
MemAccessCallback *mem_access_callback_insert(void *cpu, struct MemoryRegion *mr, hwaddr addr, hwaddr len,
                                              MemAccessCallbackFunc *cb, void *opaque);
void mem_access_callback_remove_by_ref(void *cpu, MemAccessCallback *cb);
void nv2a_arm_access_callbacks(void);

/* Renderer hot-switching (run on vCPU) is not supported in the standalone build */
typedef struct CPUState CPUState;
typedef union { void *host_ptr; } run_on_cpu_data;
#define RUN_ON_CPU_HOST_PTR(p) ((run_on_cpu_data){.host_ptr = (p)})
static inline void async_safe_run_on_cpu(void *cpu, void (*fn)(CPUState *, run_on_cpu_data), run_on_cpu_data d) {}

/* RCU is not used by the standalone build */
#define rcu_register_thread() ((void)0)
#define rcu_unregister_thread() ((void)0)

/* ------------------------------------------------------------------ atomics */
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_RELAXED)
#define qatomic_set(p, v) __atomic_store_n(p, v, __ATOMIC_RELAXED)
#define qatomic_inc(p) __atomic_fetch_add(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_dec(p) __atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_or(p, v) __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST)
#define qatomic_and(p, v) __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST)
#define qatomic_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_cmpxchg(p, o, n) ({ __typeof__(*(p)) _o = (o); __atomic_compare_exchange_n(p, &_o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); _o; })
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

/* ------------------------------------------------------------------ glib subset */
typedef char gchar;
typedef int gint;
typedef unsigned int guint;
typedef bool gboolean;
typedef void *gpointer;
typedef size_t gsize;
#define TRUE 1
#define FALSE 0

static inline void *g_malloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) abort(); return p; }
static inline void *g_malloc0(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) abort(); return p; }
static inline void *g_malloc_n(size_t a, size_t b) { return g_malloc(a * b); }
static inline void *g_malloc0_n(size_t a, size_t b) { return g_malloc0(a * b); }
static inline void *g_realloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) abort(); return p; }
static inline void g_free(void *p) { free(p); }
static inline char *g_strdup(const char *s) { return s ? strdup(s) : NULL; }
#define g_new(T, n) ((T *)g_malloc(sizeof(T) * (n)))
#define g_new0(T, n) ((T *)g_malloc0(sizeof(T) * (n)))
#define g_renew(T, p, n) ((T *)g_realloc(p, sizeof(T) * (n)))
static inline char *g_strdup_vprintf(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    char *s = (char *)g_malloc(n + 1);
    vsnprintf(s, n + 1, fmt, ap);
    return s;
}
static inline char *G_GNUC_PRINTF(1, 2) g_strdup_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *s = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return s;
}
static inline void g_autofree_cleanup(void *p) { free(*(void **)p); }
#define g_autofree __attribute__((cleanup(g_autofree_cleanup)))
#define g_assert(x) assert(x)
#define g_assert_not_reached() do { assert(!"not reached"); __builtin_unreachable(); } while (0)

typedef struct GString {
    char *str;
    size_t len;
    size_t allocated_len;
} GString;
static inline void g_string_maybe_expand(GString *s, size_t add) {
    if (s->len + add + 1 > s->allocated_len) {
        size_t n = s->allocated_len ? s->allocated_len : 64;
        while (n < s->len + add + 1) n *= 2;
        s->str = (char *)g_realloc(s->str, n);
        s->allocated_len = n;
    }
}
static inline GString *g_string_new(const char *init) {
    GString *s = (GString *)g_malloc0(sizeof(GString));
    g_string_maybe_expand(s, init ? strlen(init) : 0);
    s->str[0] = 0;
    if (init) { strcpy(s->str, init); s->len = strlen(init); }
    return s;
}
static inline GString *g_string_append_len(GString *s, const char *v, size_t n) {
    g_string_maybe_expand(s, n);
    memcpy(s->str + s->len, v, n);
    s->len += n;
    s->str[s->len] = 0;
    return s;
}
static inline GString *g_string_append(GString *s, const char *v) { return g_string_append_len(s, v, strlen(v)); }
static inline void g_string_append_vprintf(GString *s, const char *fmt, va_list ap) {
    char *t = g_strdup_vprintf(fmt, ap);
    g_string_append(s, t);
    free(t);
}
static inline void G_GNUC_PRINTF(2, 3) g_string_append_printf(GString *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_string_append_vprintf(s, fmt, ap);
    va_end(ap);
}
static inline char *g_string_free(GString *s, bool free_segment) {
    char *r = free_segment ? (free(s->str), (char *)NULL) : s->str;
    free(s);
    return r;
}

/* ------------------------------------------------------------------ threads (Win32) */
typedef struct QemuMutex { void *srw; } QemuMutex;          /* SRWLOCK */
typedef struct QemuCond { void *cv; } QemuCond;             /* CONDITION_VARIABLE */
typedef struct QemuEvent { void *handle; } QemuEvent;       /* manual-reset event */
typedef struct QemuThread { void *handle; } QemuThread;
typedef struct QemuSpin { volatile int value; } QemuSpin;
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1

void qemu_mutex_init(QemuMutex *m);
void qemu_mutex_destroy(QemuMutex *m);
void qemu_mutex_lock(QemuMutex *m);
void qemu_mutex_unlock(QemuMutex *m);
bool qemu_mutex_trylock(QemuMutex *m);
void qemu_cond_init(QemuCond *c);
void qemu_cond_destroy(QemuCond *c);
void qemu_cond_signal(QemuCond *c);
void qemu_cond_broadcast(QemuCond *c);
void qemu_cond_wait(QemuCond *c, QemuMutex *m);
bool qemu_cond_timedwait(QemuCond *c, QemuMutex *m, int ms);
void qemu_event_init(QemuEvent *e, bool init);
void qemu_event_destroy(QemuEvent *e);
void qemu_event_set(QemuEvent *e);
void qemu_event_reset(QemuEvent *e);
void qemu_event_wait(QemuEvent *e);
void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode);
void *qemu_thread_join(QemuThread *t);
static inline void qemu_spin_init(QemuSpin *s) { s->value = 0; }
static inline void qemu_spin_lock(QemuSpin *s) { while (__atomic_exchange_n(&s->value, 1, __ATOMIC_ACQUIRE)) {} }
static inline void qemu_spin_unlock(QemuSpin *s) { __atomic_store_n(&s->value, 0, __ATOMIC_RELEASE); }

/* Big QEMU lock: recursive lock serializing MMIO, vblank and IRQ state (nv2a_host.c) */
void nv2a_bql_lock(void);
void nv2a_bql_unlock(void);
static inline void bql_lock(void) { nv2a_bql_lock(); }
static inline void bql_unlock(void) { nv2a_bql_unlock(); }
static inline bool bql_locked(void) { return true; }

/* ------------------------------------------------------------------ time */
typedef enum { QEMU_CLOCK_REALTIME = 0, QEMU_CLOCK_VIRTUAL = 1, QEMU_CLOCK_HOST = 2 } QEMUClockType;
int64_t qemu_clock_get_ns(QEMUClockType type);
static inline int64_t qemu_clock_get_us(QEMUClockType type) { return qemu_clock_get_ns(type) / 1000; }
static inline int64_t qemu_clock_get_ms(QEMUClockType type) { return qemu_clock_get_ns(type) / 1000000; }
static inline int64_t get_clock(void) { return qemu_clock_get_ns(QEMU_CLOCK_REALTIME); }
#define NANOSECONDS_PER_SECOND 1000000000LL
static inline uint64_t muldiv64(uint64_t a, uint32_t b, uint32_t c) {
    return (a / c) * b + ((a % c) * b) / c;
}
static inline uint64_t muldiv64_round_up(uint64_t a, uint32_t b, uint32_t c) {
    /* a*b/c rounded up, computed as q*b + ceil(r*b/c) with r < c so r*b fits in 64 bits */
    return (a / c) * b + ((a % c) * b + c - 1) / c;
}

/* QEMU timers (only used by PTIMER alarms) -> serviced by our nv2a timer thread */
typedef void QEMUTimerCB(void *opaque);
typedef struct QEMUTimer {
    int64_t expire_time; /* ns, -1 = not pending */
    QEMUTimerCB *cb;
    void *opaque;
    int scale;
} QEMUTimer;
#define SCALE_NS 1
#define SCALE_US 1000
#define SCALE_MS 1000000
void timer_init_full(QEMUTimer *t, QEMUClockType type, int scale, QEMUTimerCB *cb, void *opaque);
#define timer_init_ns(t, type, cb, opaque) timer_init_full(t, type, SCALE_NS, cb, opaque)
void timer_mod(QEMUTimer *t, int64_t expire);
void timer_del(QEMUTimer *t);
bool timer_pending(QEMUTimer *t);

/* ------------------------------------------------------------------ errors / misc */
typedef struct Error { char *msg; } Error;
extern Error *error_fatal;
#define error_setg(errp, ...) do { if (errp) { *(errp) = (Error *)g_malloc0(sizeof(Error)); (*(errp))->msg = g_strdup_printf(__VA_ARGS__); } } while (0)
static inline const char *error_get_pretty(Error *e) { return e ? e->msg : ""; }
static inline void error_free(Error *e) { if (e) { free(e->msg); free(e); } }
void error_report(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
#define warn_report error_report
static inline bool tcg_enabled(void) { return true; } /* CPU accesses to surfaces are trapped via page protection (nv2a_host.c) */
static inline void *qemu_get_cpu(int i) { return NULL; }
static inline FILE *qemu_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int qemu_mkdir(const char *path);
int qemu_unlink(const char *path);

/* ------------------------------------------------------------------ memory regions */
/* Guest RAM is directly addressable; MemoryRegion only carries size + host pointer. */
typedef struct MemoryRegion {
    uint8_t *ptr;
    uint64_t size;
} MemoryRegion;
static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline uint8_t *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ptr; }
enum { DIRTY_MEMORY_VGA = 0, DIRTY_MEMORY_CODE = 1, DIRTY_MEMORY_MIGRATION = 2, DIRTY_MEMORY_NV2A = 3, DIRTY_MEMORY_NV2A_TEX = 4, DIRTY_MEMORY_NUM = 5 };
/* Implemented with Win32 write watching (nv2a_host.c) */
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
void memory_region_set_client_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);
void memory_region_set_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size);
bool memory_region_get_dirty(MemoryRegion *mr, hwaddr addr, hwaddr size, unsigned client);

typedef struct MemoryRegionOps {
    uint64_t (*read)(void *opaque, hwaddr addr, unsigned size);
    void (*write)(void *opaque, hwaddr addr, uint64_t data, unsigned size);
} MemoryRegionOps;

/* Physical memory accesses (Xbox RAM is mapped at 0x80000000) */
typedef struct AddressSpace { int unused; } AddressSpace;
extern AddressSpace address_space_memory;
#define XBOX_PHYS(a) ((uint8_t *)(uintptr_t)(0x80000000u + ((uint32_t)(a) & 0x0FFFFFFFu)))
static inline uint32_t ldl_le_phys(AddressSpace *as, hwaddr a) { return ldl_le_p(XBOX_PHYS(a)); }
static inline uint32_t lduw_le_phys(AddressSpace *as, hwaddr a) { return lduw_le_p(XBOX_PHYS(a)); }
static inline uint32_t ldub_phys(AddressSpace *as, hwaddr a) { return *XBOX_PHYS(a); }
static inline void stl_le_phys(AddressSpace *as, hwaddr a, uint32_t v) { stl_le_p(XBOX_PHYS(a), v); }
static inline void stw_le_phys(AddressSpace *as, hwaddr a, uint32_t v) { stw_le_p(XBOX_PHYS(a), (uint16_t)v); }
static inline void stb_phys(AddressSpace *as, hwaddr a, uint32_t v) { *XBOX_PHYS(a) = (uint8_t)v; }
#define warn_reportf_err(err, fmt, ...) do { nv2a_log(fmt "%s", ##__VA_ARGS__, error_get_pretty(err)); error_free(err); } while (0)
static inline void *qemu_add_vm_change_state_handler(void *cb, void *opaque) { return NULL; }

/* ------------------------------------------------------------------ xemu settings / ui */
typedef enum CONFIG_DISPLAY_RENDERER {
    CONFIG_DISPLAY_RENDERER_NULL = 0,
    CONFIG_DISPLAY_RENDERER_OPENGL = 1,
    CONFIG_DISPLAY_RENDERER_VULKAN = 2,
    CONFIG_DISPLAY_RENDERER__COUNT = 3,
} CONFIG_DISPLAY_RENDERER;
#define CONFIG_OPENGL 1
struct nv2a_config {
    struct {
        CONFIG_DISPLAY_RENDERER renderer;
        struct { int surface_scale; } quality;
    } display;
    struct { bool cache_shaders; } perf;
    struct {
        bool use_dsp;
        bool use_dsp_jit;
        bool hrtf;
        float volume_limit;
        struct { int num_workers; } vp;
    } audio;
};
extern struct nv2a_config g_config;
static inline const char *xemu_settings_get_base_path(void) { return "./"; }
static inline void xemu_queue_notification(const char *msg) { fprintf(stderr, "nv2a: %s\n", msg); }
static inline void xemu_queue_error_message(const char *msg) { fprintf(stderr, "nv2a error: %s\n", msg); }
extern const char xemu_version[];
extern const size_t xemu_version_len;

/* ------------------------------------------------------------------ tracing: compiled out */
#define trace_event_get_state_backends(x) 0
#define trace_enable_events(x) ((void)0)
#define trace_nv2a_pgraph_flip_increment_write(...) ((void)0)
#define trace_nv2a_pgraph_flip_stall(...) ((void)0)
#define trace_nv2a_pgraph_method(...) ((void)0)
#define trace_nv2a_pgraph_method_abbrev(...) ((void)0)
#define trace_nv2a_pgraph_method_unhandled(...) nv2a_trace_unhandled_method(__VA_ARGS__)
#define trace_nv2a_pgraph_surface_compare_mismatch(...) ((void)0)
#define trace_nv2a_pgraph_surface_cpu_read(...) ((void)0)
#define trace_nv2a_pgraph_surface_cpu_write(...) ((void)0)
#define trace_nv2a_pgraph_surface_create_color(...) ((void)0)
#define trace_nv2a_pgraph_surface_create_zeta(...) ((void)0)
#define trace_nv2a_pgraph_surface_download(...) ((void)0)
#define trace_nv2a_pgraph_surface_evict_overlapping(...) ((void)0)
#define trace_nv2a_pgraph_surface_evict_reason(...) ((void)0)
#define trace_nv2a_pgraph_surface_hit_color(...) ((void)0)
#define trace_nv2a_pgraph_surface_hit_zeta(...) ((void)0)
#define trace_nv2a_pgraph_surface_invalidated(...) ((void)0)
#define trace_nv2a_pgraph_surface_match_color(...) ((void)0)
#define trace_nv2a_pgraph_surface_match_zeta(...) ((void)0)
#define trace_nv2a_pgraph_surface_migrate_type(...) ((void)0)
#define trace_nv2a_pgraph_surface_render_to_texture(...) ((void)0)
#define trace_nv2a_pgraph_surface_target(...) ((void)0)
#define trace_nv2a_pgraph_surface_texture_compat_failed(...) ((void)0)
#define trace_nv2a_pgraph_surface_upload(...) ((void)0)
#define trace_nv2a_reg_read(...) ((void)0)
#define trace_nv2a_reg_write(...) ((void)0)
#define trace_nv2a_irq(...) ((void)0)
#define trace_nv2a_dma_map(...) ((void)0)
#define trace_mcpx_apu_reg_write(...) ((void)0)
#define trace_mcpx_apu_reg_read(...) ((void)0)
#define trace_mcpx_apu_method(...) ((void)0)
#define trace_event_get_state(x) 0
#define trace_dsp_write_peripheral(...) ((void)0)
#define trace_dsp_read_peripheral(...) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(...) ((void)0)
#define trace_dsp56k_execute_instruction(...) ((void)0)
void nv2a_trace_unhandled_method(uint32_t subchannel, uint32_t graphics_class, uint32_t method, uint32_t parameter);

extern volatile bool g_nv2a_renderer_ready;

/* Logging into the runtime log */
void nv2a_log(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

#ifdef __cplusplus
}
#endif
#endif

/* Port: cheap scoped timers for the GPU thread (summed, logged with the perf line) */
#ifndef NV2A_PROF_SCOPE_DEFINED
#define NV2A_PROF_SCOPE_DEFINED
enum { PS_DRAW_BEGIN, PS_DRAW_END, PS_FLUSH_DRAW, PS_CLEAR, PS_SURFACE_UPDATE, PS_BLIT, PS_TEXTURES, PS_SHADERS,
       PS_VERTEX_ATTRS, PS_PENDING, PS_REPORTS, PS_SYNC, PS_PUSHER, PS_SURF_MEMCHECK, PS_SURF_PART, PS_SURF_DL, PS_SURF_UL, PS_SURF_INVAL, PS__COUNT };
extern int64_t g_ps_ticks[PS__COUNT];
extern int g_ps_calls[PS__COUNT];
int64_t ps_now(void);
typedef struct { int id; int64_t t0; } ProfScope;
static inline void ps_end(ProfScope *s) { g_ps_ticks[s->id] += ps_now() - s->t0; g_ps_calls[s->id]++; }
#define PROF_SCOPE(id) ProfScope __ps_##id __attribute__((cleanup(ps_end))) = {id, ps_now()}
#endif

/* Port: GL error/FBO-completeness asserts force driver synchronization; only with XB_GL_DEBUG=1 */
#ifndef NV2A_GL_DEBUG_DEFINED
#define NV2A_GL_DEBUG_DEFINED
#include <stdlib.h>
static inline int nv2a_gl_debug(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("XB_GL_DEBUG") != NULL;
    }
    return v;
}
#endif
