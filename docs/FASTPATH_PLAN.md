# Fast NV2A path — plan (2026-11-14)

Goal: high frame rates in heavy maps on every machine the user plays on, without changing what
is rendered. The game's own D3D8 library keeps running; we rewrite the hot path below it.
D3D8 HLE is revisited only after this work is measured.

## Target machines and baselines

| Machine | Baseline (before this work) | Notes |
|---|---|---|
| RTX 2070 (dev box) | Buddha's Cave 42-80 fps uncapped, menus ~430 | only machine testable here |
| GTX 1060 + i5-7500 | Buddha's Cave below 60 | 4 cores: no spare core for spinning threads |
| Steam Deck, SteamOS (Proton) | 40+ fps at scale 1, 20+ at scale 2 | fps halves with scale -> GPU-side cost matters |
| Steam Deck, Windows 11 | lower than SteamOS | |
| Intel integrated (HD 530 class) | 20-30 fps at scale 1, Great Wall | slow GL driver per call, weak fill rate |

Acceptance on the dev box (Buddha's Cave busy phase, `--hz 1000 --novsync`, scale 3):
GPU thread cost <= 5 us per draw all-in, >= 250 fps, no visual difference from the old path.
For the other machines we ship a self-reporting benchmark (WP-E) and the user sends logs.

## Measurements that drive the plan (RTX 2070, Buddha's Cave, 700-1000 draws/frame)

Per draw, GPU thread, today (~22 us total):

| Phase | us/draw | Where |
|---|---|---|
| push-buffer decode | ~5.5 | pfifo.c pusher/puller loop, pgraph.c pgraph_method |
| textures | ~4.4 | gl/texture.c pgraph_gl_bind_textures |
| GL draw submit | ~3.3 | gl/draw.c pgraph_gl_flush_draw |
| shaders + uniforms | ~2.6 | gl/shaders.c pgraph_gl_bind_shaders |
| surface update | ~2.5 | gl/surface.c pgraph_gl_surface_update (2 calls/draw) |
| vertex attributes | ~2.2 | gl/vertex.c |
| fixed state (~40 GL calls) | rest of draw_begin | gl/draw.c pgraph_gl_draw_begin |

`XB_NULL_DRAW=1` (skips all GL work): 165-250 fps, GPU thread still ~90% busy -> decode alone
saturates the thread. Menus cap at ~440 fps with ~11 draws: ~2.3 ms fixed cost per frame.
Per frame: ~28 surface-to-texture copies (SURF_TO_TEX), 60 occlusion queries.

## Rules for every work package

1. Rendering must stay identical. Each optimisation has an env kill switch
   (`XB_NO_FAST_<AREA>=1` restores the old code path) so regressions can be bisected on the
   user's machines.
2. No reliance on NVIDIA's threaded driver. Count GL calls per draw; fewer is the goal
   (Intel's driver is slow per call, Mesa is fine).
3. No new page-fault or GetWriteWatch work on per-draw paths (slow under Proton, and
   GetWriteWatch is slow in WOW64).
4. No spinning threads (4-core CPUs).
5. Bump the shader cache key (`xemu_version` in compat/qemu_win32.c) if generated GLSL changes.
6. GL 4.6 core is the baseline; anything optional needs a fallback.

## Work packages (one agent each, own git worktree, disjoint files)

- **WP-A decode** — `src/nv2a/pfifo.c`, `src/nv2a/pgraph/pgraph.c`. Lean pusher loop (no
  per-word register SET_MASK/GET_MASK, locks taken once per batch), direct table dispatch for
  class 0x97 without the per-call context copy, bulk handlers for hot methods
  (SET_TRANSFORM_CONSTANT, ARRAY_ELEMENT16/32, INLINE_ARRAY, vertex array format/offset).
  Target <= 0.7 us/draw decode with `XB_NULL_DRAW=1`.
- **WP-B draw state + geometry** — `gl/draw.c`, `gl/vertex.c`. Shadowed GL state (only changed
  state is sent), attribute setup cached per format signature, inline arrays/elements through
  one persistent streaming buffer instead of glBufferData + hash + 50k-entry LRU.
- **WP-C textures + surfaces** — `gl/texture.c`, `gl/surface.c`. Early-out when nothing bound
  changed, sampler parameter churn removed, surface_update fast path, and removal of the
  full-surface copies behind SURF_TO_TEX (GPU cost that scales with resolution: the Deck and
  Intel case).
- **WP-D shaders + uniforms** — `gl/shaders.c`, `glsl/*`. Dirty check cost, uniform uploads
  only for changed values, no per-draw hashing when state is unchanged; review generated GLSL
  cost for weak GPUs.
- **WP-E benchmark, telemetry, frame overhead** — `nv2a_host.c`, `video/present.cpp`,
  `main.cpp`, `gl/display.c`. `--bench` mode with a one-file report (CPU ms and GPU ms per
  frame via timer queries, draws, per-phase us/draw, GL vendor/renderer), the ~2.3 ms fixed
  per-frame cost, presenter cost at high scale, sensible default scale on weak GPUs.

## Process

1. Coding agents work in parallel; lead reviews each branch and merges into `fastpath`.
2. Test agents: visual A/B (fast path vs kill switches) across menus, Great Wall, Buddha's
   Cave, movies; performance runs one at a time (concurrent runs skew timings).
3. Iterate on findings until acceptance is met, then update PROGRESS.md and hand the user a
   build + benchmark instructions for the other three machines.

## How to run

    TC=/e/IronPhoenix/port/toolchain/llvm-mingw-20261006-ucrt-x86_64/bin ./build.sh
    cd build && timeout 185 ./ironphoenix.exe --game E:/IronPhoenix/port/extracted --save ./save \
        --hz 1000 --novsync --input "$(cat ../tools/inputs/buddha_cave.txt)"

Log: `build/ironphoenix.log` — "perf:" (fps, draws as BEGIN_ENDS) and "gpu time:" (ms/calls per
phase) lines per 120 vblanks. Menus from ~60 s, match from ~110 s after start.
