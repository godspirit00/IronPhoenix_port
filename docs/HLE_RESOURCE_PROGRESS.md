# Resource identity and vertex-state frontend — 2026-11-15

This continuation introduces resource identities and native vertex-input state,
while retaining the existing renderer and SDK lifetime/content-coherence rules.
It is not full GPU-resource ownership or completion of the firm-60 goal.

## Implemented

- Signature-checked pass-through hooks audit resource Register/Destroy,
  SetStreamSource, SetTexture, both SetRenderTarget paths, and Lock2DSurface.
- A bounded registry assigns monotonic identity revisions to observed object
  headers. Re-registration, destruction, address reuse and type/Data/view-header
  changes invalidate identities. Refcount changes alone do not. The registry
  holds at most 8,192 observed headers; retired identities are never reused.
- Immutable, pointer-free vertex-input patches include declaration formats,
  offsets, stream strides, base-vertex arithmetic and resource identity revisions.
  They are attached to reserved indexed draw packets, so capacity fallback occurs
  before SDK dirty flags/cached base-index state is committed.
- GPU conformance compares these patches against the original decoded state at
  the ordered marker. Native application uses the same attribute-format semantic
  helper as legacy methods, preserving sizes and compressed-attribute flags.
- `--hle-resources off|observe|verify|1` controls the experiment in perf_run.py.
  Requested verify/native modes require the indexed bridge and actual checked /
  applied updates in the log, not merely a requested configuration.

Identity revisions are **not content versions**. CPU writes may bypass exported
locks; no RAM dirty checks or uploads are skipped on the strength of an identity.
SDK references, resource wait/free behavior and actual backend GPU allocations
remain authoritative. Surface/texture parent/backing aliases are not yet a full
owned-resource graph. Hook counts are entry hits, not unique resource changes.

## Validated first checkpoint

- Native unit tests cover registry revision/retirement/address reuse/bounds,
  declaration layouts, null streams, base-index wrap, dirty masks and immutable
  snapshots (`tools/test_hle_resource_state.cpp`). Existing UP/index packing tests
  remain available. 20 Python tooling tests pass.
- Vulkan-enabled build links.
- `scratch/perf/resources-vertex-verify/`: 2,152,739 nonempty vertex state patches
  matched original GPU state in a full Buddha Cave run, without reported failures.
- The initial restricted native case handled only clean general state:
  `scratch/perf/resources-native-gl/`, 276,323 native helper preparations and
  75,867 actual applied binding updates, with 2,427,422 delegated updates checked.
  No reported failures or registry evictions. This is only about 3% of nonempty
  binding updates, so it is not sufficient coverage for the intended frontend.
- Both target setter hooks are now present; normal SetRenderTarget exposes many
  transient views missed by the fast-only audit. The latest native run observed
  134,586 retired identities, 761 live headers and no evictions.

## Expansion plan (implemented below)

Expand native vertex-input preparation while retaining the SDK's general
render/texture/shader preparation. Preserve its entry dirty flags and post-general
state separately. Unsupported cases must use the original input remainder without
repeating general state side effects or losing newly dirtied flags. Then run a
fresh direct case, same-build reference, Vulkan gameplay and both build variants.

## Expanded vertex-input path

Native mode now mirrors StateVB's entry flag handling and delegates general
render/texture/shader preparation to the original CDevice_SetState. It then
prepares the vertex-input delta itself, using the post-general cached base and
the original entry format/offset dirty bits. The SDK's post-general dirty bits
are retained for subsequent draws. The special skip state delegates the entire
helper. Unsupported declarations use an SDK input-only remainder: no general
preparation is repeated, and its post-general/newly dirtied flags are preserved.
The general helper's entry and final flag-mask instructions are signature checked.

`scratch/perf/resources-expanded-gl/` completed a full route with 2,474,685 native
preparations, 1,998,356 nonempty patches applied, zero delegated preparations and
no reported runtime failures. Registry: 722 live headers, 126,918 identity revisions,
116,900 retirements, zero evictions. Periodic FPS min/mean/max: 35 / 50.97 / 60.
This is not a completed performance fix or a proven speedup over another fight.
The unsupported-declaration remainder has unit-tested flag construction but was
not exercised by this valid-declaration run.

The exact expanded binary is preserved as
`scratch/perf/hle-resource-state-expanded.exe`, SHA256
`8dae5137a1f092b0505946da334f86323d6302b47c6b4e292b754fce340078b1`.
Same-build original-state reference is preserved in
`scratch/perf/resources-expanded-reference/`. The resource frontend stays
independently opt-in.

The same-build reference has now completed: 2,393,839 nonempty patches matched
original GPU state, with no reported failures or registry evictions. Periodic
FPS min/mean/max was 50 / 57.59 / 60 versus native 35 / 50.97 / 60. The reference
processed 2,269,311 draw callbacks versus native 1,795,116, with different
fight/camera/material work. This does **not establish a performance improvement**;
do not recommend the resource mode as a speed fix. Reference mode includes the
same resource audit/snapshot construction plus GPU state checks. Both modes
retain general state processing and backend resource/content tracking.

`run-hle-resources.cmd` is the explicit native vertex-state experiment.
`run-hle.cmd` and `run-hle-up.cmd` disable resource mode for their existing scopes.
Vulkan full-route validation completed in `resources-expanded-vulkan/`:
2,061,470 native preparations and 1,669,410 nonempty patches applied, with zero
delegated preparations, registry evictions or reported runtime failures. FPS
min/mean/max: 22 / 29.53 / 59 in that different encounter; it is not a controlled
GL/Vulkan ratio. Captured GL and Vulkan combat geometry, lighting and HUD were
inspected. This is limited visual validation, not pixel-for-pixel equivalence.

Both Vulkan-enabled and GL-only builds pass. The exact tested Vulkan-enabled
binary is restored afterward, retaining both selectable renderers. All benchmark
processes have ended. Existing `.claude/` worktrees were left untouched.
20 Python tests and the native resource/state test (including fallback flag
construction) pass. No generated shader code changed, so cache keys are unchanged.

## Next resource boundary

Vertex-input preparation now has useful coverage, but backend buffer/texture
allocations, RAM dirtiness, upload decisions and surface synchronization remain
legacy. Next, model backing allocations and parent/view aliases, detect CPU
content changes (including direct writes), and retain in-flight resource versions
until GPU completion. Only then can resource IDs safely select owned GPU objects
and replace per-draw guest-RAM tracking. Header identity alone is insufficient.
Keep deterministic workloads/frame-time measurement on the validation backlog;
these evolving fights cannot establish small performance differences reliably.

The first restricted candidate is preserved at
`scratch/perf/hle-resource-state-prototype.exe` (SHA256
`4fe572b7393b0c5c06d3bb2b1967064250d098e909c89c9159ae14b754b5e1ca`).
