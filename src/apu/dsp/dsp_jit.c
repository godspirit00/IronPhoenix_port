/*
 * DSP56300 JIT backend stub for the Iron Phoenix runtime.
 * xemu's JIT depends on an external Rust/C++ library; the C interpreter is used instead.
 */
#include "qemu/osdep.h"
#include "dsp_internal.h"

const DSPOps jit_dsp_ops = {0};

void dsp_jit_init(DSPState *dsp)
{
    /* Fall back to the interpreter */
    dsp_c_init(dsp);
}
