# Vulkan assessment and second performance pass

## Decision

Vulkan is a credible backend candidate when host graphics submission and driver
work are limiting performance. It is not an automatic improvement: command
recording, pipeline creation, resource uploads, and synchronization still cost
time. It will not remove the guest game's CPU work or NV2A push-buffer decoding.
See the [Khronos profiling guide](https://docs.vulkan.org/guide/latest/profiling.html).

This repository includes Vulkan-aware GLSL generators, but no Vulkan renderer,
loader, allocator, SPIR-V compiler integration, or Vulkan presenter. The current
presenter and framebuffer interface expect OpenGL textures and fences.

Upstream xemu has an existing [NV2A Vulkan backend](https://github.com/xemu-project/xemu/tree/master/hw/xbox/nv2a/pgraph/vk).
Its [renderer header](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/vk/renderer.h)
depends on Vulkan headers, glslang, volk, SPIRV-Reflect, and Vulkan Memory Allocator.
Its renderer initializes separate command, pipeline, texture, surface, report,
compute, and display subsystems. Reusing it is preferable to inventing another
NV2A renderer, but it is an integration project rather than changing an API flag.

## Integration work to review before starting a backend port

1. Pin a compatible upstream revision; compare PGRAPH interfaces and shader
   uniform layouts with this port before importing files.
2. Build the dependencies for the existing 32-bit Windows target; check driver
   and extension requirements on NVIDIA, Deck/Proton, and Intel HD 530.
3. Adapt this port's write-watch epochs, lazy CPU-access downloads, alias parking,
   and shader warmup behavior. Losing these can erase a backend improvement.
4. Integrate presentation through supported shared-image synchronization or a
   native Vulkan presenter. Avoid a per-frame GPU-to-CPU-to-GPU image copy.
5. Retain the widescreen/UI rules, texture replacement, overlays, and scaled
   render targets; validate images and query/occlusion timing against OpenGL.
6. Compare both backends on the same workloads and target machines. Keep OpenGL
   available while Vulkan compatibility and performance are being established.

## Current implementation work

The second pass caches guest-VAO enable flags, constant attributes, and pointer
formats. Pointer identity includes the buffer object, offset, stride, type,
normalization, component count, and integer interpretation. Both array and inline
buffer paths use the same cache. Constant attribute values belong to the GL
context, while enable/pointer state belongs to the guest VAO; auxiliary passes
use separate VAOs and do not overwrite those constants.

Bind the common vertex array buffer once per draw instead of once per attribute.
Memory dirty checks and uploads remain active. `XB_NO_FAST_VERTEX=1` restores
the original attribute calls and per-attribute buffer binds for comparison.
`XB_GL_DEBUG=1` checks sampled actual VAO state against the requested state.

`XB_GPU_TIMING=1` samples one in 64 draw-flush command sequences using timestamp
queries. Results are read only after availability is reported; a full ring skips
sampling instead of waiting. `gpu draw sample:` lines are separate from the CPU
wall-time phase counters. These GPU spans include uploads/dependencies and can
include idle gaps while the CPU supplies commands; they are not full-frame GPU
time or a direct GPU-utilization measurement. Debug mode changes timings, so
use no-debug runs when assessing bottlenecks.

Detailed CPU phase timers previously issued timing reads throughout every draw
in normal play. They now require `XB_CPU_TIMING=1`; ordinary play avoids those
reads and phase accumulation. The runner enables CPU timing by default for
diagnostics; `--no-cpu-timing` tests ordinary-play settings. Basic FPS/write-watch
telemetry remains available. Reports distinguish absent phase measurements from
zero rendered draws. Surface statistics still reset when phase timing is off.

Runner options: `--no-fast-vertex`, `--gpu-timing`, `--no-cpu-timing`. Initial
debug cave run passed, including sampled actual VAO assertions and 32 existing
snapshot pixel comparisons. Final debug run with bind hoisting and phase timing
disabled is being collected in `scratch/perf/vertex-final-debug/`.

## Initial measurements (RTX 2070, scale 3, 120 Hz)

Window 120–184 seconds, screenshots/debug off, CPU phase and GPU sample timers on:

| Run | FPS min / mean / max | Vertex setup CPU us/draw | Whole pusher CPU us/draw | Sampled GPU span us/draw |
|---|---|---|---|---|
| `vertex-off-gpu` | 28 / 56.30 / 95 | 3.433 | 32.794 | 21.114 |
| `vertex-on-gpu` | 21 / 36.45 / 98 | 2.606 | 30.436 | 17.803 |

Vertex submission cost fell in these samples, but FPS is lower in the enabled
run's different fight. This does not establish an overall FPS gain. The baseline
was collected before removing unused cache bookkeeping from its disabled helper
branch; the final disabled branch issues the original GL calls directly. Raw
reports retain executable hashes; do not present these as identical-binary
measurements. The enabled run processed more draws in the same window, but draw
complexity also varies, so that alone is not proof of a throughput improvement.

Timestamp spans do not settle the Vulkan decision: they include dependencies and
CPU supply gaps, and the sampled meshes vary. A Vulkan prototype can target the
driver/submission costs but must be measured against the current backend. The
firm 60 fps objective remains open.
