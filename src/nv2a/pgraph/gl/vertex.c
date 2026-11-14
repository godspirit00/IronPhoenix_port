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

#include "nv2a_regs.h"
#include "nv2a_int.h"
#include "debug.h"
#include "renderer.h"

/* These helpers track the guest-draw VAO. Surface-copy and display passes use
 * their own VAOs; constant attribute values are only written through here. */
void pgraph_gl_attribute_enable(PGRAPHGLState *r, unsigned int index, bool enabled)
{
    if (r->no_fast_vertex) {
        if (enabled) glEnableVertexAttribArray(index);
        else glDisableVertexAttribArray(index);
        return;
    }
    typeof(r->vertex_attrib[0]) *a = &r->vertex_attrib[index];
    if (!a->enable_valid || a->enabled != enabled) {
        if (enabled) glEnableVertexAttribArray(index);
        else glDisableVertexAttribArray(index);
        a->enabled = enabled;
        a->enable_valid = true;
    }
    static unsigned int checks;
    if (nv2a_gl_debug() && (++checks % 257 == 0)) {
        GLint actual;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &actual);
        assert(!!actual == enabled);
    }
}

void pgraph_gl_attribute_value(PGRAPHGLState *r, unsigned int index, const GLfloat *value)
{
    if (r->no_fast_vertex) {
        glVertexAttrib4fv(index, value);
        return;
    }
    typeof(r->vertex_attrib[0]) *a = &r->vertex_attrib[index];
    if (!a->value_valid || memcmp(a->value, value, sizeof(a->value))) {
        glVertexAttrib4fv(index, value);
        memcpy(a->value, value, sizeof(a->value));
        a->value_valid = true;
    }
}

/* The caller has bound buffer to GL_ARRAY_BUFFER. A buffer's storage may be
 * updated/orphaned without changing its VAO pointer; its object ID is part of
 * the key, including the dedicated inline-buffer path in draw.c. */
void pgraph_gl_attribute_pointer(PGRAPHGLState *r, unsigned int index, GLuint buffer,
                                 GLint size, GLenum type, GLboolean normalized,
                                 GLsizei stride, uintptr_t offset, bool integer)
{
    if (r->no_fast_vertex) {
        if (integer) glVertexAttribIPointer(index, size, type, stride, (void *)offset);
        else glVertexAttribPointer(index, size, type, normalized, stride, (void *)offset);
        return;
    }
    typeof(r->vertex_attrib[0]) *a = &r->vertex_attrib[index];
    if (!a->pointer_valid || a->buffer != buffer ||
        a->size != size || a->type != type || a->normalized != normalized ||
        a->stride != stride || a->offset != offset || a->integer != integer) {
        if (integer) glVertexAttribIPointer(index, size, type, stride, (void *)offset);
        else glVertexAttribPointer(index, size, type, normalized, stride, (void *)offset);
        a->buffer = buffer;
        a->size = size;
        a->type = type;
        a->normalized = normalized;
        a->stride = stride;
        a->offset = offset;
        a->integer = integer;
        a->pointer_valid = true;
    }
    static unsigned int checks;
    if (nv2a_gl_debug() && (++checks % 257 == 0)) {
        GLint actual;
        void *pointer;
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &actual);
        assert((GLuint)actual == buffer);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &actual);
        assert(actual == size || (size == GL_BGRA && actual == 4));
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &actual);
        assert((GLenum)actual == type);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &actual);
        assert(actual == stride);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, &actual);
        assert(!!actual == integer);
        glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &actual);
        assert(!!actual == normalized);
        glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
        assert((uintptr_t)pointer == offset);
    }
}

static void update_memory_buffer(NV2AState *d, hwaddr addr, hwaddr size,
                                 bool quick)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (r->no_fast_vertex) glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);

    hwaddr end = TARGET_PAGE_ALIGN(addr + size);
    addr &= TARGET_PAGE_MASK;
    assert(end < memory_region_size(d->vram));

    static hwaddr last_addr, last_end;
    if (quick && (addr >= last_addr) && (end <= last_end)) {
        return;
    }
    last_addr = addr;
    last_end = end;

    size = end - addr;
    if (memory_region_test_and_clear_dirty(d->vram, addr, size,
                                           DIRTY_MEMORY_NV2A)) {
        glBufferSubData(GL_ARRAY_BUFFER, addr, size,
                        d->vram_ptr + addr);
        nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_1);
    }
}

void pgraph_gl_update_entire_memory_buffer(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, memory_region_size(d->vram), d->vram_ptr);
}

void pgraph_gl_bind_vertex_attributes(NV2AState *d, unsigned int min_element,
                                   unsigned int max_element, bool inline_data,
                                   unsigned int inline_stride,
                                   unsigned int provoking_element)
{
    PROF_SCOPE(PS_VERTEX_ATTRS);
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    bool updated_memory_buffer = false;
    unsigned int num_elements = max_element - min_element + 1;
    /* All active attributes in this path source the same buffer. Bind once;
     * dirty-range uploads below must still run even when pointers are cached. */
    if (!r->no_fast_vertex) {
        glBindBuffer(GL_ARRAY_BUFFER, inline_data ? r->gl_inline_array_buffer : r->gl_memory_buffer);
    }

    if (inline_data) {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d inline stride: %d)",
                             __func__, num_elements, inline_stride);
    } else {
        NV2A_GL_DGROUP_BEGIN("%s (num_elements: %d)", __func__, num_elements);
    }

    pg->compressed_attrs = 0;

    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];

        if (!attr->count) {
            pgraph_gl_attribute_enable(r, i, false);
            pgraph_gl_attribute_value(r, i, attr->inline_value);
            continue;
        }

        NV2A_DPRINTF("vertex data array format=%d, count=%d, stride=%d\n",
                     attr->format, attr->count, attr->stride);

        GLint gl_count = attr->count;
        GLenum gl_type;
        GLboolean gl_normalize;
        bool needs_conversion = false;

        switch (attr->format) {
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            // http://www.opengl.org/registry/specs/ARB/vertex_array_bgra.txt
            gl_count = GL_BGRA;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL:
            gl_type = GL_UNSIGNED_BYTE;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S1:
            gl_type = GL_SHORT;
            gl_normalize = GL_TRUE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F:
            gl_type = GL_FLOAT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K:
            gl_type = GL_SHORT;
            gl_normalize = GL_FALSE;
            break;
        case NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_CMP:
            /* 3 signed, normalized components packed in 32-bits. (11,11,10) */
            gl_type = GL_INT;
            assert(attr->count == 1);
            needs_conversion = true;
            break;
        default:
            fprintf(stderr, "Unknown vertex type: 0x%x\n", attr->format);
            assert(!"Unknown vertex type");
            break;
        }

        nv2a_profile_inc_counter(NV2A_PROF_ATTR_BIND);
        hwaddr attrib_data_addr;
        size_t stride;

        if (needs_conversion) {
            pg->compressed_attrs |= (1 << i);
        }

        hwaddr start = 0;
        if (inline_data) {
            if (r->no_fast_vertex) glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
            attrib_data_addr = attr->inline_array_offset;
            stride = inline_stride;
        } else {
            hwaddr dma_len;
            uint8_t *attr_data = (uint8_t *)nv_dma_map(
                d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
                &dma_len);
            assert(attr->offset < dma_len);
            attrib_data_addr = attr_data + attr->offset - d->vram_ptr;
            stride = attr->stride;
            start = attrib_data_addr + min_element * stride;
            update_memory_buffer(d, start, num_elements * stride,
                                        updated_memory_buffer);
            updated_memory_buffer = true;
        }

        uint32_t provoking_element_index = provoking_element - min_element;
        size_t element_size = attr->size * attr->count;
        assert(element_size <= sizeof(attr->inline_value));
        const uint8_t *last_entry;

        if (inline_data) {
            last_entry = (uint8_t*)pg->inline_array + attr->inline_array_offset;
        } else {
            last_entry = d->vram_ptr + start;
        }
        if (!stride) {
            // Stride of 0 indicates that only the first element should be
            // used.
            pgraph_update_inline_value(attr, last_entry);
            pgraph_gl_attribute_enable(r, i, false);
            pgraph_gl_attribute_value(r, i, attr->inline_value);
            continue;
        }

        pgraph_gl_attribute_pointer(r, i,
            inline_data ? r->gl_inline_array_buffer : r->gl_memory_buffer,
            gl_count, gl_type, needs_conversion ? GL_FALSE : gl_normalize,
            stride, attrib_data_addr, needs_conversion);
        pgraph_gl_attribute_enable(r, i, true);
        last_entry += stride * provoking_element_index;
        pgraph_update_inline_value(attr, last_entry);
    }

    NV2A_GL_DGROUP_END();
}

unsigned int pgraph_gl_bind_inline_array(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    unsigned int offset = 0;
    for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
        VertexAttribute *attr = &pg->vertex_attributes[i];
        if (attr->count == 0) {
            continue;
        }

        /* FIXME: Double check */
        offset = ROUND_UP(offset, attr->size);
        attr->inline_array_offset = offset;
        NV2A_DPRINTF("bind inline attribute %d size=%d, count=%d\n",
            i, attr->size, attr->count);
        offset += attr->size * attr->count;
        offset = ROUND_UP(offset, attr->size);
    }

    unsigned int vertex_size = offset;
    unsigned int index_count = pg->inline_array_length*4 / vertex_size;

    NV2A_DPRINTF("draw inline array %d, %d\n", vertex_size, index_count);

    nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_2);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_array_buffer);
    GLsizeiptr buffer_size = index_count * vertex_size;
    glBufferData(GL_ARRAY_BUFFER, buffer_size, NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, buffer_size, pg->inline_array);
    pgraph_gl_bind_vertex_attributes(d, 0, index_count-1, true, vertex_size,
                                  index_count-1);

    return index_count;
}

static void vertex_cache_entry_init(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    memcpy(&vnode->key, key, sizeof(struct VertexKey));
    vnode->initialized = false;
}

static bool vertex_cache_entry_compare(Lru *lru, LruNode *node, const void *key)
{
    VertexLruNode *vnode = container_of(node, VertexLruNode, node);
    return memcmp(&vnode->key, key, sizeof(VertexKey));
}

static const size_t element_cache_size = 50*1024;

void pgraph_gl_init_buffers(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    lru_init(&r->element_cache);
    r->element_cache_entries = g_malloc_n(element_cache_size, sizeof(VertexLruNode));
    assert(r->element_cache_entries != NULL);
    GLuint element_cache_buffers[element_cache_size];
    glGenBuffers(element_cache_size, element_cache_buffers);
    for (int i = 0; i < element_cache_size; i++) {
        r->element_cache_entries[i].gl_buffer = element_cache_buffers[i];
        lru_add_free(&r->element_cache, &r->element_cache_entries[i].node);
    }

    r->element_cache.init_node = vertex_cache_entry_init;
    r->element_cache.compare_nodes = vertex_cache_entry_compare;

    GLint max_vertex_attributes;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_vertex_attributes);
    assert(max_vertex_attributes >= NV2A_VERTEXSHADER_ATTRIBUTES);

    glGenBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    glGenBuffers(1, &r->gl_inline_array_buffer);

    glGenBuffers(1, &r->gl_memory_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, r->gl_memory_buffer);
    glBufferData(GL_ARRAY_BUFFER, memory_region_size(d->vram),
                 NULL, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &r->gl_vertex_array);
    glBindVertexArray(r->gl_vertex_array);

    assert(!nv2a_gl_debug() || glGetError() == GL_NO_ERROR);
}

void pgraph_gl_finalize_buffers(PGRAPHState *pg)
{
    PGRAPHGLState *r = pg->gl_renderer_state;

    GLuint element_cache_buffers[element_cache_size];
    for (int i = 0; i < element_cache_size; i++) {
        element_cache_buffers[i] = r->element_cache_entries[i].gl_buffer;
    }
    glDeleteBuffers(element_cache_size, element_cache_buffers);
    lru_flush(&r->element_cache);

    g_free(r->element_cache_entries);
    r->element_cache_entries = NULL;

    glDeleteBuffers(NV2A_VERTEXSHADER_ATTRIBUTES, r->gl_inline_buffer);
    memset(r->gl_inline_buffer, 0, sizeof(r->gl_inline_buffer));

    glDeleteBuffers(1, &r->gl_inline_array_buffer);
    r->gl_inline_array_buffer = 0;

    glDeleteBuffers(1, &r->gl_memory_buffer);
    r->gl_memory_buffer = 0;

    glDeleteVertexArrays(1, &r->gl_vertex_array);
    r->gl_vertex_array = 0;
}
