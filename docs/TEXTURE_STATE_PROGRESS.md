# Host texture-state ownership checkpoint (2026-11-15)

The next bounded state step caches actual OpenGL texture-stage bindings and
per-object anisotropy. It does not replace the SDK general-state helper or
implement complete native D3D8 draw snapshots. Texture RAM dirtiness, palette
checks, alias downloads, uploads and ordered render-target snapshots still run.

## Evidence and implementation

The stable multi-map binary was profiled in Castle Entrance with scale 3 / 60 Hz.
`scratch/perf/state-next-castle-profile/` measured 39 / 57.00 / 62 FPS over
120--184 seconds. CPU sampling ran separately over 190--205 seconds. The
graphics worker had 82/273 instruction observations returning through
`GetExitCodeThread`, 33 in `memcmp`, 11 in `nv2a_dirty_sync_range`, and 5 in named
or unnamed NVIDIA driver code. This is sampled instruction location data, not
reliable full call-stack attribution. GPU draw-span samples averaged 8.221 us
over 58,262 draws, including dependencies/idle gaps; they are not full-frame GPU
times. The earlier 18 FPS Castle sample did not recur at that magnitude in this
different warm encounter. No general Castle performance fix is established.

Audit found unconditional texture clears on every disabled stage and unconditional
anisotropy writes on every enabled stage. The new four-stage/four-target snapshot
only sends changed bindings. Disabled stages need not change the active stage
when their bindings already match. Anisotropy is cached per texture object,
including shared objects across stages, preserving the existing order semantics.

Creation, deletion/name recycling, surface allocation and surface-to-texture
passes invalidate cached binding assumptions. Upload/download scratch operations
save and restore the actual active-stage 2D binding. Display/pvideo rendering uses
a separate GL context and distinct surface/display objects. Context changes do
not share this binding snapshot. Surface-copy code also invalidates its cached
min-filter value after temporarily selecting linear filtering, so guest filtering
is restored by the usual parameter application.

The optimization is enabled by default on OpenGL. Set
`XB_NO_FAST_TEXTURE_STATE=1` (runner: `--no-fast-texture-state`) to restore the
previous guest binding/anisotropy calls. `XB_TEXTURE_STATE_VERIFY=1` (runner:
`--texture-state-verify`) samples actual GL active stage/target bindings and
anisotropy. Verification aborts on mismatch; it is separate from normal timing.
Counters distinguish calls, skipped requests, invalidations and GPU state checks.
They count guest state operations, not every GL operation in surface/display code.

## Validation in progress

Both Vulkan-enabled and isolated GL-only builds pass. All four native tests pass,
including binding/name-reuse scenarios. All 30 Python tests pass, including
64-bit counter parsing and invalid verification combinations.

Castle Entrance passed `texture-state-castle-verify/`: 241,290 sampled GL state
checks, 1,353,487 guest binding calls, 45,905,164 skipped binding requests,
258 anisotropy calls, and 6,177,355 skipped anisotropy writes. Its 115--154 second
window recorded 59 / 59.47 / 60 FPS. Verification overhead and different fights
prevent interpreting that as a controlled improvement over the earlier profile.

The candidate is preserved at `scratch/perf/texture-state-prototype.exe`, SHA256
`4d5a013d6495e022780e204430d9249529979aeaeefe367d576089bcb5741cea`.
The current build has the same hash and contains both renderer backends.
The 11-map continuous texture-state verifier is now running. A separate GL-only
compile overlapped part of its first verification window; that window must not
be used for performance comparisons. No compile overlaps the later normal-mode
or same-executable reference measurements. Shader generation is unchanged;
no shader-cache key change is needed. Other target GPUs remain untested.
