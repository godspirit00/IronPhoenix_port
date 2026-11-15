# Desktop input checkpoint (2026-11-15)

Requested bindings are implemented in `src/hle/input.cpp`: mouse motion to right
stick, left/right click to X/Y, middle click to RT, Left Ctrl to LT and Left
Shift to B. Existing letter-key alternatives and XInput controllers remain.
Enter is A in menus and Start in gameplay; Esc is B in menus and Start in
gameplay. Startup Enter also sends Start to skip intros. P always sends Start.

Capture is owned by the presentation thread, using foreground raw relative mouse
input with a centered-cursor fallback for absolute/legacy devices. It releases on
menus, pause, focus loss, window move/resize, system menus and normal shutdown.
Key latches suppress held actions across gameplay/menu and focus transitions.
Mouse-look samples are shared across repeated input polls, expire after 32 ms,
and compensate the game's stick dead zone. This remains stick emulation, with
the game's turn-speed limit and inversion option.

Configuration: `--mouse-sensitivity VALUE` (default 1, range 0.05..10),
`--no-mouse-capture`. Exclusive scripted input (`--input-only`) never captures
the pointer. `XB_INPUT_TRACE=1` logs merged pad changes for debugging.

## Game-state evidence

The script defines app PLAY_GAME=50, UI normal play=100/110, pause=150 and
settings=160. Native GetAppState wrapper 0x99950 reads manager global 0x36224c,
offset 0x5e4; GetActiveUI wrapper 0x95a10 reads offset 0x204. ActivateUI at
0x6d630 writes pending UI at 0x208; ActivateUITop at 0x6d620 writes 0x20c.
Getter instruction signatures are checked before enabling capture, and manager
memory is validated. Unsupported images leave capture disabled. Pending menus
and top dialogs also prevent capture.

## Validation

Both Vulkan-enabled and OpenGL-only builds compile. The new native
`tools/test_desktop_input.cpp` passes context classification, held-key/focus
gates, axis direction, radial clamp, idle reset, poll independence and clock
wrap checks. All 30 existing Python tests pass. A 141-second live Yama Hall run
passed gameplay -> pause -> Settings -> pause -> gameplay. Read-only observations
of the owned process confirmed app=50 throughout, active UI=110/150/160/150/110,
and pending/top=0 at each checkpoint. Log: `scratch/input-menu-smoke.log`;
reproduction helper: `scratch/input_menu_smoke.py` (uses normal scripted input,
not exclusive input, and observes state without injecting desktop input).

This session exposes no foreground desktop (GetForegroundWindow returns null).
The scoped interactive helper aborted before injecting mouse/key input because
the game could not become foreground. Physical raw mouse delivery, cursor
release/recapture, camera feel, physical controller mixing and Proton therefore
remain unverified. Check actual mouse look, all requested buttons, held Esc/Enter
across pause/resume, Settings/back, Alt+Tab and Alt+Enter on an interactive PC.
Unit coverage verifies focus/context latches, but does not replace these checks.

The final build additionally blocks Return until release after Alt+Enter,
including when Alt is released first. Both configurations rebuilt successfully
after that change; the live menu run preceded this isolated shortcut adjustment.
Performance renderer code is unchanged. No new FPS or cross-machine claims.

Final executable: `build/ironphoenix.exe`, preserved as
`scratch/desktop-input-checkpoint.exe`; SHA256:
`aac4c23e522aab3ef5d318e39920fc0a7142700779f76c3e01cb24464581a4e0`.
The live menu test used the earlier input build with SHA256
`b005bd4030898c745298c19316bbe00b66a9fd4e08bbcb0d01429890f1cea784`.
