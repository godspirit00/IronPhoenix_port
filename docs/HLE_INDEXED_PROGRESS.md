# Indexed high-level frontend continuation — 2026-11-15

The next boundary is selected from runtime coverage, not the static call list.
`XB_HLE_D3D8_INDEXED=observe` installs signature-checked pass-through hooks on
this build's DrawIndexedVertices (0x22c210) and RunPushBuffer (0x232cf0).
Their original routines still execute, including return values and side effects.
Logs count direct indexed API calls, submitted indices and reusable-buffer calls.
The benchmark runner exposes `--hle-indexed observe`, records the option and
clears inherited indexed configuration on baseline runs.

Audit completed: `scratch/perf/indexed-coverage-audit/`, OpenGL, scale 3,
60 Hz, 185 seconds, the existing Buddha Cave route. No rendering bypass is
enabled by observe mode. The standalone UP bridge remains independently opt-in.
The final cumulative counters report 2,894,942 indexed calls, 1,290,652,813
indices and zero RunPushBuffer calls. This establishes useful direct-API
coverage for this route, not absence of pushbuffer use on every map.

## Indexed bridge implementation

`XB_HLE_D3D8_INDEXED=verify|1` adds indexed packets to the shared owned-payload
queue. The XDK SetStateVB helper still commits deferred state, stream offsets
and base-vertex setup before the ordering marker. Direct mode copies unsigned
16-bit indices into a host snapshot, then the graphics worker supplies the
complete index sequence directly to the existing draw backend. No index
payload is written to the guest pushbuffer or decoded one DWORD at a time.
Other state/resource handling still uses the existing frontend: this remains
a hybrid bridge, not a complete high-level renderer.

Verification mode renders through the original routine, comparing every
ARRAY_ELEMENT16 pair and ARRAY_ELEMENT32 odd tail against the snapshot.
Verification is checked once per command packet so disabled conformance does
not add a new branch to each iteration of the legacy per-index loop. Original
UP verification is retained, and both APIs share ordered verification scopes.

Recording/capture flags, invalid/empty/oversized inputs and queue exhaustion
fall back to the original call. Helper signatures are checked, original draw
flags/optional KickOff are retained. The shared descriptor cap grows to 2,048
for the measured hundreds of indexed draws per frame; the pending payload
budget stays at 16 MiB. Individual index snapshots are capped at 65,536 indices.

Native packing tests pass for both APIs, including unsigned high values, odd
counts, unaligned index sources, immutable snapshots and chunk-sized sequences.
Vulkan-enabled build links. Combined UP/indexed conformance completed:
`scratch/perf/indexed-packing-verify/`, 3,217,487 indexed draws covering
1,603,508,871 indices and 497,989 UP submissions matched. No fallbacks or
reported runtime failures. At termination 259 indexed scopes and 80 UP scopes
were still pending; unconsumed work is not counted as verified.

The direct path completed in `scratch/perf/indexed-direct-gl/`.
`run-hle.cmd` launches both direct bridges on OpenGL; `run-hle-up.cmd` explicitly
disables the indexed bridge to retain a separate UP-only experiment.
The exact test binary is saved as `scratch/perf/hle-indexed-prototype.exe`, SHA256
`6c478123a61f983a0672b467e1d24481aa6e60217303eb641f41701da35d64bf`.
18 Python reporting/runner tests and the native packing test pass.

## Gameplay results

All measurements below use scale 3, 60 Hz, CPU phase timers enabled, screenshots
every ten seconds and the 120–184 second window. UP stays direct in both GL
runs. The executable and script hashes match. FPS counters are periodic samples,
not full frame-time distributions; fights/camera/materials differ.

| OpenGL indexed mode | FPS min / mean / max | Draw callbacks | Pusher CPU wall us/callback |
|---|---:|---:|---:|
| Direct host packets | 46 / 57.22 / 60 | 2,436,458 | 18.776 |
| Original routine with audit hook | 41 / 52.53 / 60 | 1,875,153 | 30.564 |

Direct consumed 3,168,994 indexed packets containing 1,340,113,190 indices,
plus 488,051 UP packets; no fallbacks or reported runtime failures. Screenshots
of actual Buddha Cave combat were inspected. Different work/materials mean the
per-callback timing and FPS differences cannot be attributed solely to this
change or presented as a repeatable speedup. Timers nest and must not be added.
Firm 60 fps is still unmet. Reference artifacts: `indexed-observe-reference/`.

A full Vulkan gameplay run also completed (`indexed-direct-vulkan/`): 1,732,721
indexed packets / 718,571,174 indices plus 328,089 UP packets consumed, with no
fallbacks or reported runtime failures. Captured character/combat geometry was
inspected. FPS min/mean/max was 10 / 38.19 / 62 in that different encounter;
do not infer a controlled GL/Vulkan ratio. Vulkan still has expensive surface
download/upload and synchronous submission paths. OpenGL is the launcher default.

GL-only compilation also passed. The exact tested Vulkan-enabled binary is
restored afterward so both backends remain selectable. This is an opt-in
prototype; other maps, long-duration play and other GPUs remain unvalidated.

## Next performance boundary

Direct indexed API coverage is now demonstrated on this route. The remaining
frontend still generates/decodes state commands, tracks guest RAM dirtiness and
binds shader/vertex/texture state through PGRAPH on every draw. Next, audit
resource registration, locks and stream/texture/target bindings and introduce
resource identities/versioned state snapshots. Preserve aliasing, CPU access,
deferred state, callbacks and visibility-query ordering. A full high-level
frontend must eventually submit those snapshots without the guest marker queue;
the current bridge deliberately keeps that ordering reference intact.
