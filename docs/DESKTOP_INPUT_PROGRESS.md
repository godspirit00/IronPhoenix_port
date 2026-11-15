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
wrap checks. All 30 existing Python tests pass. Live Yama Hall input and menu
transition checks are in progress; do not claim physical mouse feel or Proton
coverage from unit tests. Performance renderer code is unchanged.
