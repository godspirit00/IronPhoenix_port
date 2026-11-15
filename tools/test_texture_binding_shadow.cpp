#include "../src/nv2a/compat/texture_binding_shadow.h"
#include <cassert>
#include <cstdio>

int main() {
    NvTextureBindingShadow state{};
    // First disabled draw establishes all targets; later disabled draws issue
    // no bindings and need not change the active stage.
    unsigned calls = 0;
    for (unsigned stage = 0; stage < 4; ++stage)
        for (unsigned target = 0; target < 4; ++target)
            calls += nv_texture_bind(&state, stage, target, 0);
    assert(calls == 16);
    for (unsigned stage = 0; stage < 4; ++stage)
        for (unsigned target = 0; target < 4; ++target)
            assert(!nv_texture_bind(&state, stage, target, 0));
    assert(nv_texture_select(&state, 0));
    assert(!nv_texture_select(&state, 0));
    assert(nv_texture_bind(&state, 0, 2, 7));
    assert(!nv_texture_bind(&state, 0, 2, 7));
    // A texture may be shared between stages, but each stage has its own bind.
    assert(nv_texture_bind(&state, 1, 2, 7));
    assert(!nv_texture_bind(&state, 1, 2, 7));
    assert(nv_texture_bind(&state, 0, 3, 8));
    assert(nv_texture_bind(&state, 0, 2, 0));
    assert(state.objects[0][3] == 8 && state.objects[1][2] == 7);
    // Surface copies and deletion/name recycling invalidate assumptions.
    nv_texture_binding_invalidate(&state);
    assert(nv_texture_select(&state, 1));
    assert(nv_texture_bind(&state, 1, 2, 7));
    assert(nv_texture_bind(&state, 0, 3, 8));
    assert(nv_texture_bind(&state, 3, 0, 0));
    puts("Texture binding state and name-reuse tests passed");
}
