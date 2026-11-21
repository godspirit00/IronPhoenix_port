#ifndef NV2A_GL_GPU_PROFILE_H
#define NV2A_GL_GPU_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

/* XB_GPU_PROFILE=1: GPU time per operation category (see gpu_profile.c). */
enum {
    GPC_DRAW, GPC_SURF_TO_TEX, GPC_CLEAR, GPC_SURF_UPLOAD, GPC_SURF_DOWNLOAD, GPC_TEX_UPLOAD,
    GPC__COUNT
};

bool gpu_prof_enabled(void);
int gpu_prof_begin(int cat);
void gpu_prof_set_key(int slot, unsigned int key, unsigned int verts);
void gpu_prof_end(int slot);
void gpu_prof_end_p(int *slot);
void gpu_prof_flip(void);

/* Times the rest of the enclosing block on the GPU timeline. */
#define GPU_PROF_SCOPE(cat) \
    int gpu_prof_slot_##cat __attribute__((cleanup(gpu_prof_end_p))) = gpu_prof_begin(cat)

#endif
