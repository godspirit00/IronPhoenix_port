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
disabled passed in `scratch/perf/vertex-final-debug/`: actual VAO assertions and
all 32 snapshot comparisons passed. Reviewed cave combat/HUD screenshots.

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

## Ordinary-play validation and NVIDIA threading experiment

The final executable, with diagnostic timers/screenshots off, at scale 3 and the
normal 60 Hz setting, recorded 25 / 42.12 / 61 FPS sample min/mean/max in the
120–184 second window (`vertex-normal-play`). This confirms remaining drops.
The occasional 61 sample reflects the periodic FPS estimator, not a new cap.

The existing NVIDIA-profile helper now has a read-only `probe` command. The
temporary `tools/perf_nvthread_test.py` refuses pre-existing application/named
profiles and a running game; creates an application-only threaded-optimization
override; runs the test; and removes that newly created profile in `finally`.
It does not change the global driver profile. Cleanup and guard error paths are
covered by mocked tests without touching driver settings.

The approved temporary experiment recorded 23 / 38.09 / 60 FPS at the same
settings (`nv-thread-off`), with a different evolving fight. It does not establish
a performance improvement, so threaded optimization has **not** been left off.
Original state restored and verified by `nvprofile probe`:
`application_profile=0 named_profile=0`. Evidence:
`scratch/perf/nv-thread-test-state.txt`. No benchmark/monitor processes remain.

## Resume checkpoint

- Implementation: `8c69912`; following checkpoint records validation/helper tests.
- Current game executable SHA256:
  `62226e2eeb98f731ef071d863f4f8c9b248591770be72ef00ca60f003a523bd0`.
- Build passed; 11 reporting/driver-test guard and restoration tests pass.
- Generated GLSL is unchanged; no shader-cache version bump is needed.
- Next useful work: deterministic workload capture/replay and a Vulkan backend
  prototype using the upstream renderer, retaining OpenGL for A/B validation.
  Do not claim that Vulkan improves this game until the prototype is measured.
