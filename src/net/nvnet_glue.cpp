// NVNET interrupt line (Xbox IRQ 4)
#include "../common.h"
#include <windows.h>

extern bool ke_fire_interrupt(uint32_t irq);
static HANDLE g_irq_event;
static volatile bool g_level;

static DWORD WINAPI nvnet_irq_thread(void *) {
    for (;;) {
        WaitForSingleObject(g_irq_event, INFINITE);
        for (int spins = 0; g_level && spins < 1000; spins++) {
            if (!ke_fire_interrupt(4)) break;
            if (spins > 4) Sleep(0);
        }
    }
}

extern "C" void nvnet_host_set_irq(bool level) {
    g_level = level;
    if (level) SetEvent(g_irq_event);
}

extern "C" void nvnet_host_init(void);
void nvnet_init() {
    g_irq_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, nvnet_irq_thread, nullptr, 0, nullptr));
    nvnet_host_init();
}
