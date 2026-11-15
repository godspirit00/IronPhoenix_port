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
comparison peaks were 5,296,336 and 9,445,044 bytes/frame respectively. Total
uploads were about 40 MB / 29 MB for these full runs. These encounters are not
deterministic before/after comparisons and do not prove firm 60.
Artifacts: `scratch/perf/multimap-{yama,summoning}-verify/`.

Candidate executable SHA256:
`886538eaf9df0b60aa3331e7fa2f167679c182d8461e461df4616a70351eb4e8`.
Both Vulkan-enabled and isolated GL-only builds pass. All three native tests
pass, including cache admission. `tools/map_journey.py` generates a continuous
11-map deathmatch session, with per-map scene/time windows and combat inputs
separated from quit confirmation. It is now running in normal buffers mode to
exercise cross-map cache lifetimes. Remaining mode-specific arenas require
separate routes. Runtime scope will be recorded explicitly; untested maps must
not be reported as validated.
