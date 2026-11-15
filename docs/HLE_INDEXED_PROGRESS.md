# Indexed high-level frontend continuation — 2026-11-15

The next boundary is selected from runtime coverage, not the static call list.
`XB_HLE_D3D8_INDEXED=observe` installs signature-checked pass-through hooks on
this build's DrawIndexedVertices (0x22c210) and RunPushBuffer (0x232cf0).
Their original routines still execute, including return values and side effects.
Logs count direct indexed API calls, submitted indices and reusable-buffer calls.
The benchmark runner exposes `--hle-indexed observe`, records the option and
clears inherited indexed configuration on baseline runs.

Audit in progress: `scratch/perf/indexed-coverage-audit/`, OpenGL, scale 3,
60 Hz, 185 seconds, the existing Buddha Cave route. No rendering bypass is
enabled by observe mode. The standalone UP bridge remains independently opt-in.

Next: use gameplay coverage to choose direct indexed packets or the reusable
buffer path; preserve original index ordering and base-vertex state preparation
and verify output against the guest routine before exercising a direct path.
