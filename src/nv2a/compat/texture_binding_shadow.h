#pragma once
#include <stdbool.h>
#include <stdint.h>

// Four NV2A stages, four GL texture targets per stage. Object names describe
// bindings only; they never authorize skipping texture-content validation.
typedef struct NvTextureBindingShadow {
    uint32_t objects[4][4];
    uint16_t valid;
    unsigned active;
    bool active_valid;
} NvTextureBindingShadow;

static inline void nv_texture_binding_invalidate(NvTextureBindingShadow *s) {
    s->valid = 0;
    s->active_valid = false;
}
static inline bool nv_texture_select(NvTextureBindingShadow *s, unsigned stage) {
    bool changed = !s->active_valid || s->active != stage;
    s->active = stage; s->active_valid = true;
    return changed;
}
static inline bool nv_texture_bind(NvTextureBindingShadow *s, unsigned stage,
                                   unsigned target, uint32_t object) {
    uint16_t bit = (uint16_t)(1u << (stage * 4 + target));
    bool changed = !(s->valid & bit) || s->objects[stage][target] != object;
    s->valid |= bit;
    s->objects[stage][target] = object;
    return changed;
}
