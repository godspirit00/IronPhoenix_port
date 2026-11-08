#pragma once
#include "../common.h"

// Memory-mapped I/O emulation for native Xbox code.
// A region is reserved but left inaccessible; every access faults, the faulting
// instruction is decoded and the access is forwarded to the device callbacks.
typedef uint32_t (*mmio_read_fn)(void *opaque, uint32_t offset, unsigned size);
typedef void (*mmio_write_fn)(void *opaque, uint32_t offset, uint32_t value, unsigned size);

// Reserve [base, base+size) (64K aligned) as a trapping MMIO window.
void mmio_register(uint32_t base, uint32_t size, mmio_read_fn rd, mmio_write_fn wr, void *opaque, const char *name);
// Install the vectored exception handler (once).
void mmio_init();
// Counters for diagnostics
extern volatile uint32_t g_mmio_reads, g_mmio_writes;
