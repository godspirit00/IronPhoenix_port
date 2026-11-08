"""Port xemu's nvnet (MCPX Ethernet) into src/net/ for the Iron Phoenix runtime.
The NIC has no network backend: the link is permanently down ("cable unplugged"),
which is exactly what lets the title's network stack (XNet/XOnline) initialize."""
import os
import re
import shutil

XEMU = os.environ.get('XEMU_SRC', '../xemu-src')  # path to an xemu source checkout
OUT = 'src/net'
os.makedirs(OUT + '/hw/net', exist_ok=True)
shutil.copy(XEMU + '/hw/xbox/mcpx/nvnet/nvnet_regs.h', OUT + '/nvnet_regs.h')
shutil.copy(XEMU + '/include/hw/net/mii.h', OUT + '/hw/net/mii.h')

s = open(XEMU + '/hw/xbox/mcpx/nvnet/nvnet.c', encoding='utf-8').read()

# includes
i = s.index('#include "qemu/osdep.h"')
j = s.index('#include "nvnet_regs.h"')
s = s[:i] + '#include "qemu/osdep.h"\n#include "hw/net/mii.h"\n#include "nvnet_net_compat.h"\n' + s[j:]

s = s.replace('#define TYPE_NVNET "nvnet"\nOBJECT_DECLARE_SIMPLE_TYPE(NvNetState, NVNET)\n', '')
s = s.replace('''    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/

''', '')

# IRQ and DMA
s = s.replace('pci_irq_assert(d);', 'nvnet_host_set_irq(true);')
s = s.replace('pci_irq_deassert(d);', 'nvnet_host_set_irq(false);')
s = re.sub(r'\n\s*PCIDevice \*d = PCI_DEVICE\(s\);', '', s)
s = s.replace('pci_dma_read(d, ', 'nvnet_dma_read(')
s = s.replace('pci_dma_write(d, ', 'nvnet_dma_write(')

# cut the QEMU device model (realize, vmstate, class init, type registration)
cut = s.index('static void nvnet_realize(PCIDevice *pci_dev, Error **errp)')
keep_tail_start = s.index('// clang-format off', cut)
keep_tail_end = s.index('static void nvnet_reset_hold(Object *obj, ResetType type)')
tail = s[keep_tail_start:keep_tail_end]
s = s[:cut] + tail + r'''
/* ---------------------------------------------------------------------------
 * Iron Phoenix host glue (replaces the QEMU PCI device model)
 * ------------------------------------------------------------------------- */
extern void mmio_register_c(uint32_t base, uint32_t size, uint32_t (*rd)(void *, uint32_t, unsigned),
                            void (*wr)(void *, uint32_t, uint32_t, unsigned), void *opaque, const char *name);

static uint32_t nvnet_host_mmio_read(void *opaque, uint32_t addr, unsigned size)
{
    return addr < MMIO_SIZE ? (uint32_t)nvnet_mmio_read(opaque, addr, size) : 0;
}

static void nvnet_host_mmio_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    if (addr < MMIO_SIZE) {
        nvnet_mmio_write(opaque, addr, val, size);
    }
}

void nvnet_host_init(void)
{
    static NvNetState state;
    NvNetState *s = &state;
    static NICState nic;
    nic.nc.link_down = true; /* no cable */
    nic.nc.opaque = s;
    nic.nc.info = &nvnet_client_info;
    s->nic = &nic;
    s->autoneg_timer = g_new0(QEMUTimer, 1);
    timer_init_full(s->autoneg_timer, QEMU_CLOCK_VIRTUAL, SCALE_MS, autoneg_timer, s);
    mmio_register_c(0xFEF00000, 0x10000, nvnet_host_mmio_read, nvnet_host_mmio_write, s, "NVNET");
    nvnet_reset(s);
    nv2a_log("nvnet: initialized (link down)");
}
'''
open(OUT + '/nvnet.c', 'w', encoding='utf-8').write(s)
print('ported nvnet ->', OUT)
