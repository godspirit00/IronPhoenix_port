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

OpenGL scale-1 validation completed: 33 / 46.03 / 60 FPS min/mean/max. Pusher
wall time was 58.868 seconds in the 64-second measurement window; late-combat
GPU utilization averaged 47.6% over 15 samples. This fight differed enough that
scale 1 was slower than the scale-3 GL run: **do not interpret this as a negative
resolution-scaling curve**. These are distinct evolving encounters. Both native
resolution runs still missed 60 fps, with substantial CPU-side work.

The GL scale-1 run records FPS over 120–184 seconds, then samples CPU stacks
over 190–220 seconds, avoiding profiler
suspension overhead inside the FPS window. The runner now supports separate
`--end` and `--profile START,SECONDS` options, preserving the profile with that
run's artifacts. `symprof.py --profile PATH --exe PATH` can symbolize a saved
profile against its exact binary. These diagnostic tools do not replace a
future deterministic capture/replay workload.

The sample collected 662 observations per thread in 30 seconds (much less than
1 kHz due to the cost of enumerating/suspending many threads). The worker observed
in pgraph/driver calls (TID 2784) had 185 unnamed NVIDIA DLL observations plus
31 under the nearest exported `DrvPresentBuffers` symbol, 139 condition-wait
observations, 30 in `ZwGetWriteWatch` and 31 in `memcmp`. These are instruction
locations/nearest symbols, not precise call-stack attribution. The thread
observed in Xbox draw/query routines (39704) spent 532/662 samples in
`ZwWaitForMultipleObjects`; this does not identify the particular waited object.
The APU also does meaningful work, so graphics replacement does not eliminate
all non-game overhead. Naive stack scanning can mistake data for return addresses;
do not use its guessed callers (such as data address 0x2391a0) as proof.

Profile artifacts: `scratch/perf/investigate-gl-scale1/{profile.txt,symbolized-profile.txt}`.
Exact measured binary saved at `scratch/perf/investigation-renderers.exe`.
Host CPU: AMD Ryzen 5 3600; GPU: RTX 2070. Other target machines remain untested.

## Conclusion and next milestone

The reported Vulkan regression is reproduced. It retains expensive surface
alias download/upload and synchronous buffer-recycling paths that GL avoids.
Both backends also pay significant per-draw driver/state/resource tracking costs.
This does not establish an RTX 2070 hardware ceiling or predict target-device
FPS; lower-GPU performance depends on CPU, resolution, drivers and the frontend.

Keep OpenGL as the ordinary-play baseline. The concrete low-level Vulkan fixes
are resource/alias reuse and less frequent blocking submission, with safe
in-flight lifetimes; simply removing waits is incorrect. A bounded high-level
D3D8 prototype is a justified architectural experiment to bypass command-queue
and ownership tracking costs. It is not yet proven necessary or sufficient for
firm 60 fps. Require image/ordering correctness and measured combat throughput
before committing to a full migration. No high-level renderer was implemented
by this investigation, and no performance fix is claimed for the diagnostic build.

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

## Firm-60 investigation (2026-11-21)

At 60 fps the port has 16.67 ms per frame. In the most demanding sampled
OpenGL scenes, the serial PFIFO/PGRAPH graphics worker spent most of its time
in draw setup and submission. Night Demon Warship reproduced this without
surface transfers or a recurrence of the earlier owned-buffer cache thrash.

The runs below used the Ryzen 5 3600 and RTX 2070 (NVIDIA 617.42), OpenGL,
60 Hz guest vblank, `--novsync`, the four HLE switches from
`run-hle-buffers.cmd`, exact occlusion and the existing shader cache. Each used the
existing input script and checked the opened arena scene. Screenshots and
validation queries were disabled during the timed runs. Enemy behavior and
camera workload can differ between runs. The FPS counter reports a recent
quarter second every two seconds, so its minimum differs from an individual
frame time or an average over the whole run.

### Warship reproduction

`scratch/perf/firm60-warship-scale3/` used the regular build (commit `1deb0eb`,
SHA-256 `aac4c23e522aab3ef5d318e39920fc0a7142700779f76c3e01cb24464581a4e0`),
with CPU scopes and sampled GPU timestamps. At scale 3 (1920×1440 internal),
the 115–173 s window recorded 30 / 49.34 / 60 fps min/mean/max. The slower
stretch ran at roughly 30–35 fps and recovered as the draw count fell.
At 128.25 s the 30 fps reading coincided with 1,539 draws in the last
completed render frame and 89,863 draw begins in the preceding two seconds.
In that interval the worker took 1,910 ms inside the pusher, including
1,534 ms inside draw begin and end, with no surface downloads or uploads.
The GL presenter sync scope took 21 ms and the combined report processing
scope took 58 ms, even with 10,089 exact result reads. The sampled GPU spans
can include idle gaps and dependencies and should not be scaled into a
GPU frame time. Compilations during the match were occasional.

The separate trace executable records exact emulated display flip gaps,
render-ready gaps, per-frame draw counts, and the existing `glFinish` wall
time. It groups 30 entries per log line and keeps GPU timestamp queries off.
The graphics worker is identified by the thread emitting `firm60 ready`.
External samples of Windows thread CPU time exclude time asleep in a wait.
The regular `build/ironphoenix.exe` was left intact.

At scale 3 in the 109–150 s window, the trace recorded 1,639 flips (39.96
per second). The median gap was 27.36 ms, the 95th percentile was 35.53 ms
and the 99th was 40.95 ms.
997 flips took over 20 ms. The 925 render-ready intervals with 1,200–1,599
draws had a 30.26 ms median gap and only 1.40 ms average in `glFinish`.
The graphics worker used roughly 86–94% of one logical CPU across sampled
two-second combat intervals before the draw count dropped. This identifies
a CPU throughput bottleneck that includes renderer code and graphics driver
work. It cannot attribute all GPU work or all scheduling time to a single API.

The native-resolution run (scale 1, 640×480) still missed 60 fps. Compare the
same draw-count range so the scale-3 recovery as the camera view changes
does not hide the heavy-frame cost:

| Warship render-ready intervals with 1,200–1,599 draws | Count | Mean draws | Median gap | P95 gap | Mean `glFinish` |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Scale 3 | 925 | 1,430 | 30.26 ms | 37.21 ms | 1.40 ms |
| Scale 1 | 1,214 | 1,394 | 25.23 ms | 31.69 ms | 0.81 ms |

Over 99% of intervals in this draw-count range exceeded 20 ms at both scales.
The scale-1 worker used roughly 83–95% of one logical CPU in two-second
samples. Lower resolution reduced the same class of frame's cost in these
runs, but the different views and effects prevent a clean isolation of GPU
fill cost through this comparison. Prior screenshots of this route in
`texture-state-special-night-demon-warship` show that the deck view is enough
to cause the heavy workload after spawn, even before the player engages a bot.
The camera turns along the deck later in that route as the draw count falls.

The 109–135 s control (`firm60-trace-warship-no-scopes`) disabled the per-draw
CPU scopes and GPU timestamp sampling. It still measured 33.16 flips/s, with
a 29.60 ms median flip gap and only two of 862 flips at or below 20 ms. The
815 frames in the heavy draw-count range averaged 1,468 draws and had a
29.80 ms median render-ready gap. Profiler overhead cannot account for the
missed 60 fps target in this scene.

### Earlier arena results reclassified

The first slow Castle Entrance sample (`multimap-continuous`, 750.20 s,
18 fps) had 4,560 draw batches in the last render frame and 1,942 ms of
pusher work in two seconds. The screenshot at 750.234 s (`shot_0149.bmp`)
shows the pre-spawn scoreboard and the arena overview. The screenshot at
755.234 s (`shot_0150.bmp`) shows the player in the match. After spawn, the
754–779 s samples ranged from 57 to 61 fps in that run. The overview is a
real performance case, but the 18 fps reading did not measure active combat.
The fresh `firm60-trace-castle3` run reproduced it in the 94–108 s window.
159 heavy intervals averaged 4,506 draws, had a 52.05 ms median render-ready
gap, and spent 1.54 ms on average in the final `glFinish`. Another 337 frames
in that window, with 900–1,199 draws, had a 16.63 ms median gap. This confirms
that the overview's high draw count can overwhelm the same graphics worker.

The fresh Castle run also dropped to 51 fps after spawn, with a 20.93 ms
95th-percentile flip gap over 109–150 s. The 120.19 s and 136.19 s samples had
nearly the same two-second draw counts (118,805 and 117,958). Draw-end time
rose from 241 to 749 ms, while vertex binding rose from 199 to 274 ms within
that scope. The worker's CPU use rose to about 95% of one logical CPU. Draw
cost as well as draw count matters. The current scopes do not distinguish
index scanning and hashing, buffer work and driver cost inside that increase.

Other scene-verified windows in the same session show a similar load pattern.
The table pairs each quarter-second FPS reading with timings from the preceding
two-second interval. It does not establish that every sample was active combat.

| Arena | Lowest sampled fps | Last frame draws | Pusher ms / 2 s | Draw begin + end ms / 2 s |
| --- | ---: | ---: | ---: | ---: |
| Summoning Altar | 45 | 1,113 | 1,865 | 1,334 |
| Buddha's Cave | 44 | 1,028 | 1,894 | 1,380 |
| Dragon Valley | 39 | 1,768 | 1,886 | 1,450 |
| Training Square | 39 | 1,602 | 1,912 | 1,448 |
| Thousand Bone Tomb | 37 | 1,836 | 1,902 | 1,448 |
| Castle Entrance (spectator) | 18 | 4,560 | 1,942 | 1,500 |

No surface downloads were recorded in these intervals. The draw scopes are
included in the pusher scope. Vertex binding sits inside draw end, and shader,
texture and surface setup mostly sit inside draw begin. Adding nested scopes
would double-count the time. A blocking report counter records the number of
reads, regardless of how long the driver takes to return each result.

### Resume point

The HLE indexed and resource paths replace parts of the Xbox command and
vertex setup, but still run normal GL draw preparation and submission for
each packet. A backend change alone would leave that high per-draw workload.
The earlier Vulkan investigation above found additional waits and transfers on
that implementation. The new data does not establish the gains from a full
D3D8 replacement or predict FPS on the GTX 1060, Steam Deck or Intel HD 530.

Use Warship's deck view and both Castle views to reproduce the measured
costs. The next profile should split GL draw-end time into index scan/hash,
buffer updates, and driver calls, then group costs by draw or pass type.
Select a change based on that breakdown. Use the scene-verified multi-map
routes with exact flip timing as a separate acceptance check, including
spectator views, busy combat and map transitions. The existing 5 fps floor
in the map runner only catches catastrophic failures.

Raw logs and JSON reports are in `scratch/perf/firm60-*`. The trace executable
in `scratch/firm60-trace/ironphoenix.exe` has SHA-256
`b86d0630be06c00b2d34ee09e7b8c86172cd843b49f796e8bcbe7ddea861a856`.
`scratch/build_firm60_trace.py` rebuilds it from the regular Vulkan-enabled
build objects, changing only the isolated copies of the profile and GL
renderer source files. The timing records can be read with
`python scratch/firm60_analyze.py LOG START END`. The thread samples are in
`scratch/firm60-thread-samples.csv` and are summarized by
`python scratch/firm60_thread_summary.py`.

Example repeat with the regular build from the repository root:

```powershell
python tools/map_routes.py night-demon-warship --output scratch/firm60-warship.txt
python tools/perf_run.py --tag warship-repro --script scratch/firm60-warship.txt --expect-scene sw01 --hz 60 --scale 3 --seconds 174 --start 115 --shots 0 --hle-up 1 --hle-indexed 1 --hle-resources 1 --hle-buffers 1
```

The scope and flip measurements are for the emulated display pipeline.
Desktop presentation timing with normal vsync is a separate measurement.
This investigation did not change the gameplay build or driver settings.
