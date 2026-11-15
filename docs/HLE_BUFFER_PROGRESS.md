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
Overlapping interleaved attributes share checked coverage within one draw, so
each byte is compared once per cached range/draw. Extending disjoint coverage
also checks the gap; a later attribute cannot mistake unchecked holes for valid
data. A later draw resets coverage and observes its bytes again.

Verification reads back the first 32 GPU uploads and samples a fetched vertex
span every 4096 attribute binds, comparing GPU storage with immutable CPU upload
snapshots. Padding and unreferenced vertices may change under NOOVERWRITE without
invalidating a draw. Live guest RAM cannot serve as an immutable verification
reference: the CPU can already be writing bytes for a later draw.
It aborts on an actual fetched-byte or upload mismatch. Verification overhead is
deliberate; use mode `1` for performance measurements after validation.

## Validation and measurements

Both OpenGL-only and Vulkan-enabled builds pass. The GL-only compile used
`scratch/gl-only-check/` while the menus ran, finishing before the FPS window;
the tested Vulkan-enabled executable in `build/` was retained. Vulkan buffer
ownership is not implemented or runtime-tested by this milestone.
Native tests cover page-version aliases/bounds, per-draw overlapping and disjoint
coverage, resource/state preparation, and UP/index packing. All 22 Python
runner/report tests pass. Shader generation is unchanged; caches remain valid.

The final overlap candidate is preserved at
`scratch/perf/hle-buffers-coverage.exe`, SHA256
`1cab04c6abe31215a71f721b327a8fc4abe1303697f7c9f6c2e47a5428eb758f`.
The root `build/ironphoenix.exe` has the same hash and contains both backends.
All final tests use OpenGL, scale 3, 60 Hz, screenshots every 10 seconds,
UP/indexed/resources mode `1`, with FPS measured over 120--184 seconds.

| Final candidate | FPS min / mean / max | Vertex setup us/callback |
| --- | --- | --- |
| GPU verification | 27 / 51.22 / 60 | 4.875 |
| Buffers enabled, no GPU readback | 13 / 32.72 / 55 | 5.551 |
| Buffers off reference | 22 / 37.03 / 60 | 2.743 |

`buffers-coverage-verify/` exercised 15,583,928 owned attribute binds and 3,837
GPU storage checks (including 32 initial upload readbacks), with 3,874 partial
updates, 3 evictions, zero fallback attributes and no reported failures.
`buffers-coverage-fast/` exercised 17,819,158 owned binds, 4,358 partial updates,
10 evictions and zero fallback attributes, with no reported failures. Captured
combat geometry, lighting and HUD were inspected. These are sampled GPU-storage
checks and limited visual validation, not pixel equivalence or proof of complete
CPU/GPU temporal equivalence with the legacy path.

Overlapping comparisons reduced vertex setup cost relative to the initial
span candidate, but firm 60 remains unmet. Different encounters explain why
verification can have higher FPS than normal mode; verification is not faster
by design. The normal run processed 1,902,771 draw callbacks in the window versus
1,543,437 for verification, with different state/texture/driver costs. Do not
derive a speedup ratio or resolution-scaling curve from these fights. The buffer
path remains independently experimental, available through `run-hle-buffers.cmd`.

The same-executable `buffers-coverage-reference/` run completed without reported
failures, processing 1,994,292 draw callbacks in the measurement window. Its
vertex setup remained cheaper than the owned path. The final results establish
no overall FPS improvement and do not justify enabling owned buffers by default.
The buffer path reduces upload traffic and provides a tested ownership boundary,
but retains CPU content comparisons and the existing state/texture/surface
pipeline. OpenGL with the existing ordinary-play settings remains recommended.
All benchmark processes have ended. Other target GPUs remain untested.

## Next milestone

Profile and compare complete draw-state submission on a deterministic workload,
then move a bounded set of render/texture/shader state into owned host snapshots
and a host submission path. Current indexed packets still execute the SDK's
general-state helper and the NV2A state/resource pipeline. Expanding storage
ownership alone must not be assumed to remove those costs. Preserve fallback
and compare state/order before bypassing more command generation or RAM checks.
Full allocation/view lifetimes and surface/texture ownership remain open.

The initial span candidate passed a full match (`buffers-final-verify/`):
13,788,892 binds, 3,399 GPU checks, 32 initial upload readbacks, 3,185 partial
updates, and no reported failures. The normal-mode run (`buffers-final-fast/`)
recorded 26 / 44.69 / 61 FPS and 12.237 us per vertex setup callback. Both used
SHA256 `6fa24686b081d46a6b95ee4fab7aaa99787c4937b38aac2f9ed9cb7b445b5cbf`,
preserved at `scratch/perf/hle-buffers-final.exe`.

Post-window sampling (190--205 seconds, 284 observations/thread) found 70
instruction observations in `memcmp` on the graphics worker, 60 in unnamed
NVIDIA driver code plus 7 under its nearest exported `DrvPresentBuffers` symbol,
and 77 in condition waits. These are sampled instruction locations, not exact
call-stack attribution. This motivated sharing comparison coverage between
interleaved attributes. The first reference run (`buffers-final-reference/`)
was deliberately stopped before completion to test that measured optimization;
its report is incomplete and must not serve as a full-window benchmark.

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
