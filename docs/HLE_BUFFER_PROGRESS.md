# GPU-owned vertex-range checkpoint (2026-11-15)

This experiment is opt-in and OpenGL-only. The ordinary renderer remains the
baseline. It is a bounded resource step within the hybrid frontend, not a complete
D3D8 replacement or an established speed improvement.

`XB_HLE_D3D8_BUFFERS=verify|1` requires `XB_HLE_D3D8_INDEXED=1`, resource
snapshots (`XB_HLE_D3D8_RESOURCES=verify|1`), and fast vertex state. The performance
runner rejects unsupported combinations. Vulkan retains its existing buffers.

## Implementation

Resource identity and physical page range select GPU-owned vertex storage.
Each range has an independent CPU content shadow; overlapping aliases cannot
consume each other's invalidation. Exported lock calls alone are never trusted.
Every cache hit compares the actual fetched span with its shadow and uploads
changed bytes. Owned attributes bypass the legacy whole-VRAM upload buffer and
write-watch query. Fallback attributes retain the legacy coherence path.

A non-clearing physical-page version tracker was also implemented and tested.
It is inactive in the final candidate: querying the entire owned range on every
attribute was redundant with span comparison and measurably expensive. Its
enable entry point remains available for future allocation tracking, but the
renderer does not call it. Initial page-version-only reuse is not supported.

The 512-entry cache has a 64 MiB resident budget and an 8 MiB per-range limit.
Eight hash probes select a cache entry; entries used by the current draw are
pinned. Ranges begin at the page containing the attribute base, keeping original
indices and vertex IDs. Zero-stride/unsupported layouts, missing provenance,
out-of-range requests and active/parked surface overlaps use the legacy path.
Legacy format/offset commands invalidate HLE provenance. New ranges receive
immutable CPU upload snapshots; partial GPU updates retain GL's ordered draw
semantics for previously submitted work and may incur driver synchronization.
Deletion invalidates cached VAO pointers before GL names can be reused. A CPU
shadow (also capped at 64 MiB) is retained in both modes. Every cache hit compares
the contiguous vertex span used by the current draw; changed spans receive an
ordered partial GPU update. Page versions alone do not authorize reuse.

Verification reads back the first 32 GPU uploads and samples a fetched vertex
span every 4096 attribute binds, comparing GPU storage with immutable CPU upload
snapshots. Padding and unreferenced vertices may change under NOOVERWRITE without
invalidating a draw. Live guest RAM cannot serve as an immutable verification
reference: the CPU can already be writing bytes for a later draw.
It aborts on an actual fetched-byte or upload mismatch. Verification overhead is
deliberate; use mode `1` for performance measurements after validation.

## Validation in progress

Both renderer build configurations and the scripted Buddha Cave route must pass
before recommending this experiment. Native page-version tests cover overlapping
aliases, repeated non-consuming reads, writes to earlier pages, and bounds.
Runner/report tests cover unsupported combinations and 64-bit upload counters.
Runtime results and exact executable hash will be recorded here after completion.

The first race-safe GPU-readback run (`buffers-gpu-verify/`) completed with
13,277,819 owned attribute binds and 3,274 GPU storage checks, no failures and
zero fallback attributes. Its page-query implementation cost 16.445 us per
vertex-attribute setup callback and recorded 24 / 39.72 / 60 FPS min/mean/max.
It is preserved at `scratch/perf/hle-buffers-page-verify.exe` (SHA256
`9416cb6cd2552a5a456e14783fb5ea847e110a9f5d319276d5948de5d64688af`).
This run justified removing redundant range-version polling; it does not prove
a performance gain over the existing renderer.

The initial page-version-only candidate reported a live-RAM comparison mismatch
after 503,538 attribute binds, at slot 0/index 0, before the measurement window.
Artifacts: `scratch/perf/buffers-initial-verify/` (binary SHA256
`5cf685ca85ba858e348e8b784e1b7da174d8856a857ab996c7feff67bf417d95`).
An earlier upload of a larger page range can precede later writes to newly used
vertices within the same kick. The current candidate always checks fetched
spans, even when page versions are unchanged. A second live-RAM comparison failed
even after span refresh (`buffers-span-verify/`, binary SHA256
`5225dad593a009eea9d177181c2e2d6dcc54a98c4b084b810ea5ac884136ebc9`). These
checks raced CPU writes, so neither failure alone proves a wrong GPU upload or
identifies the precise write timing. GPU readback against upload snapshots
replaces that invalid verifier. Neither early run reached the timing window.
Do not enable the initial checkpoint's buffers for ordinary play.

Remaining ownership work includes full allocation/view lifetimes, Vulkan buffer
ownership, texture/surface ownership and reducing synchronization. This cache
still polls guest RAM and does not remove all low-level emulation. Other GPUs
remain untested. Different combat runs cannot establish small speed differences.
