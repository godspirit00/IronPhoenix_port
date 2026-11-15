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

## Validation and measurements

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
The 950-second continuous verifier completed all 11 deathmatch maps and ten
transitions, with expected scene IDs active before each window, no reported
mismatches and no catastrophic regression. It recorded 1,465,676 sampled GL state
checks, 13,812,381 guest binding calls, 275,205,415 skipped binding requests,
1,410 anisotropy calls and 43,489,628 skipped anisotropy writes. Screenshots from
every map were inspected. The owned-buffer budgets still held: peak admission
4,194,304, comparisons 9,373,636 and partial updates 34,416 bytes/frame.

Artifacts: `scratch/perf/texture-state-journey-verify/`, including
`map_journey.results.json`. These are GL state comparisons and limited visual
checks, not pixel-for-pixel equivalence with the old renderer. Red Viper's Den
retains the previous entry/transport-area coverage limitation.

The three mode-specific scenes also passed, including actual-GL verification:

| Scene | FPS min / mean / max (verification) | GL state checks |
| --- | --- | --- |
| Night Demon Warship | 40 / 50.53 / 60 | 181,540 |
| Training Square at night | 57 / 59.11 / 60 | 195,011 |
| Red Viper's Den entry area | 59 / 59.37 / 60 | 60,187 |

Artifacts: `scratch/perf/texture-state-special/` and its per-map sibling
directories. All 14 available arena scenes were opened and rendered, and the
continuous session covered ten map transitions. Captured scene geometry,
characters, lighting, effects and HUD were inspected. The five verification
runs performed 2,143,704 sampled GL state checks in total with no reported
mismatches. Resource-content validation and buffer admission fallback remained
active; there was no recurrence of the 1 fps cache-thrashing failure.

A separate GL-only
compile overlapped part of its first verification window; that window must not
be used for performance comparisons. No compile overlaps the later normal-mode
or same-executable reference measurements. Shader generation is unchanged;
no shader-cache key change is needed. Other target GPUs remain untested.

### Same-executable ordinary-play pair

Both runs below used the candidate hash above, OpenGL, scale 3, 60 Hz, screenshots
every 10 seconds and the Castle route. All HLE experiment flags, GPU timing and
verification were off. Measurement window: 120--184 seconds. The reference used
`--no-fast-texture-state`; no other setting changed. Both runs completed without
reported errors, confirming the ordinary frontend as well as the HLE path.

| Mode | FPS min / mean / max | Texture setup us/callback | Draw callbacks in window |
| --- | --- | --- | --- |
| Snapshot enabled | 59 / 59.38 / 60 | 0.681 | 3,874,506 |
| Previous binding/aniso calls | 59 / 59.44 / 60 | 0.815 | 3,884,332 |

Full-run guest state counters (including menus):

| Mode | Active-stage calls | Binding calls | Anisotropy writes |
| --- | --- | --- | --- |
| Snapshot enabled | 6,342,385 | 2,276,460 | 257 |
| Previous calls | 24,480,956 | 69,775,576 | 9,382,664 |

The snapshot recorded 68,821,832 skipped binding requests and 9,383,167 skipped
anisotropy writes. These counters exclude surface/display raw calls and are not
a count of all GL driver traffic. Different NPC encounters prevent treating the
pair as deterministic replay. The lower measured texture setup cost and fewer
driver calls are useful evidence; no uncapped FPS gain or firm-60 result across
all maps is established. Other-map verification still recorded drops, including
a 19 FPS Castle sample. Do not extrapolate this capped pair to weaker GPUs.
Artifacts: `texture-state-normal-castle/` and `texture-state-reference-castle/`.

The exact tested binary is retained in `build/ironphoenix.exe` and
`scratch/perf/texture-state-prototype.exe`. All benchmark processes have ended.
Existing `.claude/` worktrees were left untouched.

## Next native state boundary

Audit vertex-shader constant setters and the SDK general-state helper for a
bounded immutable constant-packet path. Compare register destinations and raw
words against the original ordered stream before bypassing payload generation
or NV2A method decoding, and preserve state-block/recording fallback. This is a
safer next step than removing live-resource coherence checks. The current host
binding snapshot does not eliminate the general-state or shader-constant
pipeline. Keep all-map/transition checks, and add deterministic workload capture
before claiming small end-to-end speed differences or reliable frame-time lows.
