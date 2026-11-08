// Mm*/Nt*VirtualMemory/Ex*Pool - memory management
//
// On the Xbox every byte of memory a title allocates (contiguous memory, virtual memory,
// pool) is backed by the 64 MB of physical RAM, and hardware (GPU, APU) can DMA from any of
// it given its physical address. We reproduce that: the whole physical RAM is one window at
// 0x80000000 (va = 0x80000000 + phys) and *all* Xbox allocations are carved out of it, so
// MmGetPhysicalAddress works for heap memory too.
#include "kernel.h"
#include <windows.h>
#include <map>
#include <vector>

static CRITICAL_SECTION g_mm_lock;
static std::map<uint32_t, uint32_t> g_phys_used; // phys offset -> size (all allocations)
// Top of RAM: 0x10000 bytes of NV2A instance memory (pages 0x3FE0-0x3FEF) + 0x10000 PFN/padding (pages 0x3FF0-0x3FFF)
// (kept at the retail position even though we emulate 128 MB, like devkits)
static const uint32_t g_gpu_instance_base = XBOX_RETAIL_RAM - 0x20000;
static const uint32_t g_gpu_instance_reserved = 0x20000;

struct VBlock {
    uint32_t size;
    uint32_t protect;
    std::vector<uint8_t> committed; // per page
};
static std::map<uint32_t, VBlock> g_vblocks;          // va -> virtual allocation (NtAllocateVirtualMemory)
static std::map<uint32_t, uint32_t> g_contig_blocks;  // va -> size (MmAllocateContiguousMemory*, system memory)

static bool in_window(uint32_t va) { return va >= XBOX_CONTIG_BASE && va < XBOX_CONTIG_BASE + XBOX_RAM_SIZE; }

void mm_init() {
    InitializeCriticalSection(&g_mm_lock);
    // MEM_WRITE_WATCH: the GPU emulation tracks CPU writes to textures/surfaces via GetWriteWatch
    void *p = VirtualAlloc((void *)XBOX_CONTIG_BASE, XBOX_DEVICE_RAM, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p != (void *)XBOX_CONTIG_BASE) fatal("cannot reserve Xbox physical window at %08x (err %lu)", XBOX_CONTIG_BASE, GetLastError());
    void *hi = VirtualAlloc((void *)(XBOX_CONTIG_BASE + XBOX_DEVICE_RAM), XBOX_RAM_SIZE - XBOX_DEVICE_RAM, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (hi != (void *)(XBOX_CONTIG_BASE + XBOX_DEVICE_RAM)) fatal("cannot reserve upper RAM window (err %lu)", GetLastError());
    // The first pages of physical memory are used by the kernel on real hardware; keep them out.
    g_phys_used[0] = 0x10000;
    g_phys_used[g_gpu_instance_base] = g_gpu_instance_reserved;
}

// Find and mark a free physical range. Caller holds g_mm_lock.
static uint32_t phys_alloc_locked(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
    if (align < 0x1000) align = 0x1000;
    size = (size + 0xFFF) & ~0xFFFu;
    uint32_t limit = XBOX_RAM_SIZE;
    if (hi == 0 || hi >= limit) hi = limit - 1;
    uint32_t found = 0xFFFFFFFF;
    uint32_t cursor = 0;
    auto consider = [&](uint32_t gap_lo, uint32_t gap_hi) { // [gap_lo, gap_hi)
        if (gap_lo < lo) gap_lo = lo;
        if (gap_hi > hi + 1) gap_hi = hi + 1;
        if (gap_hi <= gap_lo) return;
        if (top_down) {
            if (gap_hi < size) return;
            uint32_t a = (gap_hi - size) & ~(align - 1);
            if (a >= gap_lo && (found == 0xFFFFFFFF || a > found)) found = a;
        } else {
            uint32_t a = (gap_lo + align - 1) & ~(align - 1);
            if (a + size <= gap_hi && a >= gap_lo && found == 0xFFFFFFFF) found = a;
        }
    };
    for (auto &u : g_phys_used) {
        consider(cursor, u.first);
        cursor = u.first + u.second;
        if (!top_down && found != 0xFFFFFFFF) break;
    }
    consider(cursor, limit);
    if (found == 0xFFFFFFFF) return 0;
    g_phys_used[found] = size;
    return XBOX_CONTIG_BASE + found;
}

static bool phys_range_free_locked(uint32_t phys, uint32_t size) {
    auto it = g_phys_used.upper_bound(phys);
    if (it != g_phys_used.end() && it->first < phys + size) return false;
    if (it != g_phys_used.begin()) {
        --it;
        if (it->first + it->second > phys) return false;
    }
    return phys + size <= XBOX_RAM_SIZE;
}

static std::map<uint32_t, uint32_t> g_pool_free_dummy;
static void log_usage_locked(const char *why) {
    uint32_t used = 0, contig = 0, vres = 0, vcom = 0;
    for (auto &u : g_phys_used) used += u.second;
    for (auto &c : g_contig_blocks) contig += c.second;
    for (auto &v : g_vblocks) {
        vres += v.second.size;
        for (uint8_t c : v.second.committed) vcom += c ? 4096 : 0;
    }
    static int n;
    if (n++ > 4) return;
    logf("mm: %s - %u KB of %u KB in use: contiguous %u KB, virtual reserved %u KB (committed %u KB), %zu vblocks",
         why, used >> 10, XBOX_RAM_SIZE >> 10, contig >> 10, vres >> 10, vcom >> 10, g_vblocks.size());
    for (auto &v : g_vblocks) {
        uint32_t c = 0;
        for (uint8_t x : v.second.committed) c += x ? 4 : 0;
        if (v.second.size >= 0x100000) logf("   vblock %08x size %u KB committed %u KB", v.first, v.second.size >> 10, c);
    }
}

uint32_t mm_contig_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down) {
    if (hi == 0 || hi >= XBOX_DEVICE_RAM) hi = XBOX_DEVICE_RAM - 1;
    EnterCriticalSection(&g_mm_lock);
    uint32_t va = phys_alloc_locked(size, align, lo, hi, top_down);
    if (va) g_contig_blocks[va] = (size + 0xFFF) & ~0xFFFu;
    else {
        logf("mm: contiguous request size %x align %x range %x-%x failed", size, align, lo, hi);
        static bool listed;
        if (!listed) {
            listed = true;
            uint32_t sum = 0;
            for (auto &u : g_phys_used) {
                if (u.first >= 0x1000000) break;
                const char *kind = g_contig_blocks.count(XBOX_CONTIG_BASE + u.first) ? "contig" :
                                   g_vblocks.count(XBOX_CONTIG_BASE + u.first) ? "virtual" : "other/pool";
                sum += u.second;
                if (u.second >= 0x40000) logf("   low16: %07x size %6u KB %s", u.first, u.second >> 10, kind);
            }
            logf("   low16 total %u KB", sum >> 10);
        }
        log_usage_locked("contiguous allocation failed");
    }
    LeaveCriticalSection(&g_mm_lock);
    return va;
}

void mm_contig_free(uint32_t va) {
    EnterCriticalSection(&g_mm_lock);
    if (g_contig_blocks.erase(va)) g_phys_used.erase(va - XBOX_CONTIG_BASE);
    LeaveCriticalSection(&g_mm_lock);
}

uint32_t mm_contig_size(uint32_t va) {
    EnterCriticalSection(&g_mm_lock);
    auto it = g_contig_blocks.find(va);
    uint32_t s = it == g_contig_blocks.end() ? 0 : it->second;
    LeaveCriticalSection(&g_mm_lock);
    return s;
}

// ---------------------------------------------------------------- contiguous memory exports
KEXPORT uint32_t xk_MmAllocateContiguousMemory(uint32_t NumberOfBytes) {
    uint32_t r = mm_contig_alloc(NumberOfBytes, 0x1000, 0, 0xFFFFFFFF, true);
    LOGK("MmAllocateContiguousMemory(%x) -> %08x", NumberOfBytes, r);
    return r;
}

KEXPORT uint32_t xk_MmAllocateContiguousMemoryEx(uint32_t NumberOfBytes, uint32_t Lowest, uint32_t Highest, uint32_t Alignment, uint32_t Protect) {
    uint32_t r = mm_contig_alloc(NumberOfBytes, Alignment, Lowest, Highest, true);
    LOGK("MmAllocateContiguousMemoryEx(%x, %x-%x, align %x, prot %x) -> %08x", NumberOfBytes, Lowest, Highest, Alignment, Protect, r);
    return r;
}

KEXPORT void xk_MmFreeContiguousMemory(uint32_t BaseAddress) {
    LOGK("MmFreeContiguousMemory(%08x)", BaseAddress);
    mm_contig_free(BaseAddress);
}

KEXPORT void xk_MmPersistContiguousMemory(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t Persist) {}

KEXPORT uint32_t xk_MmGetPhysicalAddress(uint32_t BaseAddress) {
    if (in_window(BaseAddress)) return BaseAddress - XBOX_CONTIG_BASE;
    if (BaseAddress >= 0xF0000000) return BaseAddress & 0x0FFFFFFF;
    static int warned;
    if (warned++ < 16) logf("MmGetPhysicalAddress on non-RAM address %08x (caller %p)", BaseAddress, __builtin_return_address(0));
    return BaseAddress & 0x03FFFFFF;
}

KEXPORT uint32_t xk_MmClaimGpuInstanceMemory(uint32_t NumberOfBytes, uint32_t *NumberOfPaddingBytes) {
    // Returns the END of NV2A instance memory, which sits 0x10000 below the top of RAM on retail units.
    // D3D carves its RAMIN structures downward from here. The region is always reserved (see above).
    const uint32_t padding = 0x10000;
    if (NumberOfPaddingBytes) *NumberOfPaddingBytes = padding;
    uint32_t r = XBOX_CONTIG_BASE + XBOX_RETAIL_RAM - padding;
    logf("MmClaimGpuInstanceMemory(%x) -> %08x", NumberOfBytes, r);
    return r;
}

KEXPORT uint32_t xk_MmAllocateSystemMemory(uint32_t NumberOfBytes, uint32_t Protect) {
    uint32_t va = mm_contig_alloc(NumberOfBytes, 0x1000, 0, 0xFFFFFFFF, false);
    if (va) memset(xptr<void>(va), 0, (NumberOfBytes + 0xFFF) & ~0xFFFu);
    return va;
}
KEXPORT uint32_t xk_MmFreeSystemMemory(uint32_t BaseAddress, uint32_t NumberOfBytes) {
    uint32_t s = mm_contig_size(BaseAddress);
    mm_contig_free(BaseAddress);
    return s / 4096;
}

KEXPORT uint32_t xk_MmCreateKernelStack(uint32_t NumberOfBytes, uint32_t DebuggerThread) {
    uint8_t *p = (uint8_t *)VirtualAlloc(nullptr, NumberOfBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return p ? xaddr(p + NumberOfBytes) : 0;
}
KEXPORT void xk_MmDeleteKernelStack(uint32_t StackBase, uint32_t StackLimit) {
    VirtualFree(xptr<void>(StackLimit), 0, MEM_RELEASE);
}

KEXPORT uint32_t xk_MmQueryAllocationSize(uint32_t BaseAddress) {
    EnterCriticalSection(&g_mm_lock);
    uint32_t s = 0;
    auto c = g_contig_blocks.find(BaseAddress);
    if (c != g_contig_blocks.end()) s = c->second;
    auto v = g_vblocks.find(BaseAddress);
    if (v != g_vblocks.end()) s = v->second.size;
    LeaveCriticalSection(&g_mm_lock);
    return s;
}

KEXPORT uint32_t xk_MmQueryAddressProtect(uint32_t VirtualAddress) {
    if (in_window(VirtualAddress)) return PAGE_READWRITE;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(xptr<void>(VirtualAddress), &mbi, sizeof mbi)) return 0;
    return mbi.Protect;
}
KEXPORT void xk_MmSetAddressProtect(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t NewProtect) {
    // Keep memory accessible; write-combine/no-cache/read-only requests are meaningless here
    LOGK("MmSetAddressProtect(%08x, %x, %x)", BaseAddress, NumberOfBytes, NewProtect);
}

KEXPORT uint32_t xk_MmLockUnlockBufferPages(uint32_t BaseAddress, uint32_t NumberOfBytes, uint32_t Unlock) { return 0; }
KEXPORT uint32_t xk_MmLockUnlockPhysicalPage(uint32_t PhysicalAddress, uint32_t Unlock) { return 0; }

struct X_MM_STATISTICS {
    uint32_t Length, TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted, VirtualMemoryBytesReserved,
        CachePagesCommitted, PoolPagesCommitted, StackPagesCommitted, ImagePagesCommitted;
};
KEXPORT uint32_t xk_MmQueryStatistics(X_MM_STATISTICS *s) {
    if (!s || s->Length != sizeof(X_MM_STATISTICS)) return X_STATUS_INVALID_PARAMETER;
    uint32_t used = 0, vres = 0, vcom = 0;
    EnterCriticalSection(&g_mm_lock);
    for (auto &u : g_phys_used) used += u.second;
    for (auto &v : g_vblocks) {
        vres += v.second.size;
        for (uint8_t c : v.second.committed) vcom += c ? 4096 : 0;
    }
    LeaveCriticalSection(&g_mm_lock);
    s->TotalPhysicalPages = XBOX_DEVICE_RAM / 4096;
    s->AvailablePages = used < XBOX_DEVICE_RAM ? (XBOX_DEVICE_RAM - used) / 4096 : 0;
    s->VirtualMemoryBytesCommitted = vcom;
    s->VirtualMemoryBytesReserved = vres;
    s->CachePagesCommitted = 0;
    s->PoolPagesCommitted = 0;
    s->StackPagesCommitted = 64;
    s->ImagePagesCommitted = 0x4B0000 / 4096;
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------- virtual memory
#define X_MEM_COMMIT 0x1000
#define X_MEM_RESERVE 0x2000
#define X_MEM_DECOMMIT 0x4000
#define X_MEM_RELEASE 0x8000
#define X_MEM_NOZERO 0x800000

// Find the virtual block containing va. Caller holds the lock.
static std::map<uint32_t, VBlock>::iterator vblock_find(uint32_t va) {
    auto it = g_vblocks.upper_bound(va);
    if (it == g_vblocks.begin()) return g_vblocks.end();
    --it;
    if (va < it->first + it->second.size) return it;
    return g_vblocks.end();
}

static void commit_pages_locked(VBlock &b, uint32_t base, uint32_t va, uint32_t size, bool zero) {
    uint32_t first = (va - base) >> 12, last = (va - base + size - 1) >> 12;
    for (uint32_t p = first; p <= last && p < b.committed.size(); p++) {
        if (!b.committed[p]) {
            if (zero) memset(xptr<void>(base + p * 4096), 0, 4096);
            b.committed[p] = 1;
        }
    }
}

KEXPORT uint32_t xk_NtAllocateVirtualMemory(uint32_t *BaseAddress, uint32_t ZeroBits, uint32_t *AllocationSize, uint32_t AllocationType, uint32_t Protect) {
    uint32_t want = *BaseAddress;
    uint32_t size = *AllocationSize;
    bool zero = !(AllocationType & X_MEM_NOZERO);
    if (!size) return X_STATUS_INVALID_PARAMETER;
    uint32_t status = X_STATUS_SUCCESS;
    EnterCriticalSection(&g_mm_lock);
    if (want) {
        uint32_t lo = want & ~0xFFFu;
        uint32_t len = ((want + size + 0xFFF) & ~0xFFFu) - lo;
        auto it = vblock_find(lo);
        if (it != g_vblocks.end()) {
            // Commit (or re-reserve) inside an existing reservation
            if (lo + len > it->first + it->second.size) status = X_STATUS_INVALID_PARAMETER;
            else if (AllocationType & X_MEM_COMMIT) commit_pages_locked(it->second, it->first, lo, len, zero);
            *BaseAddress = lo;
            *AllocationSize = len;
        } else if (in_window(lo) && phys_range_free_locked(lo - XBOX_CONTIG_BASE, (len + 0xFFFF) & ~0xFFFFu)) {
            // Allocation at a caller-chosen address inside RAM
            uint32_t base = lo & ~0xFFFFu;
            uint32_t blen = ((lo + len + 0xFFFF) & ~0xFFFFu) - base;
            g_phys_used[base - XBOX_CONTIG_BASE] = blen;
            VBlock b{blen, Protect, std::vector<uint8_t>(blen / 4096, 0)};
            if (AllocationType & X_MEM_COMMIT) commit_pages_locked(b, base, lo, len, zero);
            g_vblocks[base] = std::move(b);
            *BaseAddress = base;
            *AllocationSize = blen;
        } else {
            status = X_STATUS_NO_MEMORY;
        }
    } else {
        // Fresh allocation: 64 KB granularity like NT
        uint32_t blen = (size + 0xFFFF) & ~0xFFFFu;
        // Heap memory has no device address limits: keep it above the retail 64 MB so the
        // low RAM stays free for GPU/APU buffers (D3D caps those below 64 MB).
        uint32_t base = phys_alloc_locked(blen, 0x10000, XBOX_DEVICE_RAM, 0xFFFFFFFF, false);
        if (!base) base = phys_alloc_locked(blen, 0x10000, 0, 0xFFFFFFFF, false);
        if (!base) {
            log_usage_locked("NtAllocateVirtualMemory failed");
            status = X_STATUS_NO_MEMORY;
        } else {
            VBlock b{blen, Protect, std::vector<uint8_t>(blen / 4096, 0)};
            if (AllocationType & X_MEM_COMMIT) commit_pages_locked(b, base, base, (size + 0xFFF) & ~0xFFFu, zero);
            g_vblocks[base] = std::move(b);
            *BaseAddress = base;
            *AllocationSize = (size + 0xFFF) & ~0xFFFu;
        }
    }
    LeaveCriticalSection(&g_mm_lock);
    LOGK("NtAllocateVirtualMemory(%08x, %x, type %x, prot %x) -> %08x %08x", want, size, AllocationType, Protect, status, *BaseAddress);
    if (size >= 0x400000) logf("NtAllocateVirtualMemory big: base %08x size %x type %x -> %08x %08x caller %p", want, size, AllocationType, status, *BaseAddress, __builtin_return_address(0));
    return status;
}

KEXPORT uint32_t xk_NtFreeVirtualMemory(uint32_t *BaseAddress, uint32_t *FreeSize, uint32_t FreeType) {
    uint32_t va = *BaseAddress & ~0xFFFu;
    uint32_t status = X_STATUS_SUCCESS;
    EnterCriticalSection(&g_mm_lock);
    auto it = vblock_find(va);
    if (it == g_vblocks.end()) {
        status = X_STATUS_INVALID_PARAMETER;
    } else if (FreeType & X_MEM_RELEASE) {
        *BaseAddress = it->first;
        if (FreeSize) *FreeSize = it->second.size;
        g_phys_used.erase(it->first - XBOX_CONTIG_BASE);
        g_vblocks.erase(it);
    } else if (FreeType & X_MEM_DECOMMIT) {
        uint32_t size = FreeSize && *FreeSize ? *FreeSize : it->second.size - (va - it->first);
        uint32_t first = (va - it->first) >> 12, last = (va - it->first + size - 1) >> 12;
        for (uint32_t p = first; p <= last && p < it->second.committed.size(); p++) it->second.committed[p] = 0;
    }
    LeaveCriticalSection(&g_mm_lock);
    LOGK("NtFreeVirtualMemory(%08x, %x, %x) -> %08x", va, FreeSize ? *FreeSize : 0, FreeType, status);
    return status;
}

KEXPORT uint32_t xk_NtQueryVirtualMemory(uint32_t BaseAddress, MEMORY_BASIC_INFORMATION *Buffer) {
    if (!in_window(BaseAddress)) {
        return VirtualQuery(xptr<void>(BaseAddress), Buffer, sizeof *Buffer) ? X_STATUS_SUCCESS : X_STATUS_INVALID_PARAMETER;
    }
    uint32_t va = BaseAddress & ~0xFFFu;
    EnterCriticalSection(&g_mm_lock);
    auto it = vblock_find(va);
    memset(Buffer, 0, sizeof *Buffer);
    Buffer->BaseAddress = xptr<void>(va);
    if (it != g_vblocks.end()) {
        const VBlock &b = it->second;
        uint32_t p = (va - it->first) >> 12;
        uint8_t state = b.committed[p];
        uint32_t n = 0;
        while (p + n < b.committed.size() && b.committed[p + n] == state) n++;
        Buffer->AllocationBase = xptr<void>(it->first);
        Buffer->AllocationProtect = b.protect;
        Buffer->RegionSize = n * 4096;
        Buffer->State = state ? MEM_COMMIT : MEM_RESERVE;
        Buffer->Protect = state ? (b.protect ? b.protect : PAGE_READWRITE) : 0;
        Buffer->Type = MEM_PRIVATE;
    } else {
        Buffer->AllocationBase = Buffer->BaseAddress;
        Buffer->RegionSize = 4096;
        Buffer->State = MEM_COMMIT; // contiguous / system memory
        Buffer->Protect = PAGE_READWRITE;
        Buffer->Type = MEM_PRIVATE;
    }
    LeaveCriticalSection(&g_mm_lock);
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------- pool
// First-fit allocator over 256 KB arenas taken from physical RAM. Each block carries a
// 16-byte header so ExQueryPoolBlockSize can report the size.
struct PoolHdr {
    uint32_t size;  // usable bytes
    uint32_t magic;
    uint32_t pad[2];
};
static const uint32_t kPoolMagic = 0x6C6F6F50; // 'Pool'
static std::map<uint32_t, uint32_t> g_pool_free; // address -> size (free chunks, including header space)

static uint32_t pool_alloc_locked(uint32_t n) {
    uint32_t need = ((n + 15) & ~15u) + sizeof(PoolHdr);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (auto it = g_pool_free.begin(); it != g_pool_free.end(); ++it) {
            if (it->second < need) continue;
            uint32_t addr = it->first, avail = it->second;
            g_pool_free.erase(it);
            if (avail - need >= 64) g_pool_free[addr + need] = avail - need;
            else need = avail;
            PoolHdr *h = xptr<PoolHdr>(addr);
            h->size = need - sizeof(PoolHdr);
            h->magic = kPoolMagic;
            memset(h + 1, 0, h->size);
            return addr + sizeof(PoolHdr);
        }
        uint32_t arena_size = need > 0x40000 ? (need + 0xFFFF) & ~0xFFFFu : 0x40000;
        uint32_t arena = phys_alloc_locked(arena_size, 0x1000, XBOX_DEVICE_RAM, 0xFFFFFFFF, false);
        if (!arena) arena = phys_alloc_locked(arena_size, 0x1000, 0, 0xFFFFFFFF, false);
        if (!arena) return 0;
        g_pool_free[arena] = arena_size;
    }
    return 0;
}

KEXPORT uint32_t xk_ExAllocatePoolWithTag(uint32_t NumberOfBytes, uint32_t Tag) {
    EnterCriticalSection(&g_mm_lock);
    uint32_t r = pool_alloc_locked(NumberOfBytes ? NumberOfBytes : 1);
    LeaveCriticalSection(&g_mm_lock);
    if (!r) logf("ExAllocatePoolWithTag(%x) failed", NumberOfBytes);
    return r;
}

KEXPORT void xk_ExFreePool(uint32_t P) {
    if (!P) return;
    PoolHdr *h = xptr<PoolHdr>(P - sizeof(PoolHdr));
    if (h->magic != kPoolMagic) {
        logf("ExFreePool(%08x): bad block", P);
        return;
    }
    h->magic = 0;
    EnterCriticalSection(&g_mm_lock);
    uint32_t addr = P - sizeof(PoolHdr), size = h->size + sizeof(PoolHdr);
    // coalesce with neighbours
    auto next = g_pool_free.find(addr + size);
    if (next != g_pool_free.end()) {
        size += next->second;
        g_pool_free.erase(next);
    }
    auto prev = g_pool_free.lower_bound(addr);
    if (prev != g_pool_free.begin()) {
        --prev;
        if (prev->first + prev->second == addr) {
            prev->second += size;
            LeaveCriticalSection(&g_mm_lock);
            return;
        }
    }
    g_pool_free[addr] = size;
    LeaveCriticalSection(&g_mm_lock);
}

KEXPORT uint32_t xk_ExQueryPoolBlockSize(uint32_t PoolBlock) {
    PoolHdr *h = xptr<PoolHdr>(PoolBlock - sizeof(PoolHdr));
    return h->magic == kPoolMagic ? h->size : 0;
}
