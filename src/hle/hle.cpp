#include "hle.h"
#include "../kernel/kernel.h"
#include <windows.h>

void hle_install() {
    hle_input_install();
    hle_xapi_install();
    hle_fixes_install();
}

void hle_main_loop() {
    // Placeholder until the renderer owns the window: keep the process alive.
    for (;;) Sleep(1000);
}
