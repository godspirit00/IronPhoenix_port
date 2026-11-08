/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
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

#include "nv2a_int.h"
#include "renderer.h"

/* Occlusion queries without pipeline stalls (port change).
 * xemu reads every query result with a blocking GL_QUERY_RESULT, and the game spins in
 * D3DDevice_GetVisibilityTestResult until the report lands, so each test was a full CPU<->GPU
 * round trip. If a report slot has been answered before and its GL queries are not finished yet,
 * write the slot's previous value now (one frame stale) and collect the real result later. */
#define REPORT_CACHE_SIZE 1024
#define MAX_DEFERRED 256
static struct {
    bool valid;
    uint32_t offset;
    uint32_t value;
} g_report_cache[REPORT_CACHE_SIZE];
static struct {
    uint32_t offset;
    unsigned int query_count;
    GLuint *queries;
    bool write;          /* exact mode: report not written yet - write it when the result is in */
    uint32_t parameter;
    uint32_t dma_report;
} g_deferred[MAX_DEFERRED];
static int g_num_deferred;
static bool g_deferred_need_flush; /* one glFlush per pusher batch so deferred queries complete */
volatile long g_reports_deferred, g_reports_blocking; /* perf counters */

/* XB_OCCLUSION=exact (default: blocking GL read, xemu behaviour - stale answers made light
 * effects flicker), fast (previous result while the GPU is busy), late (exact value written when
 * the GPU has it; the game waits) */
static int g_occl_mode = -1; /* 0 fast, 1 exact, 2 late */
int nv2a_occlusion_mode(void)
{
    if (g_occl_mode < 0) {
        const char *m = getenv("XB_OCCLUSION");
        g_occl_mode = !m ? 1 : !strcmp(m, "fast") ? 0 : !strcmp(m, "late") ? 2 : 1;
    }
    return g_occl_mode;
}
static bool exact_occlusion(void)
{
    return nv2a_occlusion_mode() != 0;
}

static int report_cache_slot(uint32_t offset)
{
    return (offset >> 4) & (REPORT_CACHE_SIZE - 1);
}

static uint32_t sum_queries(PGRAPHState *pg, unsigned int count, GLuint *queries)
{
    uint32_t total = 0;
    for (int i = 0; i < count; i++) {
        GLuint gl_query_result = 0;
        glGetQueryObjectuiv(queries[i], GL_QUERY_RESULT, &gl_query_result);
        /* Port change: round up. At surface scale N each native pixel is N*N samples; a small
         * light/glow test quad covering only part of a pixel gave e.g. 5/9 -> 0 ("occluded"),
         * flipping between 0 and 1 as it moved and making effects flash on and off. */
        unsigned int ss = pg->surface_scale_factor * pg->surface_scale_factor;
        total += (gl_query_result + ss - 1) / ss;
    }
    if (count) {
        glDeleteQueries(count, queries);
        g_free(queries);
    }
    return total;
}

static bool queries_available(unsigned int count, GLuint *queries)
{
    for (int i = 0; i < count; i++) {
        GLuint avail = 0;
        glGetQueryObjectuiv(queries[i], GL_QUERY_RESULT_AVAILABLE, &avail);
        if (!avail) {
            return false;
        }
    }
    return true;
}

bool pgraph_gl_reports_pending(void)
{
    return g_num_deferred > 0;
}

static void collect_deferred(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    int n = 0;
    bool blocked = false; /* exact mode: write reports in submission order */
    for (int i = 0; i < g_num_deferred; i++) {
        if (!blocked && queries_available(g_deferred[i].query_count, g_deferred[i].queries)) {
            uint32_t value = sum_queries(pg, g_deferred[i].query_count, g_deferred[i].queries);
            int slot = report_cache_slot(g_deferred[i].offset);
            g_report_cache[slot].valid = true;
            g_report_cache[slot].offset = g_deferred[i].offset;
            g_report_cache[slot].value = value;
            if (g_deferred[i].write) {
                uint32_t saved = pg->dma_report;
                pg->dma_report = g_deferred[i].dma_report;
                pgraph_write_zpass_pixel_cnt_report(d, g_deferred[i].parameter, value);
                pg->dma_report = saved;
            }
        } else {
            if (g_deferred[i].write) {
                blocked = true;
            }
            g_deferred[n++] = g_deferred[i];
        }
    }
    g_num_deferred = n;
}

static void process_pending_report(NV2AState *d, QueryReport *report)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (report->clear) {
        r->zpass_pixel_count_result = 0;
        return;
    }

    uint8_t type = GET_MASK(report->parameter, NV097_GET_REPORT_TYPE);
    assert(type == NV097_GET_REPORT_TYPE_ZPASS_PIXEL_CNT);

    uint32_t offset = GET_MASK(report->parameter, NV097_GET_REPORT_OFFSET);
    int slot = report_cache_slot(offset);
    if (!exact_occlusion() && report->query_count && g_report_cache[slot].valid && g_report_cache[slot].offset == offset &&
        g_num_deferred < MAX_DEFERRED && !queries_available(report->query_count, report->queries)) {
        g_deferred[g_num_deferred].offset = offset;
        g_deferred[g_num_deferred].query_count = report->query_count;
        g_deferred[g_num_deferred].queries = report->queries;
        g_deferred[g_num_deferred].write = false;
        g_num_deferred++;
        g_reports_deferred++;
        g_deferred_need_flush = true;
        pgraph_write_zpass_pixel_cnt_report(d, report->parameter, g_report_cache[slot].value);
        return;
    }

    /* Exact mode without stalling the renderer: if the GL results are not in yet, write the
     * report later (collect_deferred, polled every pfifo loop) instead of a blocking
     * GL_QUERY_RESULT read, which drained the GPU pipeline thousands of times per second. */
    if (nv2a_occlusion_mode() == 2 && report->query_count && g_num_deferred < MAX_DEFERRED &&
        (g_num_deferred > 0 || !queries_available(report->query_count, report->queries))) {
        g_deferred[g_num_deferred].offset = offset;
        g_deferred[g_num_deferred].query_count = report->query_count;
        g_deferred[g_num_deferred].queries = report->queries;
        g_deferred[g_num_deferred].write = true;
        g_deferred[g_num_deferred].parameter = report->parameter;
        g_deferred[g_num_deferred].dma_report = pg->dma_report;
        g_num_deferred++;
        g_reports_deferred++;
        g_deferred_need_flush = true;
        return;
    }

    /* FIXME: Multisampling affects this (both: OGL and Xbox GPU),
     *        not sure if CLEARs also count
     */
    /* FIXME: What about clipping regions etc? */
    if (report->query_count) {
        g_reports_blocking++;
    }
    r->zpass_pixel_count_result += sum_queries(pg, report->query_count, report->queries);
    g_report_cache[slot].valid = true;
    g_report_cache[slot].offset = offset;
    g_report_cache[slot].value = r->zpass_pixel_count_result;

    pgraph_write_zpass_pixel_cnt_report(d, report->parameter, r->zpass_pixel_count_result);
}

void pgraph_gl_process_pending_reports(NV2AState *d)
{
    PROF_SCOPE(PS_REPORTS);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;
    QueryReport *report, *next;

    if (g_deferred_need_flush) {
        glFlush();
        g_deferred_need_flush = false;
    }
    collect_deferred(d);
    QSIMPLEQ_FOREACH_SAFE(report, &r->report_queue, entry, next) {
        process_pending_report(d, report);
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }
}

void pgraph_gl_clear_report_value(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* FIXME: Does this have a value in parameter? Also does this (also?) modify
     *        the report memory block?
     */
    if (r->gl_zpass_pixel_count_query_count) {
        glDeleteQueries(r->gl_zpass_pixel_count_query_count,
                        r->gl_zpass_pixel_count_queries);
        r->gl_zpass_pixel_count_query_count = 0;
    }

    QueryReport *report = g_malloc(sizeof(QueryReport));
    report->clear = true;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);
}

void pgraph_gl_init_reports(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    QSIMPLEQ_INIT(&r->report_queue);
}

void pgraph_gl_get_report(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    /* Slot answered before: write the previous value right away (the game is typically
     * spinning on it) and collect the real result once the GPU has it. */
    uint32_t offset = GET_MASK(parameter, NV097_GET_REPORT_OFFSET);
    int slot = report_cache_slot(offset);
    if (!exact_occlusion() && r->gl_zpass_pixel_count_query_count && g_report_cache[slot].valid && g_report_cache[slot].offset == offset &&
        g_num_deferred < MAX_DEFERRED) {
        g_deferred[g_num_deferred].offset = offset;
        g_deferred[g_num_deferred].query_count = r->gl_zpass_pixel_count_query_count;
        g_deferred[g_num_deferred].queries = r->gl_zpass_pixel_count_queries;
        g_deferred[g_num_deferred].write = false;
        g_num_deferred++;
        g_reports_deferred++;
        r->gl_zpass_pixel_count_query_count = 0;
        r->gl_zpass_pixel_count_queries = NULL;
        g_deferred_need_flush = true;
        pgraph_write_zpass_pixel_cnt_report(d, parameter, g_report_cache[slot].value);
        return;
    }

    QueryReport *report = g_malloc(sizeof(QueryReport));
    report->clear = false;
    report->parameter = parameter;
    report->query_count = r->gl_zpass_pixel_count_query_count;
    report->queries = r->gl_zpass_pixel_count_queries;
    QSIMPLEQ_INSERT_TAIL(&r->report_queue, report, entry);

    r->gl_zpass_pixel_count_query_count = 0;
    r->gl_zpass_pixel_count_queries = NULL;
}

void pgraph_gl_finalize_reports(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    QueryReport *report, *next;
    QSIMPLEQ_FOREACH_SAFE (report, &r->report_queue, entry, next) {
        if (report->query_count) {
            glDeleteQueries(report->query_count, report->queries);
        }
        QSIMPLEQ_REMOVE_HEAD(&r->report_queue, entry);
        g_free(report);
    }

    if (r->gl_zpass_pixel_count_query_count) {
        glDeleteQueries(r->gl_zpass_pixel_count_query_count,
                        r->gl_zpass_pixel_count_queries);
        r->gl_zpass_pixel_count_query_count = 0;
    }
}
