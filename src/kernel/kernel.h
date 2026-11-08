// Xbox kernel (xboxkrnl.exe) reimplementation - shared definitions
#pragma once
#include "../common.h"

#define XSTDCALL __attribute__((stdcall))
#define XFASTCALL __attribute__((fastcall))
#define KEXPORT extern "C" XSTDCALL
#define KEXPORT_FAST extern "C" XFASTCALL

typedef int32_t NTSTATUS_X;
enum : uint32_t {
    X_STATUS_SUCCESS = 0x00000000,
    X_STATUS_PENDING = 0x00000103,
    X_STATUS_TIMEOUT = 0x00000102,
    X_STATUS_ALERTED = 0x00000101,
    X_STATUS_USER_APC = 0x000000C0,
    X_STATUS_OBJECT_NAME_EXISTS = 0x40000000,
    X_STATUS_BUFFER_OVERFLOW = 0x80000005,
    X_STATUS_NO_MORE_FILES = 0x80000006,
    X_STATUS_UNSUCCESSFUL = 0xC0000001,
    X_STATUS_NOT_IMPLEMENTED = 0xC0000002,
    X_STATUS_INVALID_HANDLE = 0xC0000008,
    X_STATUS_INVALID_PARAMETER = 0xC000000D,
    X_STATUS_NO_SUCH_FILE = 0xC000000F,
    X_STATUS_END_OF_FILE = 0xC0000011,
    X_STATUS_NO_MEMORY = 0xC0000017,
    X_STATUS_ACCESS_DENIED = 0xC0000022,
    X_STATUS_BUFFER_TOO_SMALL = 0xC0000023,
    X_STATUS_OBJECT_TYPE_MISMATCH = 0xC0000024,
    X_STATUS_OBJECT_NAME_INVALID = 0xC0000033,
    X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034,
    X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A,
    X_STATUS_SHARING_VIOLATION = 0xC0000043,
    X_STATUS_DISK_FULL = 0xC000007F,
    X_STATUS_FILE_IS_A_DIRECTORY = 0xC00000BA,
    X_STATUS_NOT_SUPPORTED = 0xC00000BB,
    X_STATUS_DIRECTORY_NOT_EMPTY = 0xC0000101,
    X_STATUS_NOT_A_DIRECTORY = 0xC0000103,
    X_STATUS_DEVICE_NOT_CONNECTED = 0xC000009D,
    X_STATUS_UNRECOGNIZED_VOLUME = 0xC000014F,
};

struct X_ANSI_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    uint32_t Buffer; // char*
};
struct X_UNICODE_STRING {
    uint16_t Length;
    uint16_t MaximumLength;
    uint32_t Buffer; // wchar_t*
};
struct X_OBJECT_ATTRIBUTES {
    uint32_t RootDirectory;
    uint32_t ObjectName; // X_ANSI_STRING*
    uint32_t Attributes;
};
struct X_IO_STATUS_BLOCK {
    uint32_t Status;
    uint32_t Information;
};
struct X_LIST_ENTRY {
    uint32_t Flink, Blink;
};
struct X_DISPATCHER_HEADER {
    uint8_t Type;
    uint8_t Absolute;
    uint8_t Size;
    uint8_t Inserted;
    int32_t SignalState;
    X_LIST_ENTRY WaitListHead;
};
enum { X_NotificationEvent = 0, X_SynchronizationEvent = 1, X_MutantObject = 2, X_ProcessObject = 3,
       X_QueueObject = 4, X_SemaphoreObject = 5, X_ThreadObject = 6, X_NotificationTimerObject = 8,
       X_SynchronizationTimerObject = 9 };
struct X_KDPC {
    int16_t Type;
    uint8_t Inserted;
    uint8_t Padding;
    X_LIST_ENTRY DpcListEntry;
    uint32_t DeferredRoutine;
    uint32_t DeferredContext;
    uint32_t SystemArgument1;
    uint32_t SystemArgument2;
};
struct X_KTIMER {
    X_DISPATCHER_HEADER Header;
    uint64_t DueTime;
    X_LIST_ENTRY TimerListEntry;
    uint32_t Dpc;
    int32_t Period;
};
struct X_KINTERRUPT {
    uint32_t ServiceRoutine;
    uint32_t ServiceContext;
    uint32_t BusInterruptLevel;
    uint32_t Irql;
    uint8_t Connected;
    uint8_t ShareVector;
    uint8_t Mode;
    uint8_t Pad;
    uint32_t ServiceCount;
    uint32_t DispatchCode[22];
};
// Xbox RTL_CRITICAL_SECTION (28 bytes)
struct X_RTL_CRITICAL_SECTION {
    X_DISPATCHER_HEADER Event;   // embedded synchronization event
    int32_t LockCount;
    int32_t RecursionCount;
    uint32_t OwningThread;
};

// --- Per-thread emulated KPCR + ETHREAD ---------------------------------------
// Xbox code reads a few fields through fs:. Those instructions are patched to read
// from this block (pointer kept in a Windows TLS slot). See cpu/fspatch.cpp.
struct XThreadBlock {
    // KPCR image (offsets match the Xbox KPCR)
    union {
        uint8_t kpcr[0x300];
        struct {
            uint32_t ExceptionList;  // 0x00 (unused; real SEH chain is in the Windows TEB)
            uint32_t TlsArray;       // 0x04 Xbox stores the TLS array pointer in NtTib.StackBase
            uint32_t StackLimit;     // 0x08
            uint32_t tib_rest[4];    // 0x0C..0x1B
            uint32_t SelfPcr;        // 0x1C
            uint32_t Prcb;           // 0x20 -> &kpcr[0x28]
            uint8_t Irql;            // 0x24
            uint8_t pad[3];
            uint32_t CurrentThread;  // 0x28 (KPRCB.CurrentThread) -> &ethread
        };
    };
    // ETHREAD image
    union {
        uint8_t ethread[0x200];
        struct {
            X_DISPATCHER_HEADER Header; // 0x00
            uint8_t kt_pad[0x28 - 0x10];
            uint32_t TlsData;           // 0x28
        };
    };
    // Host side bookkeeping (not visible to Xbox code)
    uint32_t host_tid;
    void *host_handle;
    uint32_t start_routine, start_context, system_routine;
    uint8_t *tls_alloc;
    int dpc_lock_depth;
};
static_assert(sizeof(((XThreadBlock *)0)->kpcr) == 0x300, "kpcr");
#define ETHREAD_UNIQUE_THREAD_OFF 0x12C

XThreadBlock *xthread_current();               // block of calling thread (creates one for foreign threads)
XThreadBlock *xthread_create_block(uint32_t tls_size);
void xthread_attach(XThreadBlock *b);          // bind block to the calling host thread
extern uint32_t g_xthread_tls_slot;            // Windows TLS index holding XThreadBlock*

// --- kernel export table -------------------------------------------------------
struct KernelExport {
    uint16_t ordinal;
    const char *name;
    void *addr;
};
extern const KernelExport g_kernel_exports[];
extern "C" void kstub_log(int ordinal, const char *name, int nargs, const uint32_t *args);

// --- subsystem init ------------------------------------------------------------
void mm_init();
void ke_init();
void io_init();

// Contiguous ("physical") memory window: Xbox virtual 0x80000000 + phys
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_RAM_SIZE (256u << 20)   // whole flat "physical" window at 0x80000000
#define XBOX_DEVICE_RAM (128u << 20) // low part: GPU/APU-visible, write-watched; contiguous allocations live here
                                     // high part: heap reservations (NT heap reserves big, commits little)
#define XBOX_RETAIL_RAM (64u << 20)  // NV2A instance memory stays 0x10000 below the retail 64 MB top
uint32_t mm_contig_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down);
void mm_contig_free(uint32_t va);
uint32_t mm_contig_size(uint32_t va);

// Raise/lower the emulated IRQL (DISPATCH_LEVEL acts as a global lock on the host)
uint8_t ke_raise_irql(uint8_t irql);
void ke_lower_irql(uint8_t irql);

// Generic waiting on Xbox dispatcher objects living in Xbox memory
uint32_t ke_wait_objects(uint32_t count, const uint32_t *objects, bool wait_all, const int64_t *timeout_100ns, bool alertable);
void ke_signal_object(uint32_t object);
void *ke_object_host_event(uint32_t object);

// Timeout helpers: Xbox LARGE_INTEGER* timeouts (negative relative 100ns, positive absolute)
uint32_t timeout_to_ms(const int64_t *timeout);
