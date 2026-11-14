/* Standalone host adapters for the xemu Vulkan backend. */
#pragma once
#include "epoxy/gl.h"
#include <windows.h>
typedef uint8_t guint8;
static inline unsigned long *bitmap_new(size_t bits) { return g_malloc0(BITS_TO_LONGS(bits) * sizeof(unsigned long)); }
static inline size_t find_next_bit(const unsigned long *map, size_t size, size_t start)
{
    if (start >= size) return size;
    size_t word = start / BITS_PER_LONG;
    unsigned long value = map[word] & (~0UL << (start % BITS_PER_LONG));
    while (!value) {
        if (++word * BITS_PER_LONG >= size) return size;
        value = map[word];
    }
    return MIN(word * BITS_PER_LONG + __builtin_ctzl(value), size);
}
static inline void vk_surface_trace(uint32_t a, uint32_t w, uint32_t h,
                                    const char *layout, uint32_t aa, uint32_t x,
                                    uint32_t cw, uint32_t y, uint32_t ch, uint32_t pitch) {}
#undef trace_nv2a_pgraph_surface_match_color
#undef trace_nv2a_pgraph_surface_match_zeta
#undef trace_nv2a_pgraph_surface_create_color
#undef trace_nv2a_pgraph_surface_create_zeta
#undef trace_nv2a_pgraph_surface_hit_color
#undef trace_nv2a_pgraph_surface_hit_zeta
#define trace_nv2a_pgraph_surface_match_color vk_surface_trace
#define trace_nv2a_pgraph_surface_match_zeta vk_surface_trace
#define trace_nv2a_pgraph_surface_create_color vk_surface_trace
#define trace_nv2a_pgraph_surface_create_zeta vk_surface_trace
#define trace_nv2a_pgraph_surface_hit_color vk_surface_trace
#define trace_nv2a_pgraph_surface_hit_zeta vk_surface_trace
typedef struct GArray {
    char *data;
    unsigned int len;
    size_t element_size, capacity;
    bool zero_terminated, clear;
} GArray;
typedef GArray GByteArray;
static inline GArray *g_array_sized_new(bool zero, bool clear, size_t size, unsigned int reserve)
{
    GArray *a = g_malloc0(sizeof(*a));
    a->element_size = size; a->zero_terminated = zero; a->clear = clear;
    a->capacity = (reserve + (zero ? 1 : 0)) * size;
    a->data = g_malloc0(a->capacity);
    return a;
}
static inline GArray *g_array_new(bool zero, bool clear, size_t size)
{ return g_array_sized_new(zero, clear, size, 0); }
static inline GArray *g_array_set_size(GArray *a, unsigned int count)
{
    assert(count <= (SIZE_MAX / a->element_size) - 1);
    size_t bytes = (count + (a->zero_terminated ? 1 : 0)) * a->element_size;
    if (bytes > a->capacity) {
        a->capacity = MAX(bytes, MAX((size_t)64, a->capacity * 2));
        a->data = g_realloc(a->data, a->capacity);
    }
    if (a->clear && count > a->len)
        memset(a->data + a->len * a->element_size, 0, (count - a->len) * a->element_size);
    a->len = count;
    if (a->zero_terminated) memset(a->data + count * a->element_size, 0, a->element_size);
    return a;
}
static inline GArray *g_array_append_vals(GArray *a, const void *values, unsigned int count)
{
    unsigned int old = a->len;
    g_array_set_size(a, old + count);
    if (count) memcpy(a->data + old * a->element_size, values, count * a->element_size);
    return a;
}
#define g_array_append_val(a, value) g_array_append_vals((a), &(value), 1)
#define g_array_index(a, T, i) (((T *)(a)->data)[i])
static inline char *g_array_free(GArray *a, bool segment)
{
    if (!a) return NULL;
    char *data = a->data;
    if (segment) { g_free(data); data = NULL; }
    g_free(a);
    return data;
}
static inline void vk_array_cleanup(void *p) { g_array_free(*(GArray **)p, true); }
#undef g_autoptr
#define g_autoptr(T) T * __attribute__((cleanup(vk_array_cleanup)))
static inline GByteArray *g_byte_array_new_take(void *data, size_t size)
{
    GByteArray *a = g_malloc0(sizeof(*a));
    a->data = data; a->len = size; a->capacity = size; a->element_size = 1;
    return a;
}
static inline void g_byte_array_unref(GByteArray *a) { g_array_free(a, true); }
static inline void error_prepend(Error **errp, const char *fmt, ...)
{
    if (!errp || !*errp) return;
    va_list ap; va_start(ap, fmt);
    char *prefix = g_strdup_vprintf(fmt, ap); va_end(ap);
    char *msg = g_strdup_printf("%s%s", prefix, (*errp)->msg);
    g_free(prefix); g_free((*errp)->msg); (*errp)->msg = msg;
}
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT 0x9587
#define GL_TEXTURE_TILING_EXT 0x9580
#define GL_NUM_TILING_TYPES_EXT 0x9582
#define GL_TILING_TYPES_EXT 0x9583
#define GL_OPTIMAL_TILING_EXT 0x9584
#define GL_LINEAR_TILING_EXT 0x9585
extern void (APIENTRY *glCreateMemoryObjectsEXT)(GLsizei, GLuint *);
extern void (APIENTRY *glDeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
extern void (APIENTRY *glImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void *);
extern void (APIENTRY *glTexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
bool pgraph_vk_port_init_interop(Error **errp);
void pgraph_vk_port_present_complete(void);
void pgraph_vk_port_wait_present(void);
