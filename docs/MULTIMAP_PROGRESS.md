# Multi-map regression checkpoint (2026-11-15)

Previous runtime validation covered Buddha's Cave only. The user's 1 fps report
on Yama Hall and Summoning Altar exposed inadequate map and transition coverage.
The broader high-FPS goal applies to all maps; one-map validation is insufficient.

## Captured regression and fix

The user's last runtime log is preserved at
`scratch/perf/user-other-map-regression/ironphoenix.log`. It records buffers mode
`1`, about 55 GB cumulative vertex uploads, 43,008 evictions, and sustained 1 fps.
The prior cache keyed each exact maximum-index range and evicted entries still
used by a map. A working set exceeding its budget could continuously recreate
large GPU buffers. This was an implementation regression, not evidence that the
RTX 2070 cannot render those maps.

Capacity buckets now share nearby range sizes. Current and previous-frame
entries cannot be evicted. A frame admits at most 4 MiB of new cache storage,
compares at most 16 MiB and performs at most 4 MiB of partial updates. Requests
that cannot fit use the legacy vertex path before changing cache content or
checked coverage. Native tests replay a 720-draw/2-MiB-miss thrashing frame and
verify budget limits, overflow rejection and frame-age wraparound. This is a
bounded hybrid fallback; it does not promise that every draw uses owned storage.

## Expanded validation

`tools/map_routes.py` generates offline routes for the 14 available arena assets,
including team-only arenas and the challenge arena. Initial menu indices require
runtime confirmation. `tools/perf_maps.py` runs routes sequentially, preserving
per-map logs, screenshots, executable hashes and a cumulative `summary.json`.
The file layer logs successfully opened scene filenames; `--expect-scene` rejects
a route that reaches the wrong map. The runner also checks cache byte-budget
telemetry. The suite's 5 fps floor detects catastrophic regressions; it is
explicitly separate from the unresolved firm-60 performance target.

Example (GPU verification on the reported maps):

```
python tools/perf_maps.py --tag reported-maps --mode verify --maps yama-hall summoning-altar
```

Both reported maps completed GPU-storage verification at scale 3 / 60 Hz,
with normal scripted combat measured over 120--184 seconds:

| Map | FPS min / mean / max | GPU checks | Evictions |
| --- | --- | --- | --- |
| Yama Hall (`se06`) | 55 / 59.16 / 60 | 11,841 | 0 |
| Summoning Altar (`sw05`) | 45 / 56.28 / 61 | 6,729 | 0 |

Neither run reported validation failures. Peak new admission was 4 MiB/frame;
comparison peaks were 5,296,336 and 9,445,044 bytes/frame respectively. Owned-cache
uploads were about 40 MB / 29 MB for these full runs. These encounters are not
deterministic before/after comparisons and do not prove firm 60.
Artifacts: `scratch/perf/multimap-{yama,summoning}-verify/`.

Candidate executable SHA256:
`886538eaf9df0b60aa3331e7fa2f167679c182d8461e461df4616a70351eb4e8`.
Both Vulkan-enabled and isolated GL-only builds pass. All three native tests
pass, including cache admission. `tools/map_journey.py` generates a continuous
11-map deathmatch session, with per-map scene/time windows and combat inputs
separated from quit confirmation. The 950-second session completed in normal
buffers mode, opening all 11 expected scenes in order and passing all per-map
windows. Captured geometry, characters, lighting and HUD were inspected.

| Continuous session map | FPS min / mean / max |
| --- | --- |
| Summoning Altar | 45 / 56.50 / 60 |
| Yama Hall | 51 / 58.07 / 60 |
| Buddha's Cave | 44 / 57.47 / 60 |
| Great Wall | 59 / 59.53 / 60 |
| Temple of Paganism | 59 / 59.47 / 60 |
| Dragon Valley | 39 / 56.73 / 61 |
| Training Square | 39 / 54.80 / 61 |
| Thousand Bone Tomb | 37 / 54.33 / 60 |
| Castle Entrance | 18 / 56.40 / 61 |
| Apparatus Maze | 54 / 59.27 / 60 |
| Electric Shock Chamber | 36 / 54.13 / 60 |

The session exercised 199,895,623 owned attribute binds, with 130,905 buffer
fallbacks. Cache totals: 214,907,340 bytes uploaded, 513 evictions, 66,105,344
resident bytes (within 64 MiB). Peak admission/comparison/partial-update work was
4,194,304 / 8,689,140 / 45,904 bytes per frame. No runtime validation failures
or budget violations were reported. Indexed packets also exercised their
existing SDK fallback (194,649 calls); not every draw bypassed the SDK.
Artifacts: `scratch/perf/multimap-continuous/`, including
`map_journey.results.json` for separate scene-verified windows.

All windows remained above the catastrophic-regression floor, but these short
encounters do **not** establish firm 60 on every map. Castle Entrance's 18 fps
sample remains a performance concern for the next profiling pass. Comparison
with the user's different failing session establishes no precise speedup ratio.
The remaining mode-specific routes also completed GPU-storage verification:

| Arena scene | FPS min / mean / max | GPU checks | Evictions |
| --- | --- | --- | --- |
| Night Demon Warship (`sw01`) | 29 / 42.95 / 61 | 5,032 | 0 |
| Training Square at night (`se07`) | 45 / 54.42 / 61 | 7,626 | 0 |
| Red Viper's Den (`in01`) | 59 / 59.42 / 60 | 2,103 | 0 |

All three opened the expected scene before their 115--154 second measurement
windows and reported no validation failures or budget violations. Screenshots
were inspected. Artifacts and summary: `scratch/perf/multimap-special/` and
`scratch/perf/multimap-special-{MAP}/`. Together with the deathmatch journey,
all 14 available arena scenes in this disc dump have been exercised. Red Viper's
Den coverage is its challenge entry/transport area with visible NPC matches;
transport into a player duel was not tested. Other game modes on shared maps,
all camera positions/effects, and long busy fights on every arena are not yet
covered. This is scene/transition regression coverage, not complete game QA or
pixel equivalence with the legacy renderer. Other GPUs remain untested.

The five GPU-verification runs performed 33,331 sampled GPU storage checks in
total. All 28 Python tests and three native tests pass. The tested executable
remains at `build/ironphoenix.exe`, also preserved as
`scratch/perf/hle-buffers-multimap.exe`, with the hash above. All benchmark
processes have ended. Existing `.claude/` worktrees were left untouched.

## Continuing performance work

Keep the multi-map matrix as an acceptance check for future renderer changes;
Buddha-only validation must not be reported as general map compatibility.
Reproduce affected-map combat, rerun the continuous transition route, and check
the mode-specific arenas when resource/state submission changes. A 5 fps floor
only detects catastrophic regressions; firm-60 work needs separate frame-time
and busy-fight measurements. Native draw-state/resource submission remains the
next performance boundary, with special attention to Castle Entrance and Night
Demon Warship. Do not remove content checks or fallback merely to improve a
one-map benchmark. Vulkan has compile coverage only in this milestone.
