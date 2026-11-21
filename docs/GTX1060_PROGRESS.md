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
conservative depth.

| Arena | matrix 1 | matrix 2 | matrix 3 |
| --- | --- | --- | --- |
| great-wall | 58.0/59.71/71.1/0 | 60.0/60.0/17.8/0 | 53.0/59.24/168.6/2 |
| temple-of-paganism | 49.9/59.29/383.6/1 | 59.5/59.97/30.5/0 | 60.0/60.0/17.6/0 |
| dragon-valley | 53.7/59.7/233.7/1 | 60.0/60.0/17.6/0 | 55.5/59.76/102.7/1 |
| training-square | 51.9/59.08/333.8/3 | 58.0/59.76/23.9/0 | 59.5/59.97/39.2/0 |
| thousand-bone-tomb | 48.5/58.35/404.0/4 | 50.5/59.32/347.8/2 | 59.0/59.92/63.2/0 |
| castle-entrance | 56.5/59.82/143.3/1 | 60.0/60.0/20.6/0 | 35.1/58.71/622.9/1 |
| apparatus-maze | 56.8/59.88/111.8/1 | 58.0/59.82/97.7/0 | 59.0/59.95/39.6/0 |
| yama-hall | 60.0/60.0/29.1/0 | 60.0/60.0/21.0/0 | 59.5/59.97/34.5/0 |
| electric-shock-chamber | 54.1/59.73/220.1/1 | 49.5/59.0/615.3/2 | 60.0/60.0/20.6/0 |
| summoning-altar | 39.0/56.42/1079.5/5 | 54.0/58.87/36.7/3 | 60.0/60.0/17.7/0 |
| buddha-cave | 50.8/59.42/363.0/2 | 54.5/58.68/110.0/5 | 57.5/59.82/36.4/1 |
| night-demon-warship | 56.2/59.58/21.3/2 | 50.0/57.18/27.7/9 | 60.0/60.0/22.2/0 |
| training-square-night | 54.0/59.68/216.6/1 | 60.0/60.0/24.8/0 | 56.5/59.82/148.6/1 |
| red-vipers-den | 55.0/59.76/183.7/1 | 60.0/60.0/17.6/0 | 60.0/60.0/17.7/0 |

Sustained-load arenas improved most (Warship 57.2 -> 60.0, Summoning Altar 58.9 -> 60.0).
Matrix 3's Great Wall and Training Square Night dips coincide with first-time shader compiles
(the conservative-depth variants are new cache entries; the cache is now warm). Gameplay is
not deterministic between runs, so single dips are not comparisons.

## Open issues

* Castle Entrance matrix 3: two flip gaps (622 ms, 390 ms) late in the window without a
  shader compile or screenshot nearby. Not investigated yet.
* Vulkan build not compiled on this machine (toolchain not prepared); the conservative-depth
  generator change is guarded to OpenGL.
* Next ideas: per-pixel depth math is still ~20% of depth-tested draw time (variant 2 vs
  hardware depth); shader compiles are synchronous on the renderer (30-330 ms each on first
  visit); DSP interpreter still ~0.45 of a core.

* One first run (cold shader cache) had ~510 ms flip gaps once per second for ~70 s while the
  APU's 5 s status line stopped (it is not printed while the APU is held). Not reproduced in a
  second cold-cache run. The APU "held" log is now rate-limited instead of capped at 40 lines,
  so the next occurrence is visible.
