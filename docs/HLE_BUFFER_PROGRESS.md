# GPU-owned vertex-range checkpoint (2026-11-15)

This experiment is opt-in and OpenGL-only. The ordinary renderer remains the
baseline. It is a bounded resource step within the hybrid frontend, not a complete
D3D8 replacement or an established speed improvement.

`XB_HLE_D3D8_BUFFERS=verify|1` requires `XB_HLE_D3D8_INDEXED=1`, resource
snapshots (`XB_HLE_D3D8_RESOURCES=verify|1`), and fast vertex state. The performance
runner rejects unsupported combinations. Vulkan retains its existing buffers.

## Implementation

Resource identity and physical page range select GPU-owned vertex storage.
Non-clearing page versions invalidate overlapping aliases independently of the
legacy client dirty bits. Versions observe OS write watching, explicit RAM dirty
marks, and GPU-to-RAM write-watch discards. Existing write-watch kick-epoch
semantics still apply; exported lock calls alone are never trusted.

The 512-entry cache has a 64 MiB resident budget and an 8 MiB per-range limit.
Eight hash probes select a cache entry; entries used by the current draw are
pinned. Ranges begin at the page containing the attribute base, keeping original
indices and vertex IDs. Zero-stride/unsupported layouts, missing provenance,
out-of-range requests and active/parked surface overlaps use the legacy path.
Legacy format/offset commands invalidate HLE provenance. Buffers are refreshed
with orphaned storage, retaining bytes needed by previously submitted GPU work.
Deletion invalidates cached VAO pointers before GL names can be reused.

Verification retains CPU upload snapshots, reads back the first 32 GPU uploads,
and checks fetched vertex bytes when live range hashes change. Padding and
unreferenced vertices may change under NOOVERWRITE without invalidating a draw.
It aborts on an actual fetched-byte or upload mismatch. Verification overhead is
deliberate; use mode `1` for performance measurements after validation.

## Validation in progress

Both renderer build configurations and the scripted Buddha Cave route must pass
before recommending this experiment. Native page-version tests cover overlapping
aliases, repeated non-consuming reads, writes to earlier pages, and bounds.
Runner/report tests cover unsupported combinations and 64-bit upload counters.
Runtime results and exact executable hash will be recorded here after completion.

Remaining ownership work includes full allocation/view lifetimes, Vulkan buffer
ownership, texture/surface ownership and reducing synchronization. This cache
still polls guest RAM and does not remove all low-level emulation. Other GPUs
remain untested. Different combat runs cannot establish small speed differences.
