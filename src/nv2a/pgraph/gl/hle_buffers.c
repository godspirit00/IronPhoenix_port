/* Experimental GPU-owned vertex ranges. Identities are provenance, never
 * content versions: every physical page also participates in write watching. */
#include "renderer.h"
#include "qemu/fast-hash.h"
#include "vertex_coverage.h"
#include "hle_buffer_budget.h"

#define HLE_BUFFER_COUNT 512
#define HLE_BUFFER_BUDGET (64u * 1024 * 1024)
#define HLE_BUFFER_MAX (8u * 1024 * 1024)
typedef struct HleBufferEntry {
    uint64_t resource, draw;
    uint32_t frame;
    hwaddr base, size;
    GLuint buffer;
    uint8_t *shadow;
    NvVertexCoverage coverage;
} HleBufferEntry;
struct HleBuffers {
    bool verify;
    uint64_t draw, binds, hits, uploads, bytes, checks, evictions, fallbacks;
    uint64_t span_updates, span_bytes;
    uint64_t admission_fallback, compare_fallback, update_fallback;
    uint32_t frame;
    size_t frame_admit, frame_compare, frame_update;
    size_t peak_admit, peak_compare, peak_update;
    size_t resident;
    unsigned readbacks;
    int64_t report_us;
    HleBufferEntry entries[HLE_BUFFER_COUNT];
};

static void release_entry(PGRAPHGLState *r, HleBufferEntry *e)
{
    if (!e->buffer) return;
    for (unsigned i = 0; i < 16; ++i)
        if (r->vertex_attrib[i].buffer == e->buffer)
            r->vertex_attrib[i].pointer_valid = false;
    glDeleteBuffers(1, &e->buffer); pgraph_gl_buffer_bindings_invalidate();
    r->hle_buffers->resident -= e->size;
    ++r->hle_buffers->evictions;
    g_free(e->shadow);
    memset(e, 0, sizeof(*e));
}

void pgraph_gl_hle_buffers_init(NV2AState *d)
{
    PGRAPHGLState *r = d->pgraph.gl_renderer_state;
    const char *mode = getenv("XB_HLE_D3D8_BUFFERS");
    if (!mode || (!strcmp(mode, "off"))) return;
    const char *resources = getenv("XB_HLE_D3D8_RESOURCES");
    const char *indexed = getenv("XB_HLE_D3D8_INDEXED");
    if ((strcmp(mode, "verify") && strcmp(mode, "1")) ||
        !resources || (strcmp(resources, "verify") && strcmp(resources, "1")) ||
        !indexed || strcmp(indexed, "1") || r->no_fast_vertex) {
        nv2a_log("HLE buffers unavailable: requires native indexed packets, resource snapshots and fast vertex state");
        return;
    }
    r->hle_buffers = g_malloc0(sizeof(*r->hle_buffers));
    r->hle_buffers->verify = !strcmp(mode, "verify");
    nv2a_log("HLE buffers mode: %s", mode);
    nv2a_log("HLE buffers limits: admission 4 MiB/frame comparison 16 MiB/frame updates 4 MiB/frame; preserve current/previous-frame entries");
}

void pgraph_gl_hle_buffers_finalize(PGRAPHGLState *r)
{
    if (!r->hle_buffers) return;
    for (unsigned i = 0; i < HLE_BUFFER_COUNT; ++i) release_entry(r, &r->hle_buffers->entries[i]);
    g_free(r->hle_buffers);
    r->hle_buffers = NULL;
}

void pgraph_gl_hle_buffers_begin(PGRAPHGLState *r)
{
    struct HleBuffers *c = r->hle_buffers;
    if (!c) return;
    ++c->draw;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (now - c->report_us < 2000000) return;
    c->report_us = now;
    nv2a_log("HLE buffers: binds %llu hits %llu uploads %llu bytes %llu checks %llu readbacks %u evictions %llu fallback %llu resident %zu span_updates %llu span_bytes %llu admission_fallback %llu compare_fallback %llu update_fallback %llu peak_admit %zu peak_compare %zu peak_update %zu",
        (unsigned long long)c->binds, (unsigned long long)c->hits,
        (unsigned long long)c->uploads, (unsigned long long)c->bytes,
        (unsigned long long)c->checks, c->readbacks,
        (unsigned long long)c->evictions, (unsigned long long)c->fallbacks, c->resident,
        (unsigned long long)c->span_updates, (unsigned long long)c->span_bytes,
        (unsigned long long)c->admission_fallback, (unsigned long long)c->compare_fallback,
        (unsigned long long)c->update_fallback, c->peak_admit, c->peak_compare, c->peak_update);
}

static bool surface_overlap(PGRAPHGLState *r, hwaddr base, hwaddr end)
{
    SurfaceBinding *s;
    QTAILQ_FOREACH(s, &r->surfaces, entry)
        if (base < s->vram_addr + s->size && s->vram_addr < end) return true;
    QTAILQ_FOREACH(s, &r->parked_surfaces, entry)
        if (base < s->vram_addr + s->size && s->vram_addr < end) return true;
    return false;
}

bool pgraph_gl_hle_buffer(NV2AState *d, unsigned slot, unsigned min_element, unsigned max_element,
                         hwaddr address, GLuint *buffer, hwaddr *offset)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    struct HleBuffers *c = r->hle_buffers;
    if (!c || !pg->hle_indexed_draw_active) return false;
    uint32_t frame = (uint32_t)pg->frame_time;
    if (c->frame != frame) {
        c->frame = frame;
        c->frame_admit = c->frame_compare = c->frame_update = 0;
    }
    VertexAttribute *a = &pg->vertex_attributes[slot];
    uint64_t resource = pg->hle_vertex_resources[slot];
    hwaddr element_size = a->size * a->count;
    hwaddr end = address + (uint64_t)max_element * a->stride + element_size;
    hwaddr base = address & ~UINT64_C(4095);
    end = (end + 4095) & ~UINT64_C(4095);
    if (!resource || !a->stride || !element_size || element_size > a->stride ||
        (a->offset | (a->dma_select ? 0x80000000u : 0)) != pg->hle_vertex_offsets[slot] ||
        end <= base || end > memory_region_size(d->vram) || end - base > HLE_BUFFER_MAX) {
        ++c->fallbacks;
        return false;
    }
    hwaddr size = end - base;
    // Different max indices in the same backing range share a capacity bucket.
    size = hle_buffer_capacity(size);
    if (!size || size > HLE_BUFFER_MAX || size > memory_region_size(d->vram) - base ||
        surface_overlap(r, base, base + size)) { ++c->fallbacks; return false; }
    uint64_t key[3] = {resource, base, size};
    unsigned bucket = fast_hash((uint8_t *)key, sizeof(key)) % HLE_BUFFER_COUNT;
    HleBufferEntry *e = NULL, *victim = NULL;
    for (unsigned i = 0; i < 8; ++i) {
        HleBufferEntry *p = &c->entries[(bucket + i) % HLE_BUFFER_COUNT];
        if (p->buffer && p->resource == resource && p->base == base && p->size == size) { e = p; break; }
        if ((!p->buffer || !hle_buffer_recent(frame, p->frame)) &&
            (!victim || p->draw < victim->draw)) victim = p;
    }
    if (!e) {
        if (!victim) { ++c->fallbacks; return false; }
        if (size > HLE_BUFFER_ADMIT_LIMIT - c->frame_admit) {
            ++c->fallbacks; ++c->admission_fallback; return false;
        }
        release_entry(r, victim);
        while (c->resident + size > HLE_BUFFER_BUDGET) {
            HleBufferEntry *oldest = NULL;
            for (unsigned i = 0; i < HLE_BUFFER_COUNT; ++i) {
                HleBufferEntry *p = &c->entries[i];
                if (p->buffer && !hle_buffer_recent(frame, p->frame) &&
                    (!oldest || p->draw < oldest->draw)) oldest = p;
            }
            if (!oldest) { ++c->fallbacks; return false; }
            release_entry(r, oldest);
        }
        e = victim;
        e->resource = resource; e->base = base; e->size = size;
    }
    e->draw = c->draw; // Pin all buffers referenced by this VAO/draw.
    e->frame = frame;
    if (!e->buffer) {
        bool reserved = hle_buffer_reserve(&c->frame_admit, size, HLE_BUFFER_ADMIT_LIMIT);
        assert(reserved);
        c->peak_admit = MAX(c->peak_admit, c->frame_admit);
        const uint8_t *source = d->vram_ptr + base;
        if (!e->shadow) e->shadow = g_malloc(size);
        memcpy(e->shadow, source, size);
        source = e->shadow;
        e->coverage = (NvVertexCoverage){c->draw, 0, size};
        // Orphan storage: earlier submitted draws retain their original bytes.
        if (!e->buffer) {
            glGenBuffers(1, &e->buffer);
            c->resident += size;
        }
        pgraph_gl_bind_array_buffer(e->buffer);
        glBufferData(GL_ARRAY_BUFFER, size, source, GL_STATIC_DRAW);
        if (c->verify && c->readbacks < 32) {
            uint8_t *actual = g_malloc(size);
            glGetBufferSubData(GL_ARRAY_BUFFER, 0, size, actual);
            if (memcmp(actual, source, size)) {
                nv2a_log("HLE buffers MISMATCH: uploaded GPU storage");
                abort();
            }
            g_free(actual);
            ++c->readbacks;
            ++c->checks;
        }
        ++c->uploads; c->bytes += size;
    } else {
        ++c->hits;
        // A kick may contain later CPU writes to previously unreferenced bytes
        // in an already scanned page (NOOVERWRITE). Page versions alone cannot
        // authorize reuse. Compare only this draw's contiguous fetched span.
        hwaddr pos = address - base + (uint64_t)min_element * a->stride;
        hwaddr length = (uint64_t)(max_element - min_element) * a->stride + element_size;
        assert(pos + length <= size);
        NvVertexSpan spans[2];
        NvVertexCoverage coverage = e->coverage;
        unsigned count = nv_vertex_uncovered(&coverage, c->draw, pos, pos + length, spans);
        size_t compare = 0, update = 0;
        bool changed[2] = {false, false};
        for (unsigned i = 0; i < count; ++i) compare += spans[i].end - spans[i].begin;
        if (!hle_buffer_reserve(&c->frame_compare, compare, HLE_BUFFER_COMPARE_LIMIT)) {
            ++c->fallbacks; ++c->compare_fallback; return false;
        }
        c->peak_compare = MAX(c->peak_compare, c->frame_compare);
        for (unsigned i = 0; i < count; ++i) {
            pos = spans[i].begin; length = spans[i].end - pos;
            changed[i] = memcmp(e->shadow + pos, d->vram_ptr + base + pos, length) != 0;
            if (changed[i]) update += length;
        }
        if (!hle_buffer_reserve(&c->frame_update, update, HLE_BUFFER_UPDATE_LIMIT)) {
            ++c->fallbacks; ++c->update_fallback; return false;
        }
        c->peak_update = MAX(c->peak_update, c->frame_update);
        e->coverage = coverage;
        for (unsigned i = 0; i < count; ++i) {
            pos = spans[i].begin; length = spans[i].end - pos;
            if (changed[i]) {
                memcpy(e->shadow + pos, d->vram_ptr + base + pos, length);
                pgraph_gl_bind_array_buffer(e->buffer);
                // GL preserves ordered earlier draws when updating live storage.
                glBufferSubData(GL_ARRAY_BUFFER, pos, length, e->shadow + pos);
                ++c->uploads; c->bytes += length;
                ++c->span_updates; c->span_bytes += length;
            }
        }
    }
    if (c->verify && (c->binds % 4096 == 0)) {
        // Compare GPU storage against the immutable upload snapshot, never
        // against live guest RAM which the CPU can already be writing for a
        // later draw. Readback sampling deliberately stalls verification only.
        hwaddr pos = address - base + (uint64_t)min_element * a->stride;
        hwaddr length = (uint64_t)(max_element - min_element) * a->stride + element_size;
        uint8_t *actual = g_malloc(length);
        pgraph_gl_bind_array_buffer(e->buffer);
        glGetBufferSubData(GL_ARRAY_BUFFER, pos, length, actual);
        ++c->checks;
        if (memcmp(e->shadow + pos, actual, length)) {
            nv2a_log("HLE buffers MISMATCH: GPU vertex span slot %u", slot);
            abort();
        }
        g_free(actual);
    }
    ++c->binds;
    *buffer = e->buffer;
    *offset = address - base;
    return true;
}
