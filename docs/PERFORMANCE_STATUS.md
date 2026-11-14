# Performance work checkpoint — 2026-11-14

## Repository audit

The second pass (after user retesting) is tracked in
[VULKAN_ASSESSMENT.md](VULKAN_ASSESSMENT.md). It adds vertex-state caching,
per-draw buffer-bind hoisting, and optional asynchronous GPU timestamps. The
first-pass measurements below are historical, not measurements of that new build.
Normal play now leaves detailed CPU phase timing off (`XB_CPU_TIMING=1` enables
it). The benchmark runner explicitly enables it unless `--no-cpu-timing` is used.
Second-pass build/VAO/pixel validation passed; normal scale-3 60 Hz cave test still
dropped below 60 fps (42.12 mean sample FPS). A temporary per-application NVIDIA
threading experiment did not establish a benefit and its original profile state
was restored. See the linked assessment for final artifacts, hash, and next work.

- Branch `fastpath`, starting commit `9fb8cc7`. No tracked local changes at the start.
- Native 32-bit Windows runtime; guest CPU code runs directly. NV2A commands are
  decoded by PFIFO/PGRAPH and rendered through OpenGL. Audio uses an interpreted
  DSP. SteamOS support currently means running the Windows executable with Proton.
- Read `README.md`, `PROGRESS.md`, `docs/FASTPATH_PLAN.md`, build script, input
  automation, and renderer initialization/draw/surface/display code.
- Existing optimizations cover write-watch, lazy surface downloads, alias parking,
  asynchronous shader warmup, MMIO patching, and interrupt stalls. Do not undo these.
- Existing profiling suggests per-draw CPU overhead; old cross-machine observations
  also suggest GPU costs on Deck/Intel. These are leads, not new measurements.
- Five `.claude/worktrees` exist. Leave them intact. One has an uncommitted shader
  instrumentation change; none has an optimization commit beyond the starting point.
- Local GPU: RTX 2070, driver `32.0.16.1742`. Other target hardware is unavailable.
- Build uses `C:/Program Files/Git/bin/bash.exe build.sh` and bundled llvm-mingw.
  Full build succeeds with existing warnings (volatile operations, stack sampling,
  and an uninitialized debug stack marker).

## Current experiment

Cache individual fixed-function GL states in the render context. Clear and
surface-to-texture drawing invalidate the cache; display rendering uses a separate
context. `XB_NO_FAST_STATE=1` restores unconditional calls. No generated GLSL changes.

Initial build passed. The isolated cave run completed through busy combat with
`XB_GL_DEBUG=1`, without a GL assertion failure. Reviewed screenshots of the
title/menu, weapon selection, cave combat, HUD, and effects. Timing comparisons
are still pending; debug-run FPS is not a performance result.
Preserved executable: `scratch/perf/ironphoenix-baseline.exe`; preserved older log:
`scratch/perf/previous-session.log`. These are ignored local artifacts.

## Benchmark pitfalls discovered

- `tools/inputs/buddha_cave.txt` uses absolute wall-clock timings. The first new
  uncapped run navigated to `HalReturnToFirmware` at about 69 seconds, rather than
  entering a match. Saved as `scratch/perf/baseline-navigation-failed.log`. Do not
  report its menu frame rates as gameplay results.
- Input scripts currently silently truncate after 64 presses, shorter than the
  checked-in cave script. Fixed using dynamic storage.
- Physical input also affected menu selection during automated runs. Added
  `--input-only` alongside `--input` to isolate scripted player-one input.
  The isolated run reached the correct cave and combat; ordinary controls keep
  their existing behavior when this option is absent.
- Existing `gpu time:` values are CPU wall time on the rendering thread, including
  driver calls/waits, not GPU execution timing. Phases nest; do not sum them.
- Telemetry interval is 120 emulated vblanks, not fixed two seconds. At `--hz 1000`
  the log is emitted about every 120 ms. FPS values also repeat between updates.
- `build.sh` can hide linker errors through `|| true` and accept a stale executable.
  Fixed: linker output is saved to `build/link.log`, and a failing link exits
  nonzero. Validated using a deliberately failing compiler wrapper: exit 1,
  existing executable preserved, rather than falsely reporting success.

## Validation tools and artifacts

- `python tools/perf_run.py --tag NAME [--no-fast-state] [--no-fast-uniforms]`
  runs one isolated 185-second cave test; logs/reports/screenshots go to
  `scratch/perf/NAME`. Do not run concurrent game instances. Default refresh 120,
  scale 3. Add `--shots 0` for timing comparisons, `--gl-debug` for validation.
- `python tools/perf_report.py LOG --start 120 --end 184` reports a chosen window.
  It warns about truncated runs, firmware exits, and missing rendered frames.
  Scene identification still requires screenshots. Four parser tests pass:
  `python -m unittest discover -s tools -p test_perf_report.py`.
- `scratch/perf/route-isolated/` contains the validated cave route and debug-run
  screenshots/report. `state-baseline/` is the original fixed-state path timing run.
- `scratch/perf/ironphoenix-state-only.exe` preserves the first experiment binary.

## Uniform experiment (in progress)

The original code uploads every active uniform on every draw. Compare uniform
groups against values last uploaded to each GL program; lazily allocate storage
only for programs used in draws. Initialize/reset on program creation or binary
load, free on eviction/shutdown. `XB_NO_FAST_UNIFORMS=1` restores unconditional
uploads. This changes host submission only, so no shader cache version bump.
Initialize inactive uniform array lanes before comparing; their old stack values
were unspecified. Missing texture bindings use scale 1.

Important exception: `ui_unsquash_full_width` writes `uiSquash` after the ordinary
uniform update. Route this write through `pgraph_gl_override_ui_squash`, which
also records the actual program value. Otherwise later UI draws can retain the
full-width override incorrectly. Build passed. Combined path reached cave combat
with GL assertions enabled; the first combined timing run is saved separately.

## First measured comparison (provisional)

RTX 2070, scale 3, 120 Hz, vsync off, exclusive cave script, screenshots/debug off,
elapsed window 120–184 seconds:

| Path | FPS sample min / mean / max | draw_begin CPU us/call |
|---|---|---|
| Original state and uniform submission | 14 / 40.17 / 74 | 15.489 |
| Cached state and uniforms | 31 / 57.48 / 93 | 12.526 |
| Final build, including snapshot copies | 26 / 41.31 / 62 | 13.730 |
| Same final executable, all three optimizations disabled | 15 / 39.66 / 77 | 14.235 |

Artifacts: `scratch/perf/state-baseline/` and `state-uniforms-fast/`. These are
single evolving combat runs, not deterministic replays. The number of draws per
frame differs with combat/camera state, so do not describe the FPS ratio as a
proven repeatable speedup. Final runs are saved in `final-fast-scale3/` and
`final-original-scale3/`, with identical executable/script hashes. The final
comparison is only about 4% different in mean FPS, within a variable combat
workload; do not lead with the earlier 57.48 fps result or claim a large speedup.
Uniform CPU time was 3.104 us/call enabled vs 2.764 disabled on this NVIDIA driver,
so caching uniforms is not established as a win here and needs isolation testing.
The strict 60 fps minimum is not met yet. Repeat runs
and per-frame GPU timings are still needed. No other target hardware tested.

## Snapshot copy experiment

Avoid reallocating unchanged destination texture storage. For matching normalized
RGB/RGBA formats and compatible dimensions, use `glCopyImageSubData` instead of
the old fullscreen shader pass. This keeps separate snapshot storage, preserving
feedback/post-effect semantics; format conversions and unsupported GL functions
use the original shader path. `XB_NO_FAST_COPY=1` restores original allocation
and shader behavior. The slow fallback invalidates cached allocation metadata.

`XB_GL_DEBUG=1` additionally compares the first 32 image-copy results with the old
shader path using byte-for-byte RGBA readbacks. This is deliberately expensive
and excluded from timing runs. Build passed. All 32 comparisons in
`scratch/perf/copies-conformance/` matched byte for byte (RGBA8 / RGB565 snapshots,
including 1920x1440 and 960x720 scaled targets). Final no-debug timing and the
same-executable fallback comparison both completed without reported failures.

## Saved state

- Implementation checkpoint: `a8de42f`; subsequent checkpoint records final results.
- Built executable: `build/ironphoenix.exe`, SHA256
  `2e04823258034f368923e1801bd18e35a3985e4919c644e9f9d591cb2514b081`.
- No shader generator changes; existing binary shader cache remains compatible.
- Existing `.claude/` worktrees remain untouched and untracked.
- All benchmark processes were ended after their measurement windows.
- This is a validated first optimization pass, not completion of the overall
  high-FPS goal. Cross-machine testing and further profiling remain required.

## Next steps

1. Capture deterministic scene workloads and per-frame CPU/GPU timings before
   making stronger FPS claims. Isolate uniform-cache cost before further tuning.
2. Profile submission/driver waits and command decoding. First-pass changes do
   not establish a firm 60 fps, and the busiest combat remains below that target.
3. Continue decode/texture/vertex/shader work based on measurements. Do not claim
   the plan's 250 fps target has been achieved.
4. Collect logs on GTX 1060, Steam Deck/Proton, and the specific Intel GPU before
   claiming a cross-machine improvement or choosing new resolution defaults.
