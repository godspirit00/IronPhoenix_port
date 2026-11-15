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
Vulkan-enabled build links. Combined UP/indexed conformance is in progress:
`scratch/perf/indexed-packing-verify/`.

Next: finish conformance, test actual indexed packets on the same route, compare
with an observe-only run of the identical executable, and inspect gameplay.
