# High-level D3D8 prototype — 2026-11-15

User authorized continuing with the bounded high-level rendering recommendation.

## First bridge

`src/hle/d3d8_up.cpp` replaces this game's XDK 5849 `DrawVerticesUP` entry
point (0x22bec0), gated by `XB_HLE_D3D8_UP=1`. Original routine remains callable
through a prologue-checked trampoline. Unsupported layouts and queue exhaustion
fall back to that routine. No hook is installed when the option is off.

The wrapper retains original deferred state/declaration preparation, copies the
active vertex attributes into an owned host packet, and inserts a two-DWORD
ordering marker into the existing stream. The graphics worker consumes the
packet at that exact position and invokes the common draw backend directly.
Vertex payload bypasses guest pushbuffer generation, DMA reading and per-word
method dispatch. Caller memory may be changed immediately after return.

This is a **hybrid draw-submission bridge**, not a complete high-level renderer.
Render states, shaders, resource lifetimes, other draw paths and display still
use the existing frontend. The ordering marker still uses the guest queue.
There is no claim of removing GPU emulation or achieving firm 60 fps yet.
This step tests a real D3D API interception boundary before building a complete
state/resource frontend. Do not call it a finished menu renderer.

Pending packets were initially bounded to 128; the indexed continuation expands
the shared descriptor limit to 2,048 (hundreds of indexed draws occur per frame).
The shared payload budget stays at 16 MiB, with individual payloads limited
to 65,536 DWORDs. The stream contains opaque live tokens, not host pointers.
Ordinary guest NOP/interrupt semantics remain in place. Renderer/context access
stays on the existing graphics worker; no per-draw synchronous CPU barrier is
added. Original UP declaration/flag mutations and optional KickOff are retained.

## Conformance mode

`XB_HLE_D3D8_UP=verify` packs the expected vertices but draws through the original
routine. Ordered begin/end markers compare every emitted INLINE_ARRAY DWORD
against the host packet, including copies split across guest command chunks.
Length/data differences fail loudly. This verifies packing, not the entire image
or state translation; direct host packet rendering still requires gameplay and
visual validation.

The benchmark runner exposes `--hle-up off|verify|1` and records the choice.
It clears inherited HLE configuration for baseline runs. Logs record cumulative
calls, submissions, consumptions, fallbacks and verified packets.

## Validation checkpoint

- Vulkan-enabled build links, with OpenGL still selectable/default.
- Native packing tests pass (`tools/test_hle_up_layout.cpp`), covering contiguous
  attributes, skipped attributes, source stride/offset, invalid declaration size,
  bounds and rejection without modifying output.
- Original-packing conformance:
  first attempt `scratch/perf/hle-up-verify/` exposed non-atomic verification
  marker reservation under queue pressure. Both markers now reserve together;
  capacity exhaustion falls back before any marker is emitted.
- Fixed full 185-second conformance run completed without mismatches:
  `scratch/perf/hle-up-verify-fixed/`, 363,396 verified submissions, 78,003
  fallbacks, and 14 submitted verification scopes still pending at termination.
  Counters are cumulative over the run; conformance FPS includes extra work
  and is not used as a performance baseline.
- Private helper entry points are signature-checked. Recording/capture flags
  conservatively fall back so host tokens are not saved into replayable buffers.
- `run-hle-up.cmd` launches the opt-in direct bridge on OpenGL in this checkout.
- Direct-packet gameplay run completed (`scratch/perf/hle-up-direct/`): 390,251
  packets submitted and consumed, one fallback, no reported runtime validation
  failures. Menus and Buddha Cave gameplay were visually inspected. At scale 3,
  60 Hz, timers enabled and shots every 10 seconds, the 120–184 second window
  recorded 51 / 59.16 / 60 FPS min/mean/max. This is not proof of a speedup:
  the fight/camera is not deterministic.
- Same-build disabled reference completed (`scratch/perf/hle-up-off-reference/`):
  36 / 57.47 / 60 FPS, with no active HLE mode in the log. Both runs use the
  identical executable/script hashes and settings. Direct processed 1,901,836
  draw callbacks versus 2,324,537 for the reference in the window. Its pusher
  wall time was 40.799 s versus 45.905 s, but the lower draw workload means this
  does not establish an overall performance improvement. Firm 60 fps is unmet.
- Exact direct-run executable saved as `scratch/perf/hle-up-prototype.exe`, SHA256
  `4ef81dcfcd486a2c639a9f15cb541e8708be59acead43859d50417038374998f`.
- Benchmark reports verify the requested HLE mode was actually exercised and
  include cumulative coverage counters, independent of the FPS window.
- 16 Python tooling tests and the native packing test pass.
- Vulkan 45-second startup/menu smoke passed with the direct bridge active:
  `scratch/perf/hle-up-vulkan-smoke/`, 18,211 packets consumed, no fallbacks or
  reported runtime failures. The captured menu was visually inspected. Vulkan
  combat with this bridge and other GPUs remain unvalidated.
- Ordinary GL-only compilation also passed. The exact validated Vulkan-enabled
  executable was restored from `scratch/perf/hle-up-prototype.exe` afterward:
  both renderer backends are available and the HLE bridge remains opt-in.
- Benchmark/validation game processes have ended. Existing `.claude/` worktrees
  were not changed.

## Next milestone

Audit dynamic DrawIndexedVertices/RunPushBuffer coverage before selecting the
next high-level boundary. Most indexed geometry remains on the original path;
optimizing only UP cannot remove its cost. A complete high-level frontend needs
state/resource ownership and deferred draw snapshots without guest command
markers. Retain byte/ordering conformance and measured workload controls while
expanding coverage. This checkpoint proves one interception/submission path,
not the full high-level architecture proposed in the investigation.

Reproduce:

```
VULKAN=1 ./build.sh
python tools/perf_run.py --tag hle-up-verify --renderer opengl --hle-up verify --hz 60 --scale 3 --shots 10
python tools/perf_run.py --tag hle-up-direct --renderer opengl --hle-up 1 --hz 60 --scale 3 --shots 10
```

Do not make the prototype the ordinary-play default without this validation.
