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

Initial Yama Hall verification is underway, with scene `se06.scn` confirmed and
combat currently around 50 fps; final window results and other maps are pending.
The test also inspects the pause/return flow to add a map-transition session.
25 Python tests and the native cache-admission/coverage test pass. Runtime scope
will be recorded explicitly; untested maps must not be reported as validated.
