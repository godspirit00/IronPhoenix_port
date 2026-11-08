#pragma once
#include "common.h"

struct XbeSectionHeader {
    uint32_t Flags;
    uint32_t VirtualAddress;
    uint32_t VirtualSize;
    uint32_t RawAddress;
    uint32_t RawSize;
    uint32_t SectionName;
    uint32_t SectionReferenceCount;
    uint32_t HeadSharedPageReferenceCount;
    uint32_t TailSharedPageReferenceCount;
    uint8_t ShaDigest[20];
};

struct XbeInfo {
    uint32_t base;
    uint32_t entry;
    uint32_t kernel_thunk;
    uint32_t tls_dir;
    uint32_t image_size;
    uint32_t cert;
    uint32_t title_id;
    uint32_t nsections;
    XbeSectionHeader *sections; // points into the mapped header at 0x10000
};
extern XbeInfo g_xbe;

// Map default.xbe into the reserved region at 0x10000. Returns false on error.
bool xbe_load(const char *path);
// Resolve the kernel thunk table to host implementations.
void xbe_resolve_kernel_imports();
const char *xbe_section_name(const XbeSectionHeader *s);
