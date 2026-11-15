# Renderer investigation — 2026-11-15

User reports Vulkan is slower than OpenGL during extended play. Treat the
Vulkan implementation as experimental; previous unpaired FPS samples were not
evidence of a Vulkan speedup.

## Investigation in progress

- Added opt-in CPU scopes to Vulkan draw, texture, shader, vertex, surface,
  blit and report work. The existing `XB_CPU_TIMING` switch controls them.
- Separately time Vulkan finish calls, main fence waits, helper queue-idle waits,
  and the GL presenter completion wait. These scopes nest: do not sum them.
- Comparing the same executable, script, 60 Hz and scale 3 across both backends.
  Combat is not deterministic, so these are workload observations, not exact
  replay comparisons. Resolution sensitivity requires a scale-1 run as well.

## Evidence from source

Vulkan's `pgraph_vk_end_single_time_commands` waits for `vkQueueWaitIdle` on
every helper submission. `pgraph_vk_finish` waits for the main submission fence
before recycling buffers and descriptors. Display output also waits for the GL
presenter before overwriting its shared image. These are correctness constraints
of the current implementation, not proof that an individual wait dominates.

The Vulkan backend has not received the GL render-target alias parking or RAM
hash rules. Its surface invalidation path downloads and invalidates overlapping
targets; the previous Vulkan combat logs show roughly three downloads and three
uploads per displayed frame. Sampling timers will quantify their cost.

## High-level D3D8 replacement feasibility

The checked-in call inventory (`notes/d3d_calls_from_text.txt`) identifies 127
distinct D3D/XGRPH targets and 503 static call sites, with 13 unidentified targets.
These are static references, not dynamic call counts or complete coverage.
Known calls include DrawIndexedVertices, DrawVerticesUP, shader creation/binding,
state blocks, resource registration/locking, render-target changes, visibility
queries, and RunPushBuffer. The latter at 0x232cf0 reads the Xbox pushbuffer
resource and executes/references GPU commands; replacing ordinary draw functions
alone cannot cover that path. Inline command generation and indirect calls also
need auditing.

Intercepting Xbox D3D8 above NV2A is a plausible way to bypass guest command
generation/decoding and track resource ownership directly. It still requires
translation of Xbox shaders, vertex formats, swizzled/compressed textures,
render states and pushbuffers. It is high-level emulation/translation, not a
drop-in desktop D3D8 DLL or removal of every emulated subsystem. The Xbox CPU
code already executes natively; audio/kernel behavior remains separate.

Do not remove the working renderer or promise firm 60 fps on any target based
on this audit. First establish which costs a higher-level path would bypass.

## Scale-3 comparison

Both runs: same executable SHA-256
`ae414e5f6fa4ff9404ccbf48588bdb3d3762a204be771bd87f2c4de38c7f699b`,
same script, 60 Hz, scale 3, CPU timers enabled, screenshots every ten seconds,
120–184 second measurement window. Screenshots confirm Buddha Cave gameplay.
The user also identifies Buddha Cave as the affected map. These encounters are
not deterministic; a repeatable speedup ratio cannot be inferred from FPS alone.

| Observation | OpenGL | Vulkan |
|---|---:|---:|
| FPS min / mean / max | 47 / 57.44 / 60 | 23 / 29.56 / 39 |
| `draw_begin` CPU us/callback | 8.449 | 14.894 |
| `flush_draw` CPU us/callback | 3.814 | 23.738 |
| Pusher total wall time in window | 50.765 s | 62.451 s |
| Surface download scope total | 0 | 5.736 s |
| Surface upload scope total | 0 | 6.151 s |
| Main submission fence wait | n/a | 18.403 s |
| Helper queue-idle wait | n/a | 2.578 s |
| Presenter fence wait | n/a | 0.005 s |

Times are CPU wall durations, include nested work, and **must not be added**.
In particular surface invalidation includes finish/wait work. Most upload scope
calls are cheap early returns; the aggregate includes real uploads as well.
Vulkan's pusher occupied about 98% of this window; main submission waits alone
occupied about 29%. This is evidence of CPU/submission/synchronization overhead,
not a measurement of GPU busy time. The presenter fence is not the dominant cost.

GL logs show hundreds of alias park/restore operations per reporting interval
and no surface downloads/uploads. Vulkan reports roughly three of each per
frame, plus extra flushes for vertex-buffer dirtiness. Its per-draw submission
cost remains substantially higher even after accounting for those waits.
Pipeline generation is largely absent from the measured window; startup shader
compilation is not a sufficient explanation for this sustained regression.

NVIDIA telemetry sampled during late combat: Vulkan averaged 55.27% GPU usage,
1890 MHz graphics clock over 15 samples; GL averaged 51.67%, 1879 MHz over 18.
These sparse whole-GPU readings are corroborating evidence, not GPU frame
timings. Vulkan CSV also contains post-exit idle samples: analysis excludes
timestamps after the game ended, using the runtime-log artifact write time.

Raw artifacts: `scratch/perf/investigate-{gl,vk}-scale3/`; the extra
`phase-report.json` files include summed phase wall times. No driver settings
were changed.

## Native-resolution probe

`investigate-vk-scale1` uses the same executable, script and measurement window
at scale 1. It recorded 22 / 43.69 / 60 FPS min/mean/max. Main fence waits fell
to 8.997 seconds, but pusher wall time remained 59.343 seconds in the 64-second
window. GPU telemetry during 15 late-combat samples averaged 25.27% utilization.
Reduced resolution helps this run, but it still does not meet firm 60 fps.
Combat/draw complexity differs, so the FPS change is not an isolated resolution
scaling measurement. This does not prove that either GPU work or CPU work alone
explains every dropped frame.

OpenGL scale-1 validation is in progress. Its run records FPS over 120–184
seconds, then samples CPU stacks over 190–220 seconds, avoiding profiler
suspension overhead inside the FPS window. The runner now supports separate
`--end` and `--profile START,SECONDS` options, preserving the profile with that
run's artifacts. `symprof.py --profile PATH --exe PATH` can symbolize a saved
profile against its exact binary. These diagnostic tools do not replace a
future deterministic capture/replay workload.

## Proposed high-level architecture

A game-specific D3D8 frontend should own a mirror of draw state and resource
lifetimes, and submit host draw packets without the guest PFIFO/MMIO/pusher
round trip. Render targets should stay GPU-resident until an explicit CPU read
requires synchronization. Shader and texture translation can reuse audited
pure conversion routines; resource ownership needs a separate design.

Audit direct pushbuffer execution, inline command writers, state blocks,
callbacks, locks and visibility-result ordering before disabling low-level
submission. Mixing an intercepted draw with unobserved guest state changes
would silently render incorrectly. The first milestone should cover a bounded
menu/pass with complete state and compare its output against the working GL
path; then extend to all gameplay/resource operations and measure busy combat.
Do not label a menu-only prototype a finished renderer.

The host graphics API is a separate choice: a high-level frontend can target
either Vulkan or OpenGL. Replacing Xbox D3D8 does not require desktop D3D8.
As an architectural reference, Cxbx's
[D3D8-to-D3D9 implementation](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/Direct3D9/Direct3D9.cpp)
also has separate resource, shader, render-state and pushbuffer translation.
No implementation code from that project was imported here.
