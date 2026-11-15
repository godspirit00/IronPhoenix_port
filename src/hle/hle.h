#pragma once
#include "../common.h"

// Install all high-level replacements of XDK library functions.
void hle_install();
// Runs on the process main thread: window + message pump. Returns on quit.
void hle_main_loop();

void hle_input_install();
void hle_xapi_install();
void hle_fixes_install();
void hle_input_set_script(const char *spec, bool exclusive = false);
void hle_input_set_focus(bool focused);
void hle_input_set_mouse_client(bool inside);
void hle_input_configure_mouse(float sensitivity, bool enabled);
bool hle_input_wants_mouse_capture();
void hle_input_mouse_capture(bool captured);
void hle_input_mouse_motion(int dx, int dy);
