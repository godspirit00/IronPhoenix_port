#pragma once
#include "../common.h"

// Install all high-level replacements of XDK library functions.
void hle_install();
// Runs on the process main thread: window + message pump. Returns on quit.
void hle_main_loop();

void hle_input_install();
void hle_xapi_install();
void hle_fixes_install();
void hle_input_set_script(const char *spec);
