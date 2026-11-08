// APU interrupt delivery: level-triggered line on Xbox IRQ 5
#include "../common.h"
#include <windows.h>
#include "../kernel/kernel.h"
#define XBOX_RAM_SIZE_APU XBOX_RAM_SIZE

extern bool ke_fire_interrupt(uint32_t irq);
static HANDLE g_apu_irq_event;
static volatile bool g_apu_irq_level;
static const uint32_t kApuIrq = 5;

static DWORD WINAPI apu_irq_thread(void *) {
    logf("apu irq thread: tid %lu", GetCurrentThreadId());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        WaitForSingleObject(g_apu_irq_event, INFINITE);
        for (int spins = 0; g_apu_irq_level && spins < 1000; spins++) {
            if (!ke_fire_interrupt(kApuIrq)) break;
            if (spins > 4) Sleep(0);
        }
    }
}

extern "C" void mcpx_apu_host_set_irq(bool level) {
    g_apu_irq_level = level;
    if (level) SetEvent(g_apu_irq_event);
}

extern "C" void mcpx_apu_host_init(uint8_t *ram, uint32_t ram_size);

void apu_init() {
    g_apu_irq_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, apu_irq_thread, nullptr, 0, nullptr));
    mcpx_apu_host_init((uint8_t *)0x80000000u, XBOX_RAM_SIZE_APU);
}
