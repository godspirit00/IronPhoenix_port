/*
 * QEMU Geforce NV2A implementation - PBUS (PCI config mirror)
 * Adapted for the Iron Phoenix native runtime: no PCI device, fixed IDs.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "nv2a_int.h"

static uint32_t pci_command = 0x02B00007;

/* PBUS - bus control */
uint64_t pbus_read(void *opaque, hwaddr addr, unsigned int size)
{
    uint64_t r = 0;
    switch (addr) {
    case NV_PBUS_PCI_NV_0:
        r = 0x02A010DE; /* device 0x02A0 (NV2A), vendor 0x10DE (NVIDIA) */
        break;
    case NV_PBUS_PCI_NV_1:
        r = pci_command;
        break;
    case NV_PBUS_PCI_NV_2:
        r = 0x030000A1; /* class VGA, revision A1 */
        break;
    default:
        break;
    }

    nv2a_reg_log_read(NV_PBUS, addr, size, r);
    return r;
}

void pbus_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{
    nv2a_reg_log_write(NV_PBUS, addr, size, val);

    switch (addr) {
    case NV_PBUS_PCI_NV_1:
        pci_command = (uint32_t)val;
        break;
    default:
        break;
    }
}
