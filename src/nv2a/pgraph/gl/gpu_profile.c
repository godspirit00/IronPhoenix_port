/*
 * XB_GPU_PROFILE=1: GPU time per operation category on the render context (port diagnostic).
 *
 * Each timed operation gets a pair of GL_TIMESTAMP queries; results are collected without ever
 * waiting (only slots whose end query is available are read). Every flip also records a
 * timestamp so the log can compare the categorized GPU time with the GPU-side frame span
 * (which also contains idle time while the GPU waits for commands). Logged every ~2 s as
 * "gpu profile: ...". Timestamps measure command spans on the GPU timeline, so a span can
 * include waiting for earlier work queued before it; the categories are leaf operations
 * issued in order, so this is small in practice.
 */

#include "nv2a_int.h"
#include "renderer.h"
#include "gpu_profile.h"

#define GP_SLOTS 8192

static const char *const gp_names[GPC__COUNT] = {
    "draw", "surf_to_tex", "clear", "surf_upload", "surf_download", "tex_upload",
};

static struct {
    GLuint q[2];
    int8_t cat;    /* -1 free, else category; GPC__COUNT = flip marker (q[0] only) */
    uint32_t key;  /* draws: GL program */
    uint32_t verts;
} g_slots[GP_SLOTS];
/* per-program draw totals for the current window (open addressing) */
#define GP_KEYS 2048
static struct { uint32_t key; uint64_t ns, draws, verts; } g_keys[GP_KEYS];
static unsigned g_head, g_tail; /* ring: [tail, head) pending */
static int g_enabled = -1;
static uint64_t g_ns[GPC__COUNT], g_count[GPC__COUNT];
static uint64_t g_last_flip_ts, g_flip_span_ns, g_flips;
static int64_t g_report_us;
static unsigned g_dropped;

bool gpu_prof_enabled(void)
{
    if (g_enabled < 0) {
        g_enabled = getenv("XB_GPU_PROFILE") != NULL;
    }
    return g_enabled;
}

static void gp_init(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;
    for (int i = 0; i < GP_SLOTS; i++) {
        glGenQueries(2, g_slots[i].q);
        g_slots[i].cat = -1;
    }
    g_report_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

static void gp_collect(void)
{
    while (g_tail != g_head) {
        unsigned i = g_tail % GP_SLOTS;
        int cat = g_slots[i].cat;
        GLuint end_q = cat == GPC__COUNT ? g_slots[i].q[0] : g_slots[i].q[1];
        GLuint ready = 0;
        glGetQueryObjectuiv(end_q, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready) {
            break; /* in submission order: later ones are not ready either */
        }
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(g_slots[i].q[0], GL_QUERY_RESULT, &t0);
        if (cat == GPC__COUNT) {
            if (g_last_flip_ts && t0 > g_last_flip_ts) {
                g_flip_span_ns += t0 - g_last_flip_ts;
                g_flips++;
            }
            g_last_flip_ts = t0;
        } else {
            glGetQueryObjectui64v(g_slots[i].q[1], GL_QUERY_RESULT, &t1);
            if (t1 > t0) {
                g_ns[cat] += t1 - t0;
            }
            g_count[cat]++;
            if (cat == GPC_DRAW && g_slots[i].key) {
                unsigned h = (g_slots[i].key * 2654435761u) % GP_KEYS;
                for (int probe = 0; probe < GP_KEYS; probe++, h = (h + 1) % GP_KEYS) {
                    if (g_keys[h].key == g_slots[i].key || !g_keys[h].key) {
                        g_keys[h].key = g_slots[i].key;
                        g_keys[h].ns += t1 > t0 ? t1 - t0 : 0;
                        g_keys[h].draws++;
                        g_keys[h].verts += g_slots[i].verts;
                        break;
                    }
                }
            }
        }
        g_slots[i].cat = -1;
        g_tail++;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (now - g_report_us >= 2000000) {
        char buf[512];
        int n = 0;
        uint64_t total = 0;
        for (int c = 0; c < GPC__COUNT; c++) {
            total += g_ns[c];
            n += snprintf(buf + n, sizeof buf - n, " %s %.1fms/%llu", gp_names[c], g_ns[c] / 1e6,
                          (unsigned long long)g_count[c]);
        }
        double wall_ms = (now - g_report_us) / 1000.0;
        nv2a_log("gpu profile: %.0f ms wall | categorized %.1f ms (%.0f%%) |%s | gpu frame span avg %.2f ms over %llu flips%s",
                 wall_ms, total / 1e6, total / 1e4 / wall_ms, buf,
                 g_flips ? g_flip_span_ns / 1e6 / g_flips : 0.0, (unsigned long long)g_flips,
                 g_dropped ? " (ring full: samples dropped)" : "");
        /* top programs by GPU time */
        char top[512];
        int tn = 0;
        for (int k = 0; k < 6; k++) {
            int best = -1;
            for (int j = 0; j < GP_KEYS; j++) {
                if (g_keys[j].key && g_keys[j].draws && (best < 0 || g_keys[j].ns > g_keys[best].ns)) best = j;
            }
            if (best < 0) break;
            tn += snprintf(top + tn, sizeof top - tn, " prog %u%s%s%s: %.1fms/%llu draws/%llu verts;", g_keys[best].key >> 3,
                           g_keys[best].key & 1 ? " ztest" : " noztest", g_keys[best].key & 2 ? " zwrite" : "",
                           g_keys[best].key & 4 ? " stencil" : "",
                           g_keys[best].ns / 1e6, (unsigned long long)g_keys[best].draws,
                           (unsigned long long)g_keys[best].verts);
            if (k < 3) {
                /* dump the program's shader sources once (gpuprof_prog_<id>_<n>.glsl next to the exe) */
                static uint32_t dumped[64];
                static int ndumped;
                bool seen = false;
                for (int j = 0; j < ndumped; j++) seen |= dumped[j] == g_keys[best].key;
                if (!seen && ndumped < 64) {
                    dumped[ndumped++] = g_keys[best].key;
                    GLuint shaders[4];
                    GLsizei ns = 0;
                    glGetAttachedShaders(g_keys[best].key >> 3, 4, &ns, shaders);
                    for (int j = 0; j < ns; j++) {
                        GLint len = 0;
                        glGetShaderiv(shaders[j], GL_SHADER_SOURCE_LENGTH, &len);
                        if (len <= 0) continue;
                        char *src = g_malloc(len);
                        glGetShaderSource(shaders[j], len, NULL, src);
                        char name[64];
                        snprintf(name, sizeof name, "gpuprof_prog_%u_%d.glsl", g_keys[best].key >> 3, j);
                        FILE *f = fopen(name, "w");
                        if (f) { fputs(src, f); fclose(f); }
                        g_free(src);
                    }
                }
            }
            g_keys[best].draws = 0; /* exclude from the next pick */
        }
        if (tn) nv2a_log("gpu profile top:%s", top);
        {
            uint64_t cls[8] = {0};
            for (int j = 0; j < GP_KEYS; j++) if (g_keys[j].key) cls[g_keys[j].key & 7] += g_keys[j].ns;
            nv2a_log("gpu profile depth classes: noztest %.1fms, ztest+zwrite %.1fms, ztest only %.1fms, nozt+zwrite %.1fms (+stencil: %.1f/%.1f/%.1f/%.1f)",
                     cls[0] / 1e6, cls[3] / 1e6, cls[1] / 1e6, cls[2] / 1e6, cls[4] / 1e6, cls[7] / 1e6, cls[5] / 1e6, cls[6] / 1e6);
        }
        memset(g_keys, 0, sizeof g_keys);
        memset(g_ns, 0, sizeof g_ns);
        memset(g_count, 0, sizeof g_count);
        g_flip_span_ns = g_flips = 0;
        g_dropped = 0;
        g_report_us = now;
    }
}

static int gp_alloc(int cat)
{
    gp_init();
    if ((g_head & 255) == 0) {
        gp_collect();
    }
    if (g_head - g_tail >= GP_SLOTS) {
        gp_collect();
        if (g_head - g_tail >= GP_SLOTS) {
            g_dropped++;
            return -1;
        }
    }
    unsigned i = g_head % GP_SLOTS;
    g_slots[i].cat = (int8_t)cat;
    g_slots[i].key = 0;
    g_slots[i].verts = 0;
    return (int)i;
}

int gpu_prof_begin(int cat)
{
    if (!gpu_prof_enabled()) {
        return -1;
    }
    int i = gp_alloc(cat);
    if (i >= 0) {
        glQueryCounter(g_slots[i].q[0], GL_TIMESTAMP);
        g_head++;
    }
    return i;
}

void gpu_prof_set_key(int slot, unsigned int key, unsigned int verts)
{
    if (slot >= 0) {
        g_slots[slot].key = key;
        g_slots[slot].verts = verts;
    }
}

void gpu_prof_end(int slot)
{
    if (slot >= 0) {
        glQueryCounter(g_slots[slot].q[1], GL_TIMESTAMP);
    }
}

void gpu_prof_end_p(int *slot)
{
    gpu_prof_end(*slot);
}

void gpu_prof_flip(void)
{
    if (!gpu_prof_enabled()) {
        return;
    }
    int i = gp_alloc(GPC__COUNT);
    if (i >= 0) {
        glQueryCounter(g_slots[i].q[0], GL_TIMESTAMP);
        g_head++;
    }
    gp_collect();
}
