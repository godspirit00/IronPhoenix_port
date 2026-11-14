#include "qemu/osdep.h"
#include "port.h"
/* Shared GL contexts: presenter publishes completion before the Vulkan worker
 * reuses or destroys the externally backed display image. renderer_lock and
 * the display request event serialize access to this fence. */
static GLsync present_fence;
void pgraph_vk_port_present_complete(void)
{
    if (present_fence) glDeleteSync(present_fence);
    present_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}
void pgraph_vk_port_wait_present(void)
{
    if (!present_fence) return;
    GLenum status;
    do {
        status = glClientWaitSync(present_fence, 0, 100000000);
    } while (status == GL_TIMEOUT_EXPIRED);
    assert(status != GL_WAIT_FAILED);
    glDeleteSync(present_fence);
    present_fence = NULL;
}
void (APIENTRY *glCreateMemoryObjectsEXT)(GLsizei, GLuint *);
void (APIENTRY *glDeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
void (APIENTRY *glImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void *);
void (APIENTRY *glTexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);
bool pgraph_vk_port_init_interop(Error **errp)
{
#define LOAD_GL(name) do { name = (void *)wglGetProcAddress(#name); \
    if (!name) { error_setg(errp, "Vulkan presenter requires OpenGL interop entry point %s", #name); return false; } } while (0)
    LOAD_GL(glCreateMemoryObjectsEXT);
    LOAD_GL(glDeleteMemoryObjectsEXT);
    LOAD_GL(glImportMemoryWin32HandleEXT);
    LOAD_GL(glTexStorageMem2DEXT);
#undef LOAD_GL
    return true;
}
