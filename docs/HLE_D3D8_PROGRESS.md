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

Pending packets are bounded to 128 and 16 MiB, with individual payloads limited
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
- Scripted original-packing conformance run is in progress:
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
- Next: inspect coverage and conformance, run the direct-packet path through the
  same route, preserve comparisons and assess whether UP has useful coverage.

Reproduce:

```
VULKAN=1 ./build.sh
python tools/perf_run.py --tag hle-up-verify --renderer opengl --hle-up verify --hz 60 --scale 3 --shots 10
python tools/perf_run.py --tag hle-up-direct --renderer opengl --hle-up 1 --hz 60 --scale 3 --shots 10
```

Do not make the prototype the ordinary-play default without this validation.
