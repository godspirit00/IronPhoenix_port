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

Tried, no measurable effect: `XB_REPORT_SPIN=1` (renderer re-polls occlusion results
without a 1 ms timed wait): 59.64 vs 59.23 average, within run-to-run variation.

## Benchmark pitfalls on this machine

* Screenshots (`--shots`) stall the game ~0.5-1 s each at scale 3. Most hitches over 200 ms
  in the first matrix followed a screenshot. `perf_maps.py --shots N` now sets the interval.
* The Yama Hall route did not reach `se06` in the first ordinary-settings matrix.
* First visits to a map compile shaders synchronously on the renderer (30-330 ms each).

## Open issues

* One first run (cold shader cache) had ~510 ms flip gaps once per second for ~70 s while the
  APU's 5 s status line stopped (it is not printed while the APU is held). Not reproduced in a
  second cold-cache run. The APU "held" log is now rate-limited instead of capped at 40 lines,
  so the next occurrence is visible.
