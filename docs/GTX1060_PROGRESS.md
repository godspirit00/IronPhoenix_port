# GTX 1060 / 4-thread CPU pass (2026-11-21)

Test machine: NVIDIA GeForce GTX 1060 6 GB (driver 572.47, 78 W), Intel Core i5-7500
(4 cores / 4 threads, 3.4 GHz), Windows 10 22H2. Branch `gtx1060` from `main` `1a10c89`.
Settings unless noted: OpenGL, scale 3 (1920x1440), `--hz 60`, no vsync, HLE bridges off
(ordinary user configuration), `tools/perf_run.py` routes from `tools/map_routes.py`.

## Measuring

The `perf: N fps` log figure is a 250 ms snapshot printed every 2 s. It hid a run where the
game flipped only every ~510 ms for over a minute while the snapshot still read 60. New
`frames:` line: exact flips per log interval and the worst flip-to-flip gap;
`perf_report.py` reports it as `fps_interval` (min / mean / worst gap / intervals below 58).
Use it, not `fps_samples`, for comparisons. `perf_maps.py --ordinary` runs the arena matrix
with the HLE bridges off.

## Where the time goes on this machine (Warship combat)

All-thread sampling profile, 45 s of combat:

| Thread | Busy | Notes |
| --- | --- | --- |
| Game (guest) | ~95% | ~42% of samples spinning in `D3DDevice_GetVisibilityTestResult` |
| Renderer (pfifo) | ~26% | mostly waiting for work |
| APU frame thread | ~60% | DSP interpreter (GP ~31.5k cycles per 32-sample frame, EP ~16k every 8th) |
| 2 voice workers | ~20% each | HRTF, mixing, libm |
| Present | blocked in `NtGdiDdDDIPresent` ~60% | |

GPU utilization ~70% (peaks 82%). On the RTX 2070 the renderer thread was the limit; on this
4-thread CPU the game thread and the APU are. APU utilization reaches 1.00 in heavy combat and
falls below real time (1340-1460 of 1500 frames/s), which also coincides with fps dips.

## Changes

| Change | Effect (Warship combat) | Kill switch |
| --- | --- | --- |
| `float_to_24b`: SSE2 `cvtsd2si` instead of UCRT `lrint` (32-bit UCRT saves/restores the FP environment per call; 1.5 M calls/s) | was ~14% of the APU frame thread's busy time | none (exact) |
| libsamplerate built with `ENABLE_SSE2_LRINT` and `NDEBUG` | `lrint` was ~58% of a voice worker's busy time | none (exact) |
| HRTF: mirrored history buffer, no integer modulo per tap | bit-exact vs the original over 1.28 M outputs (`scratch/test_hrtf.c`) | none |
| DSP interpreter TU built with `NDEBUG`; inline fast paths for plain X/Y RAM | DSP share 0.52 -> 0.45 of a core | build with `-DXB_DSP_ASSERTS` |
| `GetVisibilityTestResult` hook in late/exact modes: same results, but a pending query waits (<= 1 ms) on an event the renderer sets on every report write, instead of spinning | frees the game thread's spin (~40% of its time) for other threads; ~8 waits per frame | `XB_VIS_WAIT=0` |

APU utilization in the same combat: 0.70-1.00 (falling to 1340 frames/s) before, 0.55-0.76
(1500 frames/s) after. Warship interval average 59.2 -> 59.75 fps; it was already close to 60
here, so arena means do not separate the variants well.

### GPU: conservative depth (largest single gain on this machine)

`XB_GPU_PROFILE=1` (new) showed draws at ~95% of categorized GPU time at scale 3, ~12-14 ms
per frame in Warship's heavy phase; 85% of it in draws with depth test on and depth writes
off (particles, effects, and the colour pass after the game's depth pre-pass). One particle
program alone took ~5 ms per frame from ~18 draws.

Every emulated fragment shader (upstream xemu design) reconstructs NV2A depth per pixel from
the triangle's vertices and writes `gl_FragDepth`, which disables early depth rejection.
Experiments on the heavy phase (GPU draw time per 2 s):

| Variant | Draw GPU | Exact? |
| --- | --- | --- |
| Original | 1190-1350 ms | yes |
| Depth computed, not written (`gl_FragDepth` dropped) | 839 ms | no |
| No shader depth at all (hardware depth) | 652 ms | no |
| Exact depth written + `layout(depth_greater)` | 837 ms | yes, if written >= interpolated |

Conservative depth keeps the exact written value but lets the GPU reject fragments early
using the interpolated depth, provided the written depth is never below it. It is enabled
only for shader states where that holds: no w-buffer (`z_perspective`), depth-clip cull mode
(no far clamp), no polygon offset. The vertex shader lowers `gl_Position.z` by 512/2^24 of the
range (covers D16/D24 flooring and interpolation error); only the early test sees this
because every fragment shader writes `gl_FragDepth`, and `GL_DEPTH_CLAMP` is always on, so
nothing is clipped. Requires `GL_ARB_conservative_depth`; OpenGL only (the Vulkan generator
is unchanged; the Vulkan build was not compiled here because its toolchain is not set up on
this machine). `XB_NO_CONSERVATIVE_DEPTH=1` restores the original shaders.

Validation: `XB_CONSERVATIVE_VERIFY=1` makes those shaders count `gl_FragDepth <
gl_FragCoord.z` on the GPU with atomic counters (logged every 120 flips). Warship combat:
0 violations in ~161 billion fragments. Sensitivity checks: a 1e-3 threshold flags 100% of
fragments; without the vertex bias there were still 0 (NVIDIA), so the bias is margin for
other drivers. Screenshots show no missing or z-fighting geometry.

Result: Warship heavy phase 50-58 fps -> 60 fps; GPU time of depth-tested, non-writing
draws -38%.

All-arena verification (`XB_CONSERVATIVE_VERIFY=1 python tools/perf_maps.py --ordinary`,
`scratch/perf/g1060-cdverify-maps*`): 0 violations in every one of the 14 arenas, ~2 trillion
fragments in total; all routes reached their scenes; screenshots (water reflections, floor
decals, shadows, fire) correct.

Tried, no measurable effect: `XB_REPORT_SPIN=1` (renderer re-polls occlusion results
without a 1 ms timed wait): 59.64 vs 59.23 average, within run-to-run variation.

## Benchmark pitfalls on this machine

* Screenshots (`--shots`) stall the game ~0.5-1 s each at scale 3. Most hitches over 200 ms
  in the first matrix followed a screenshot. `perf_maps.py --shots N` now sets the interval.
* The Yama Hall route did not reach `se06` in the first ordinary-settings matrix.
* First visits to a map compile shaders synchronously on the renderer (30-330 ms each).

## Arena matrix (`perf_maps.py --ordinary`, scale 3, 60 Hz, window 115-155 s)

Interval fps min / mean / worst flip gap (ms) / 2 s intervals below 58. Matrix 1: start of
this pass (also screenshots every 5 s, cold shaders for most maps). Matrix 2: CPU-side
fixes (APU, libsamplerate, HRTF, occlusion wait), screenshots every 60 s. Matrix 3: plus
conservative depth. Matrix 4 (2026-11-22, commit `04512bd`): plus asynchronous log and
background shader compiles, warm shader cache.

| Arena | matrix 1 | matrix 2 | matrix 3 | matrix 4 |
| --- | --- | --- | --- | --- |
| great-wall | 58.0/59.71/71.1/0 | 60.0/60.0/17.8/0 | 53.0/59.24/168.6/2 | 59.5/60.0/20.5/0 |
| temple-of-paganism | 49.9/59.29/383.6/1 | 59.5/59.97/30.5/0 | 60.0/60.0/17.6/0 | 57.0/59.76/60.0/1 |
| dragon-valley | 53.7/59.7/233.7/1 | 60.0/60.0/17.6/0 | 55.5/59.76/102.7/1 | 59.5/59.95/34.5/0 |
| training-square | 51.9/59.08/333.8/3 | 58.0/59.76/23.9/0 | 59.5/59.97/39.2/0 | 59.5/60.03/17.6/0 |
| thousand-bone-tomb | 48.5/58.35/404.0/4 | 50.5/59.32/347.8/2 | 59.0/59.92/63.2/0 | 58.5/59.84/83.5/0 |
| castle-entrance | 56.5/59.82/143.3/1 | 60.0/60.0/20.6/0 | 35.1/58.71/622.9/1 | 59.5/60.03/17.6/0 |
| apparatus-maze | 56.8/59.88/111.8/1 | 58.0/59.82/97.7/0 | 59.0/59.95/39.6/0 | 57.5/59.74/108.2/1 |
| yama-hall | 60.0/60.0/29.1/0 | 60.0/60.0/21.0/0 | 59.5/59.97/34.5/0 | 59.5/60.0/17.6/0 |
| electric-shock-chamber | 54.1/59.73/220.1/1 | 49.5/59.0/615.3/2 | 60.0/60.0/20.6/0 | 59.5/60.0/19.3/0 |
| summoning-altar | 39.0/56.42/1079.5/5 | 54.0/58.87/36.7/3 | 60.0/60.0/17.7/0 | 59.5/60.0/28.4/0 |
| buddha-cave | 50.8/59.42/363.0/2 | 54.5/58.68/110.0/5 | 57.5/59.82/36.4/1 | 59.5/59.97/17.5/0 |
| night-demon-warship | 56.2/59.58/21.3/2 | 50.0/57.18/27.7/9 | 60.0/60.0/22.2/0 | 59.5/59.97/17.7/0 |
| training-square-night | 54.0/59.68/216.6/1 | 60.0/60.0/24.8/0 | 56.5/59.82/148.6/1 | 59.5/60.0/17.6/0 |
| red-vipers-den | 55.0/59.76/183.7/1 | 60.0/60.0/17.6/0 | 60.0/60.0/17.7/0 | 59.5/60.0/17.6/0 |

Sustained-load arenas improved most (Warship 57.2 -> 60.0, Summoning Altar 58.9 -> 60.0).
Matrix 4: every arena 59.74-60.03 mean, two intervals below 58 in total. Its larger gaps: the
120 s screenshot (Apparatus Maze 108 ms), two 60-80 ms gaps while a state still new to the
cache compiled in the background (~200 ms on the worker; Apparatus Maze, Thousand Bone Tomb),
and one 83 ms gap with nothing logged nearby (Thousand Bone Tomb 125 s).
Matrix 3's Great Wall and Training Square Night dips coincide with first-time shader compiles
(the conservative-depth variants are new cache entries; the cache is now warm). Gameplay is
not deterministic between runs, so single dips are not comparisons.

### Log writes stalled the vblank and renderer threads (2026-11-22)

Castle Entrance matrix 3's late gaps (622 ms, 390 ms) were not GPU or game work. Every log line
was an unbuffered `stderr` write plus an `fflush` of `ironphoenix.log`, done on the calling
thread under one global lock, and `E:` here is a 5400 rpm laptop HDD (ST1000LM048). The
622 ms gap started when the renderer logged its `Texture state` line, and the 390 ms one
fell between two consecutive lines of the vblank thread. No vblank means no flips, and the
vblank thread's 2 s stats interval stretched by the same amount (`frames: 84 in 2390 ms`).
Earlier runs show the same signature: matrix 1 Apparatus Maze, `frames: 120 in 2517 ms` after
two lines of ~260 ms each; `gpuprof2` `frames: 120 in 2629 ms`; matrix 1 Castle Entrance 501 ms.

Change: the log is asynchronous. Callers format and timestamp the line and copy it into a
1 MB buffer; a writer thread (above normal priority) writes batches to `stderr` and the file.
A full buffer makes the caller wait, so no line is dropped. Flushed (bounded, 3 s) by
`fatal`, the crash handler, the exit paths and `atexit`; the thread dump skips the writer.
A writer batch that takes >= 100 ms is reported (`log: write of N bytes took X ms`);
`XB_LOG_SYNC=1` restores direct writes and reports a calling thread blocked >= 100 ms.

A/B, Castle Entrance, HDD write pressure on `E:` (8 MB + 40 x 4 KB writes, each followed by
`fsync`, in a loop) from 120 s to 173 s, window 128-174 s (`scratch/perf/g1060-log2-*`):

| Variant | Interval fps min / mean | Worst gap | Notes |
| --- | --- | --- | --- |
| `XB_LOG_SYNC=1` | 42.9 / 59.19 | 796.9 ms | vblank thread blocked in a log write for 757 ms and 797 ms (the two gaps); two more threads waited 267 / 708 ms on the log lock |
| async (default) | 59.5 / 60.0 | 17.6 ms | no hitch, no slow write reported, log complete to the end |

A first attempt (stress from 100 s) was confounded: that run loaded the scene 6 s later, so
the stress hit the game's own asset reads from the same disk. Its log also ended 15 s before
the process was killed (writer at normal priority, starved or blocked; the writer is now
above normal and reports slow writes, and the rerun's log was complete).

### Background shader compiles (2026-11-22)

A shader state the cache had not seen was generated, compiled, linked and validated on the
renderer thread: 30-330 ms each, with bursts of 15-40 when a map loads or combat starts (first
visit, new driver, or new settings). Now the renderer queues the state and skips the draw calls
that need it (`r->shader_pending`; vertex and inline-attribute bookkeeping still runs). A worker
thread with its own shared GL context compiles the state to a program binary, writes it to the
disk cache, and hands it back; the renderer loads it with `glProgramBinary` on a later bind, the
same path as shaders preloaded from disk. Only the renderer modifies the shader LRU. With
`GL_KHR_parallel_shader_compile` (present on this driver) the worker issues a whole burst
without querying status and the driver compiles it on its own threads (`XB_SHADER_THREADS`,
default 2), publishing each program as it completes. A binary that fails to load or a state
the worker cannot link falls back to the synchronous path (which reports errors as before).
`XB_SHADER_ASYNC=0` restores synchronous compiles; use it with `XB_GPU_PROFILE` GLSL dumps.

Measured cold: both our shader cache and the driver's GLSL cache for this executable
(`%LOCALAPPDATA%\NVIDIA\GLCache\...\6461f0cef940c812.*`) renamed aside for each run and
restored afterwards. `__GL_SHADER_DISK_CACHE=0` does not disable the driver cache on Windows, and
runs with only our cache cleared compile in 0-10 ms each. Night Demon Warship, 68 states,
`scratch/perf/g1060-sa4-*`:

| Variant | Hitches >= 50 ms after 95 s | Worst | Frames 102-104 s | Map load burst: worst wait / skipped draws | Combat start burst |
| --- | --- | --- | --- | --- | --- |
| synchronous (`XB_SHADER_ASYNC=0`) | 14 | 1000 ms | 53 / 120 | - | - |
| async, one at a time (`XB_SHADER_THREADS=0`) | 2 | 77 ms | 106 / 120 | 2743 ms / 35,405 | 1139 ms / 7,543 |
| async, parallel (default) | 4 | 154 ms | 99 / 120 | 712 ms / 699 | 530 ms / 2,554 |

The parallel variant's 154 ms gap is the driver's compiler threads competing for the four
cores during the combat-start burst; it shortens missing-effect time 2-4x. Warm cache (nothing
to compile): async build vs `XB_SHADER_ASYNC=0`, Warship heavy phase 120 frames per 2 s in both,
interval mean 60.03 vs 59.84, so an idle worker costs nothing. One cold async run showed a 10-15%
lower heavy phase without compiles; it did not repeat (two later async runs matched sync) and
the sync runs vary as much. Screenshots after the bursts match the synchronous build.

### Vulkan on this machine (2026-11-22)

Toolchain: `python tools/prepare_vulkan.py` (pinned, checksum-verified sources; glslang built
with the bundled llvm-mingw in ~3 min, no CMake) and `VULKAN=1 OUT=build-vk ./build.sh` compile
cleanly here. The Vulkan executable keeps the OpenGL one intact when built into `build-vk`;
for runs it is copied to `build/ironphoenix-vk.exe` (saves live next to the executable).
`perf_maps.py` gained `--renderer` and `--exe`. Conservative depth and background shader
compiles are OpenGL-only; the Vulkan path otherwise renders Warship correctly.

Warship, scale 3, 60 Hz, ordinary settings: Vulkan 33.05 fps interval mean (OpenGL 59.97). Per
frame in the heavy phase (~1460 draws), compared with OpenGL: 157 render passes (OpenGL has no
equivalent cost), 1351 index uploads (OpenGL: element cache hits), 132 occlusion queries.

Fixed: every occlusion query was reset with `vkCmdResetQueryPool`, which is not allowed
inside a render pass, so each guest query boundary ended and restarted the render pass. The
pool is now reset in bulk at command buffer start (the preceding finish reads every result);
queries begin inside the render pass and end before it ends; report processing already sums
all queries between two guest reports. `XB_VK_QUERY_OUTSIDE_RP=1` restores upstream.

| Warship, CPU timing on (`g1060-vkq-*`) | Upstream queries | Queries in render pass |
| --- | --- | --- |
| Render passes per frame | 179 | 30 |
| Interval fps min / mean | 24.5 / 34.82 | 28.0 / 40.13 |
| Worst gap | 71.9 ms | 44.8 ms |
| `vis real` / `incomplete` per 2 s | 9132 / 401 | 8401 / 391 |

Remaining cost (queries in render pass, 39 s window; scopes nest): renderer busy 30.9 s,
blocked in `vkWaitForFences` after a submit 13.3 s (34%), `flush_draw` 21.3 s, textures 6.6 s,
shaders 4.8 s. Every submit waits for the GPU before continuing (~5.4 per frame): 3.25 because
the game rewrites vertex memory that an earlier draw in the same command buffer reads (the
backend mirrors guest vertex RAM into one persistent mapped buffer, so it must drain the GPU
before overwriting), 1.1 when the pusher runs dry (to write occlusion reports), 1 at flip.
Removing these waits needs renaming of rewritten vertex ranges (append the new data and rebind
those draws) and/or several command buffers in flight with lifetime tracking for staging
buffers, descriptor sets, framebuffers and deferred query results. Not attempted yet.

### Emulated depth and the audio DSP: measured, not changed (2026-11-22)

Warship heavy phase, GPU profiler, shader disk cache off (`XB_NO_SHADER_CACHE=1`, new: neither
loads nor writes the cache, so experiments never store shaders under ordinary keys):

* Geometry shader (every triangle draw passes one so the fragment shader gets all three vertex
  positions): skipping it for filled triangles (measurement-only build, wrong depth) left draw
  GPU time within run-to-run variation (1340-1656 vs 1389-1708 ms per 2 s). Not a cost.
* Per-triangle depth plane: the geometry shader already computes exact plane gradients for the
  polygon-offset slope; passing them on and evaluating `z0 + dzx*dx + dzy*dy` per fragment
  (w-buffer: on 1/w) replaces three barycentric areas and a division. Draw GPU time -6%
  (1451 vs 1545 ms per 2 s, 8.27 vs 8.67 us per draw). Not bit-exact: in combat 0.6% of
  fragments floor to a different integer depth, 0.12% by 2+ units (0 in menus; GPU counters,
  `g1060-dp-verify`). The earlier "22% of draw time" was computed-vs-no depth; most of it is
  the `gl_FragDepth` write and clip/discard code, not the barycentric formula. Reverted: a ~5%
  GPU gain does not justify giving up exact depth. The GPU is 74-85% busy in this phase.
* Audio DSP (APU frame thread, 30 s sampled, `g1060-apuprof*`): ~41% busy; dispatch, the
  per-instruction PC/interrupt epilogue, parallel-move decode, CCR update and address
  calculation each take a similar share. Inlining the epilogue's common checks with the rare
  branches out of line: no change (DSP share 0.45 before and after); a fast path that skipped
  the epilogue when no DO loop is active made it worse (0.54): audio code runs inside DO loops.
  Kept: the per-frame APU debug bookkeeping (reset of 256 voice records and an ~18 KB struct copy
  1500 times a second, ~4% of the thread's busy samples) now runs only when voice isolation is
  on, its only reader in this port. A real DSP reduction needs predecoding or a JIT; APU
  utilization is ~0.58 at full real-time rate here, so it is not limiting.

## Open issues

* Vulkan: builds and runs here now (see above); ~40 fps in Warship after the query fix, limited
  by blocking submits. Conservative depth and background compiles are OpenGL-only.
* Next ideas: none pending at scale 3 on this machine (see the measured-not-changed section);
  a DSP predecode/JIT and fewer `gl_FragDepth` writes would be larger projects.

* One first run (cold shader cache) had ~510 ms flip gaps once per second for ~70 s while the
  APU's 5 s status line stopped (it is not printed while the APU is held). Not reproduced in a
  second cold-cache run. The APU "held" log is now rate-limited instead of capped at 40 lines,
  so the next occurrence is visible.
