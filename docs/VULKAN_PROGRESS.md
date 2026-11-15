# Vulkan implementation checkpoint

User authorized switching to Vulkan after the OpenGL optimization passes.
Follow-up (2026-11-15): the reported sustained slowdown relative to GL has been
reproduced. See [RENDERER_INVESTIGATION.md](RENDERER_INVESTIGATION.md) for the
CPU/wait timings, native-resolution probes and high-level D3D8 assessment.
Use OpenGL as the ordinary-play baseline while Vulkan remains experimental.

## Source and build

- Imported xemu's Vulkan backend from pinned revision
  `d6cb7536a2acf0dcc41934a627a0d63a5700ec76`, retaining license headers.
- Added native host/container adapters; removed the SDL offscreen-window
  requirement. The existing window presenter will sample a shared GPU image.
- Corrected 32-bit issues: non-dispatchable Vulkan handles use `VK_NULL_HANDLE`,
  and vertex buffer sizes use `VkDeviceSize` instead of `size_t`.
- Bounded oversized upstream compute scratch allocations for this 32-bit process.
- Vulkan C sources pass syntax/compatibility checks with the pinned dependencies.
- `python tools/prepare_vulkan.py` fetches checksum-pinned headers, volk, VMA,
  SPIRV-Reflect, and glslang into ignored `toolchain/vulkan`.
- `tools/build_glslang.py` builds glslang's declared source groups directly with
  llvm-mingw (no system CMake required); the 32-bit static library built successfully.
- Build variant: `VULKAN=1 ./build.sh`, separate `build/obj-vulkan` object directory.
  The ordinary build continues to exclude Vulkan sources/dependencies.
- Saved prior working executable: `scratch/perf/ironphoenix-before-vulkan.exe`.

## Integration changes

- `--renderer opengl|vulkan`; an explicitly requested renderer must initialize
  successfully. It may not silently switch to the other backend.
- Logs identify `Renderer active:` and selected Vulkan physical device. Benchmark
  runner accepts `--renderer` and rejects missing/mismatched backend evidence.
- Vulkan UI uniform updates include the existing full-width Im2D exception and
  stage-zero render-target detection; inactive uniform lanes are initialized.
- Texture state reuse also checks the DMA-kick epoch and render-target draw time,
  so unchanged sampler state does not hide changed RAM or target contents.

## Pending validation

Full Vulkan-enabled link succeeded. RTX 2070 initialization, external-memory image
presentation, menus and a 185-second scripted Buddha Cave match succeeded.
`scratch/perf/vk-match-first/report.json` identifies the actual backend as Vulkan;
the 120-184 second window at scale 3 / 120 Hz averaged 43.83 fps (27-61).
This does not establish a substantial improvement over the previous GL run:
the encounters vary, and neither backend meets firm 60 fps yet. Captured gameplay
shows the cave, fighters, effects and HUD; a full visual regression pass is pending.

Presenter completion now publishes a shared GL fence, waited before Vulkan
overwrites or destroys the external display image. Vulkan submission already waits
for completion before the presenter samples it. No CPU framebuffer copy is used.
Scaled occlusion counts round up so a visible subpixel effect is not reported as
invisible. A second 185-second match with these fixes at 60 Hz completed without
runtime validation assertions. Its 120-184 second window averaged 49.09 fps
(32-60), with 32 periodic samples; the logger's interval is two seconds at 60 Hz.
This is a different pacing/encounter run, not a paired speedup measurement.
Artifacts: `scratch/perf/vk-match-fenced/`; executable SHA-256:
`d7e86e423916c92b72013e504f68bd98f33f789c406ecc3bec0a487d13121142`.
These runs used runtime assertions, not installed Khronos validation layers.
The 12 performance-tool tests pass, including actual-backend marker parsing.
The OpenGL-only build also links successfully; its 35-second startup/menu smoke
test confirms `Renderer active: OpenGL` (`scratch/perf/gl-after-vulkan-smoke`).
Requesting Vulkan in that build exits with code 1 and an explicit unavailable
message before initializing the game. The delivered `build/ironphoenix.exe` is
restored to the Vulkan-enabled build, with both backends selectable.

Build/reproduce from Git Bash:

```
python tools/prepare_vulkan.py
VULKAN=1 ./build.sh
```

Launch `run-vulkan.cmd` from this checkout, or run the executable with
`--renderer vulkan --game ../extracted --hz 60 --scale 3` from `build`.
Use `--renderer opengl` for the existing backend. Vulkan selection fails explicitly
if initialization is unsupported; it does not silently fall back. The current
interop path requires Vulkan 1.3, Win32 external memory/semaphore support and GL
memory-object import on the same GPU. HD530 and Steam Deck/Proton support are
unverified; do not assume this path works there. OpenGL remains the general default.

Next steps:
Port render-target alias parking and RAM dirty/hash rules as needed, retaining
lazy CPU-access downloads. Texture replacement must also be integrated. Establish
warm shader/pipeline behavior before measuring gameplay performance.

The attempted CMake downloads were stopped after the direct source build worked;
no system tools or NVIDIA driver settings were changed by this implementation.
