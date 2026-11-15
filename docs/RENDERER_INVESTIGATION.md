# Renderer investigation — 2026-11-15

User reports Vulkan is slower than OpenGL during extended play. Treat the
Vulkan implementation as experimental; previous unpaired FPS samples were not
evidence of a Vulkan speedup.

## Investigation in progress

- Added opt-in CPU scopes to Vulkan draw, texture, shader, vertex, surface,
  blit and report work. The existing `XB_CPU_TIMING` switch controls them.
- Separately time Vulkan finish calls, main fence waits, helper queue-idle waits,
  and the GL presenter completion wait. These scopes nest: do not sum them.
- Comparing the same executable, script, 60 Hz and scale 3 across both backends.
  Combat is not deterministic, so these are workload observations, not exact
  replay comparisons. Resolution sensitivity requires a scale-1 run as well.

## Evidence from source

Vulkan's `pgraph_vk_end_single_time_commands` waits for `vkQueueWaitIdle` on
every helper submission. `pgraph_vk_finish` waits for the main submission fence
before recycling buffers and descriptors. Display output also waits for the GL
presenter before overwriting its shared image. These are correctness constraints
of the current implementation, not proof that an individual wait dominates.

The Vulkan backend has not received the GL render-target alias parking or RAM
hash rules. Its surface invalidation path downloads and invalidates overlapping
targets; the previous Vulkan combat logs show roughly three downloads and three
uploads per displayed frame. Sampling timers will quantify their cost.

## High-level D3D8 replacement feasibility

The checked-in call inventory (`notes/d3d_calls_from_text.txt`) identifies 127
distinct D3D/XGRPH targets and 503 static call sites, with 13 unidentified targets.
These are static references, not dynamic call counts or complete coverage.
Known calls include DrawIndexedVertices, DrawVerticesUP, shader creation/binding,
state blocks, resource registration/locking, render-target changes, visibility
queries, and RunPushBuffer. The latter at 0x232cf0 reads the Xbox pushbuffer
resource and executes/references GPU commands; replacing ordinary draw functions
alone cannot cover that path. Inline command generation and indirect calls also
need auditing.

Intercepting Xbox D3D8 above NV2A is a plausible way to bypass guest command
generation/decoding and track resource ownership directly. It still requires
translation of Xbox shaders, vertex formats, swizzled/compressed textures,
render states and pushbuffers. It is high-level emulation/translation, not a
drop-in desktop D3D8 DLL or removal of every emulated subsystem. The Xbox CPU
code already executes natively; audio/kernel behavior remains separate.

Do not remove the working renderer or promise firm 60 fps on any target based
on this audit. First establish which costs a higher-level path would bypass.
