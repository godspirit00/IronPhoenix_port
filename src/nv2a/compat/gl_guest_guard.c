/*
 * Guest-memory guard for GL calls that read or write client memory.
 *
 * Render-target pages in Xbox RAM are kept PAGE_NOACCESS (surface access callbacks, nv2a_host.c).
 * On Windows a GL driver touching such a page faults in user mode, our vectored handler disarms
 * the page and the driver continues. Under Wine/Proton the driver (Mesa) runs on the Unix side:
 * the fault never reaches our handler, Wine abandons the call half-way and returns
 * STATUS_ACCESS_VIOLATION - leaving Mesa's shared texture mutex locked, so the next
 * glTexImage2D blocks forever (the SteamOS match-start hang; reproduced with Wine 9 + llvmpipe).
 *
 * These wrappers, installed over glad's function pointers, disarm exactly the guest bytes a call
 * will touch before calling the driver - the same thing the fault handler does for an emulator
 * thread. Pointers outside guest RAM pass straight through.
 */
#include "nv2a_compat.h"
#include <glad/gl.h>

void nv2a_gl_guest_access(const void *p, size_t len, bool write); /* nv2a_host.c */
bool nv2a_gl_is_guest_ptr(const void *p);

static PFNGLTEXIMAGE2DPROC real_TexImage2D;
static PFNGLTEXSUBIMAGE2DPROC real_TexSubImage2D;
static PFNGLTEXIMAGE3DPROC real_TexImage3D;
static PFNGLTEXSUBIMAGE3DPROC real_TexSubImage3D;
static PFNGLCOMPRESSEDTEXIMAGE2DPROC real_CompressedTexImage2D;
static PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC real_CompressedTexSubImage2D;
static PFNGLCOMPRESSEDTEXIMAGE3DPROC real_CompressedTexImage3D;
static PFNGLBUFFERDATAPROC real_BufferData;
static PFNGLBUFFERSUBDATAPROC real_BufferSubData;
static PFNGLREADPIXELSPROC real_ReadPixels;
static PFNGLGETTEXIMAGEPROC real_GetTexImage;

static unsigned int pixel_bytes(GLenum format, GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV:
        return 1;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        return 2;
    case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_24_8: case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    }
    unsigned int size;
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: size = 1; break;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: size = 2; break;
    case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: size = 4; break;
    default: return 16; /* unknown: assume the largest pixel */
    }
    unsigned int comps;
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_RED_INTEGER:
    case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
        comps = 1; break;
    case GL_RG: case GL_RG_INTEGER: case GL_DEPTH_STENCIL:
        comps = 2; break;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER:
        comps = 3; break;
    default:
        comps = 4; break;
    }
    return size * comps;
}

/* bytes a w*h*d image transfer touches, from the current (UN)PACK state of the context */
static size_t image_bytes(bool pack, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type)
{
    if (w <= 0 || h <= 0 || d <= 0) {
        return 0;
    }
    GLint row_length = 0, image_height = 0, alignment = 4;
    glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &row_length);
    glGetIntegerv(pack ? GL_PACK_IMAGE_HEIGHT : GL_UNPACK_IMAGE_HEIGHT, &image_height);
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &alignment);
    size_t bpp = pixel_bytes(format, type);
    size_t row = (size_t)(row_length > 0 ? row_length : w) * bpp;
    if (alignment > 1) {
        row = (row + alignment - 1) / alignment * alignment;
    }
    size_t rows = image_height > 0 ? image_height : h;
    return row * rows * (d - 1) + row * (h - 1) + (size_t)w * bpp;
}

static void guard_image(bool pack, const void *p, GLsizei w, GLsizei h, GLsizei d, GLenum format, GLenum type)
{
    if (p && nv2a_gl_is_guest_ptr(p)) {
        nv2a_gl_guest_access(p, image_bytes(pack, w, h, d, format, type), pack);
    }
}
static void guard_bytes(const void *p, size_t len, bool write)
{
    if (p && len && nv2a_gl_is_guest_ptr(p)) {
        nv2a_gl_guest_access(p, len, write);
    }
}

static void GLAD_API_PTR w_TexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty,
                                      const void *p)
{
    guard_image(false, p, w, h, 1, f, ty);
    real_TexImage2D(t, l, ifmt, w, h, b, f, ty, p);
}
static void GLAD_API_PTR w_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty,
                                         const void *p)
{
    guard_image(false, p, w, h, 1, f, ty);
    real_TexSubImage2D(t, l, x, y, w, h, f, ty, p);
}
static void GLAD_API_PTR w_TexImage3D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f,
                                      GLenum ty, const void *p)
{
    guard_image(false, p, w, h, d, f, ty);
    real_TexImage3D(t, l, ifmt, w, h, d, b, f, ty, p);
}
static void GLAD_API_PTR w_TexSubImage3D(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d,
                                         GLenum f, GLenum ty, const void *p)
{
    guard_image(false, p, w, h, d, f, ty);
    real_TexSubImage3D(t, l, x, y, z, w, h, d, f, ty, p);
}
static void GLAD_API_PTR w_CompressedTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei n,
                                                const void *p)
{
    guard_bytes(p, n > 0 ? (size_t)n : 0, false);
    real_CompressedTexImage2D(t, l, ifmt, w, h, b, n, p);
}
static void GLAD_API_PTR w_CompressedTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f,
                                                   GLsizei n, const void *p)
{
    guard_bytes(p, n > 0 ? (size_t)n : 0, false);
    real_CompressedTexSubImage2D(t, l, x, y, w, h, f, n, p);
}
static void GLAD_API_PTR w_CompressedTexImage3D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b,
                                                GLsizei n, const void *p)
{
    guard_bytes(p, n > 0 ? (size_t)n : 0, false);
    real_CompressedTexImage3D(t, l, ifmt, w, h, d, b, n, p);
}
static void GLAD_API_PTR w_BufferData(GLenum t, GLsizeiptr n, const void *p, GLenum usage)
{
    guard_bytes(p, n > 0 ? (size_t)n : 0, false);
    real_BufferData(t, n, p, usage);
}
static void GLAD_API_PTR w_BufferSubData(GLenum t, GLintptr off, GLsizeiptr n, const void *p)
{
    guard_bytes(p, n > 0 ? (size_t)n : 0, false);
    real_BufferSubData(t, off, n, p);
}
static void GLAD_API_PTR w_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, void *p)
{
    guard_image(true, p, w, h, 1, f, ty);
    real_ReadPixels(x, y, w, h, f, ty, p);
}
static void GLAD_API_PTR w_GetTexImage(GLenum t, GLint l, GLenum f, GLenum ty, void *p)
{
    if (p && nv2a_gl_is_guest_ptr(p)) {
        GLint w = 0, h = 0, d = 0;
        glGetTexLevelParameteriv(t, l, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(t, l, GL_TEXTURE_HEIGHT, &h);
        glGetTexLevelParameteriv(t, l, GL_TEXTURE_DEPTH, &d);
        nv2a_gl_guest_access(p, image_bytes(true, w, h, d > 0 ? d : 1, f, ty), true);
    }
    real_GetTexImage(t, l, f, ty, p);
}

#define WRAP(name)                          \
    do {                                    \
        if (glad_gl##name && !real_##name) { \
            real_##name = glad_gl##name;    \
            glad_gl##name = w_##name;       \
        }                                   \
    } while (0)

/* Call once after gladLoadGL (pointers are process-wide, shared by all contexts). */
void nv2a_gl_install_guest_guard(void)
{
    WRAP(TexImage2D);
    WRAP(TexSubImage2D);
    WRAP(TexImage3D);
    WRAP(TexSubImage3D);
    WRAP(CompressedTexImage2D);
    WRAP(CompressedTexSubImage2D);
    WRAP(CompressedTexImage3D);
    WRAP(BufferData);
    WRAP(BufferSubData);
    WRAP(ReadPixels);
    WRAP(GetTexImage);
}
