#include "xbe.h"
#include "kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

XbeInfo g_xbe;
void *misc_ntdll_forward(const char *name);

// The executable reserves 0x11000..0x4B0000 (see src/reserve.S); the PE header page
// at 0x10000 gets overwritten with the XBE header.
static const uint32_t kRegionStart = 0x10000;
static const uint32_t kRegionEnd = 0x4B0000;

const char *xbe_section_name(const XbeSectionHeader *s) { return xptr<const char>(s->SectionName); }

bool xbe_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        logf("cannot open %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = (uint8_t *)malloc(size);
    fread(file, 1, size, f);
    fclose(f);
    if (memcmp(file, "XBEH", 4) != 0) {
        logf("%s is not an XBE", path);
        return false;
    }
    uint32_t *h = (uint32_t *)file;
    uint32_t base = h[0x104 / 4];
    uint32_t hdr_size = h[0x108 / 4];
    uint32_t img_size = h[0x10C / 4];
    if (base != kRegionStart || base + img_size > kRegionEnd) {
        logf("unexpected XBE layout base=%08x size=%08x", base, img_size);
        return false;
    }
    DWORD old;
    if (!VirtualProtect((void *)kRegionStart, kRegionEnd - kRegionStart, PAGE_EXECUTE_READWRITE, &old))
        fatal("VirtualProtect(xbe region) failed: %lu", GetLastError());

    // Hybrid header page. Windows (and DLLs such as rpcrt4) keep reading our PE headers
    // through the module base, while Xbox code reads the XBE header at the same address.
    // The XBE header uses 0x000..hdr_size; the only PE fields needed at fixed offsets are
    // 'MZ' (offset 0, over the XBE magic, which nothing checks) and e_lfanew (0x3C, inside
    // the never-read RSA signature). The NT headers are relocated past the XBE header.
    const uint32_t kNtOff = 0xC00;
    uint8_t *mod = (uint8_t *)kRegionStart;
    uint32_t lfanew = *(uint32_t *)(mod + 0x3C);
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(mod + lfanew);
    uint32_t nt_size = (uint32_t)(offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader +
                                  nt->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER));
    if (hdr_size > kNtOff || kNtOff + nt_size > 0x1000) fatal("header page overflow (xbe %x, nt %x)", hdr_size, nt_size);
    uint8_t ntcopy[0x400];
    memcpy(ntcopy, nt, nt_size);
    memset((void *)(kRegionStart + 0x1000), 0, kRegionEnd - kRegionStart - 0x1000);
    memset(mod, 0, 0x1000);
    memcpy(mod, file, hdr_size);
    memcpy(mod + kNtOff, ntcopy, nt_size);
    mod[0] = 'M';
    mod[1] = 'Z';
    *(uint32_t *)(mod + 0x3C) = kNtOff;

    g_xbe.base = base;
    g_xbe.image_size = img_size;
    g_xbe.cert = h[0x118 / 4];
    g_xbe.nsections = h[0x11C / 4];
    g_xbe.sections = xptr<XbeSectionHeader>(h[0x120 / 4]);
    uint32_t ep = h[0x128 / 4], kt = h[0x158 / 4];
    g_xbe.entry = ep ^ 0xA8FC57AB;
    g_xbe.kernel_thunk = kt ^ 0x5B6D40B6;
    if (g_xbe.entry < base || g_xbe.entry >= base + img_size) { // debug XBE keys
        g_xbe.entry = ep ^ 0x94859D4B;
        g_xbe.kernel_thunk = kt ^ 0xEFB1F152;
    }
    g_xbe.tls_dir = h[0x12C / 4];
    g_xbe.title_id = *xptr<uint32_t>(g_xbe.cert + 8);

    for (uint32_t i = 0; i < g_xbe.nsections; i++) {
        XbeSectionHeader *s = &g_xbe.sections[i];
        memcpy(xptr<void>(s->VirtualAddress), file + s->RawAddress, s->RawSize);
        logf("section %-10s %08x-%08x raw %06x", xbe_section_name(s), s->VirtualAddress,
             s->VirtualAddress + s->VirtualSize, s->RawSize);
    }
    free(file);
    logf("XBE loaded: entry=%08x thunk=%08x title=%08x", g_xbe.entry, g_xbe.kernel_thunk, g_xbe.title_id);
    return true;
}

void xbe_resolve_kernel_imports() {
    uint32_t *thunk = xptr<uint32_t>(g_xbe.kernel_thunk);
    for (; *thunk; thunk++) {
        uint32_t ord = *thunk & 0x7FFFFFFF;
        const KernelExport *e = g_kernel_exports;
        while (e->name && e->ordinal != ord) e++;
        if (!e->name) fatal("kernel import %u not in export table", ord);
        void *fwd = misc_ntdll_forward(e->name);
        *thunk = xaddr(fwd ? fwd : e->addr);
    }
}
