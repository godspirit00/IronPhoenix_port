// Nt*File / Io* - file system on top of host directories
//   \Device\CdRom0                 -> g_game_dir           (D:)
//   \Device\Harddisk0\PartitionN   -> g_save_dir\PartitionN (E:, T:, U:, cache X/Y/Z, ...)
#include "kernel.h"
#include <windows.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

extern "C" void nv2a_host_cpu_access(uint32_t va, uint32_t len, bool write);
extern "C" void debug_log_guest_stack(const uint32_t *sp, int want);
std::string hle_file_override(const std::string &xbox_path, const std::string &host_path); // hle/fixes.cpp

// Files opened without FILE_SYNCHRONOUS_IO_* do asynchronous I/O on the Xbox: NtReadFile returns
// STATUS_PENDING and completes later. Shared between an open file and its in-flight reads, so a
// close while a read is pending is safe.
struct AsyncIoState {
    volatile LONG pending = 0;
    volatile LONG refs = 1;
    HANDLE idle = CreateEventA(nullptr, TRUE, TRUE, nullptr); // signaled while nothing is pending
};

static void aio_release(AsyncIoState *a) {
    if (InterlockedDecrement(&a->refs) == 0) {
        CloseHandle(a->idle);
        delete a;
    }
}

struct XFile {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string xbox_path;  // canonical Xbox object path (\Device\...)
    std::string host_path;
    AsyncIoState *aio = nullptr; // non-null: opened for asynchronous I/O
    bool is_dir = false;
    bool is_symlink = false;
    std::string link_target;
    // directory enumeration
    HANDLE find = INVALID_HANDLE_VALUE;
    std::string find_mask;
    bool find_pending = false; // a WIN32_FIND_DATA is buffered in fd
    WIN32_FIND_DATAA fd;
};

static CRITICAL_SECTION g_io_lock;
static std::map<uint32_t, XFile *> g_files;              // Xbox handle -> file
static std::map<std::string, std::string> g_symlinks;    // lower-case "\??\x:" -> target object path

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

static std::string ansi(const X_ANSI_STRING *s) {
    if (!s || !s->Buffer) return {};
    return std::string(xptr<char>(s->Buffer), s->Length);
}

void io_init() {
    InitializeCriticalSection(&g_io_lock);
    g_symlinks["\\??\\d:"] = "\\Device\\CdRom0";
    g_symlinks["\\??\\c:"] = "\\Device\\Harddisk0\\Partition2";
    g_symlinks["\\??\\e:"] = "\\Device\\Harddisk0\\Partition1";
    for (int i = 1; i <= 7; i++) {
        char p[600];
        snprintf(p, sizeof p, "%s\\Partition%d", g_save_dir, i);
        CreateDirectoryA(g_save_dir, nullptr);
        CreateDirectoryA(p, nullptr);
    }
    // Standard directories of a formatted retail HDD (E:)
    for (const char *d : {"\\Partition1\\CACHE", "\\Partition1\\TDATA", "\\Partition1\\UDATA"})
        CreateDirectoryA((std::string(g_save_dir) + d).c_str(), nullptr);
}

// Resolve Xbox object name to (canonical device path, host path). Returns false if unknown device.
static bool resolve_path(const std::string &in, std::string &xbox_out, std::string &host_out) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '/', '\\');
    // Expand symbolic links (\??\X: and anything registered with IoCreateSymbolicLink)
    for (int depth = 0; depth < 4; depth++) {
        std::string lp = lower(p);
        bool changed = false;
        size_t best = 0;
        std::string target;
        for (auto &l : g_symlinks) {
            if (lp.compare(0, l.first.size(), l.first) == 0 && l.first.size() > best &&
                (lp.size() == l.first.size() || lp[l.first.size()] == '\\')) {
                best = l.first.size();
                target = l.second;
            }
        }
        if (best) {
            p = target + p.substr(best);
            changed = true;
        }
        if (!changed) break;
    }
    // Bare drive letter path "D:\foo"
    if (p.size() >= 2 && p[1] == ':') return resolve_path("\\??\\" + p, xbox_out, host_out);
    std::string lp = lower(p);
    xbox_out = p;
    auto starts = [&](const char *pre, std::string &rest) {
        size_t n = strlen(pre);
        if (lp.compare(0, n, pre) != 0) return false;
        if (lp.size() > n && lp[n] != '\\') return false;
        rest = p.substr(n);
        return true;
    };
    std::string rest;
    if (starts("\\device\\cdrom0", rest)) {
        host_out = std::string(g_game_dir) + rest;
    } else if (lp.compare(0, 27, "\\device\\harddisk0\\partition") == 0 && lp.size() >= 28 && isdigit((unsigned char)lp[27])) {
        size_t n = 28;
        while (n < lp.size() && isdigit((unsigned char)lp[n])) n++;
        host_out = std::string(g_save_dir) + "\\Partition" + p.substr(27, n - 27) + p.substr(n);
    } else {
        return false;
    }
    while (host_out.size() > 3 && host_out.back() == '\\') host_out.pop_back();
    return true;
}

static std::string object_name(X_OBJECT_ATTRIBUTES *oa) {
    std::string name = ansi(xptr<X_ANSI_STRING>(oa->ObjectName));
    if (oa->RootDirectory && oa->RootDirectory != 0xFFFFFFFD /*ObDosDevicesDirectory*/) {
        EnterCriticalSection(&g_io_lock);
        auto it = g_files.find(oa->RootDirectory);
        std::string root = it != g_files.end() ? it->second->xbox_path : std::string();
        LeaveCriticalSection(&g_io_lock);
        if (!root.empty()) {
            if (!name.empty() && name[0] != '\\') name = "\\" + name;
            name = root + name;
        }
    } else if (oa->RootDirectory == 0xFFFFFFFD) {
        name = "\\??\\" + name;
    }
    return name;
}

static uint32_t win32_to_status(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: return X_STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_PATH_NOT_FOUND: return X_STATUS_OBJECT_PATH_NOT_FOUND;
    case ERROR_ACCESS_DENIED: return X_STATUS_ACCESS_DENIED;
    case ERROR_SHARING_VIOLATION: return X_STATUS_SHARING_VIOLATION;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return X_STATUS_OBJECT_NAME_COLLISION;
    case ERROR_DIR_NOT_EMPTY: return X_STATUS_DIRECTORY_NOT_EMPTY;
    case ERROR_DIRECTORY: return X_STATUS_NOT_A_DIRECTORY;
    case ERROR_HANDLE_EOF: return X_STATUS_END_OF_FILE;
    case ERROR_DISK_FULL: return X_STATUS_DISK_FULL;
    default: return X_STATUS_UNSUCCESSFUL;
    }
}

static uint32_t new_fake_handle() {
    // A real (unused) event handle guarantees the value is unique among host handles.
    return xaddr(CreateEventA(nullptr, TRUE, TRUE, nullptr));
}

bool io_close_handle(uint32_t h) {
    EnterCriticalSection(&g_io_lock);
    auto it = g_files.find(h);
    if (it == g_files.end()) {
        LeaveCriticalSection(&g_io_lock);
        return false;
    }
    XFile *f = it->second;
    g_files.erase(it);
    LeaveCriticalSection(&g_io_lock);
    if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
    if (f->h != INVALID_HANDLE_VALUE && xaddr(f->h) != h) CloseHandle(f->h);
    CloseHandle(xptr<void>(h));
    LOGF("close %08x %s", h, f->xbox_path.c_str());
    if (f->aio) aio_release(f->aio);
    delete f;
    return true;
}

// What a wait on handle `h` should wait for: for an asynchronous file, its "no I/O pending" event
// (an NT file object is signaled when its I/O completes; the host handle is always signaled).
void *io_wait_object(uint32_t h) {
    EnterCriticalSection(&g_io_lock);
    auto it = g_files.find(h);
    void *w = (it != g_files.end() && it->second->aio) ? it->second->aio->idle : nullptr;
    LeaveCriticalSection(&g_io_lock);
    return w;
}

static XFile *get_file(uint32_t h) {
    EnterCriticalSection(&g_io_lock);
    auto it = g_files.find(h);
    XFile *f = it == g_files.end() ? nullptr : it->second;
    LeaveCriticalSection(&g_io_lock);
    return f;
}

enum { XD_SUPERSEDE, XD_OPEN, XD_CREATE, XD_OPEN_IF, XD_OVERWRITE, XD_OVERWRITE_IF };
enum { FILE_SUPERSEDED, FILE_OPENED, FILE_CREATED, FILE_OVERWRITTEN, FILE_EXISTS, FILE_DOES_NOT_EXIST };
#define X_FILE_DIRECTORY_FILE 0x00000001
#define X_FILE_NON_DIRECTORY_FILE 0x00000040
#define X_FILE_DELETE_ON_CLOSE 0x00001000
#define X_FILE_SYNCHRONOUS_IO_ALERT 0x00000010
#define X_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020

static uint32_t create_file(uint32_t *FileHandle, uint32_t DesiredAccess, X_OBJECT_ATTRIBUTES *oa, X_IO_STATUS_BLOCK *iosb,
                            uint32_t FileAttributes, uint32_t ShareAccess, uint32_t Disposition, uint32_t Options) {
    std::string name = object_name(oa), xpath, hpath;
    if (!resolve_path(name, xpath, hpath)) {
        logf("NtCreateFile: unknown device for '%s'", name.c_str());
        iosb->Status = X_STATUS_OBJECT_PATH_NOT_FOUND;
        return X_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    // Raw whole-disk device: the HDD config area (sectors read by XOnline for Live accounts).
    // Backed by a zero-filled image = a console with no Xbox Live accounts.
    if (lower(xpath) == "\\device\\harddisk0\\partition0") {
        std::string img = std::string(g_save_dir) + "\\Partition0.img";
        HANDLE h = CreateFileA(img.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            iosb->Status = X_STATUS_ACCESS_DENIED;
            return X_STATUS_ACCESS_DENIED;
        }
        LARGE_INTEGER sz;
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart < 0x80000) { // 512 KB config area
            LARGE_INTEGER end;
            end.QuadPart = 0x80000;
            SetFilePointerEx(h, end, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
        }
        XFile *f = new XFile;
        f->xbox_path = xpath;
        f->host_path = img;
        f->h = h;
        EnterCriticalSection(&g_io_lock);
        g_files[xaddr(h)] = f;
        LeaveCriticalSection(&g_io_lock);
        *FileHandle = xaddr(h);
        iosb->Status = X_STATUS_SUCCESS;
        iosb->Information = FILE_OPENED;
        LOGF("NtCreateFile %s -> raw disk image %s", name.c_str(), img.c_str());
        return X_STATUS_SUCCESS;
    }
    if (lower(xpath).compare(0, 14, "\\device\\cdrom0") == 0) hpath = hle_file_override(xpath, hpath);
    DWORD attr = GetFileAttributesA(hpath.c_str());
    bool exists = attr != INVALID_FILE_ATTRIBUTES;
    bool is_dir = exists ? (attr & FILE_ATTRIBUTE_DIRECTORY) != 0 : (Options & X_FILE_DIRECTORY_FILE) != 0;
    uint32_t info = FILE_OPENED;
    uint32_t status = X_STATUS_SUCCESS;

    if (exists && (Options & X_FILE_NON_DIRECTORY_FILE) && is_dir) status = X_STATUS_FILE_IS_A_DIRECTORY;
    else if (exists && (Options & X_FILE_DIRECTORY_FILE) && !is_dir) status = X_STATUS_NOT_A_DIRECTORY;
    else if (exists && Disposition == XD_CREATE) status = X_STATUS_OBJECT_NAME_COLLISION;
    else if (!exists && (Disposition == XD_OPEN || Disposition == XD_OVERWRITE)) {
        // distinguish missing file vs missing parent directory
        std::string parent = hpath.substr(0, hpath.find_last_of('\\'));
        status = GetFileAttributesA(parent.c_str()) == INVALID_FILE_ATTRIBUTES ? X_STATUS_OBJECT_PATH_NOT_FOUND : X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (status != X_STATUS_SUCCESS) {
        LOGF("NtCreateFile %s (%s) disp=%u opt=%x -> %08x", name.c_str(), hpath.c_str(), Disposition, Options, status);
        iosb->Status = status;
        iosb->Information = exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST;
        return status;
    }

    XFile *f = new XFile;
    f->xbox_path = xpath;
    f->host_path = hpath;
    f->is_dir = is_dir;
    if (is_dir) {
        if (!exists) {
            if (!CreateDirectoryA(hpath.c_str(), nullptr)) {
                status = win32_to_status(GetLastError());
                delete f;
                iosb->Status = status;
                return status;
            }
            info = FILE_CREATED;
        }
        f->h = CreateFileA(hpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    } else {
        DWORD disp;
        switch (Disposition) {
        case XD_SUPERSEDE: disp = CREATE_ALWAYS; info = exists ? FILE_SUPERSEDED : FILE_CREATED; break;
        case XD_OPEN: disp = OPEN_EXISTING; info = FILE_OPENED; break;
        case XD_CREATE: disp = CREATE_NEW; info = FILE_CREATED; break;
        case XD_OPEN_IF: disp = OPEN_ALWAYS; info = exists ? FILE_OPENED : FILE_CREATED; break;
        case XD_OVERWRITE: disp = TRUNCATE_EXISTING; info = FILE_OVERWRITTEN; break;
        default: disp = CREATE_ALWAYS; info = exists ? FILE_OVERWRITTEN : FILE_CREATED; break;
        }
        bool on_dvd = lower(xpath).compare(0, 14, "\\device\\cdrom0") == 0;
        DWORD access = GENERIC_READ | (on_dvd ? 0 : GENERIC_WRITE) | DELETE;
        if (DesiredAccess & 0x00010000 /*DELETE*/) access |= DELETE;
        DWORD flags = (Options & X_FILE_DELETE_ON_CLOSE) ? FILE_FLAG_DELETE_ON_CLOSE : 0;
        f->h = CreateFileA(hpath.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp,
                           FILE_ATTRIBUTE_NORMAL | flags, nullptr);
        if (f->h == INVALID_HANDLE_VALUE && !on_dvd) // read-only file on host?
            f->h = CreateFileA(hpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disp, flags, nullptr);
    }
    if (f->h == INVALID_HANDLE_VALUE) {
        status = win32_to_status(GetLastError());
        logf("NtCreateFile %s -> host open failed (%lu)", hpath.c_str(), GetLastError());
        delete f;
        iosb->Status = status;
        return status;
    }
    if (!is_dir && !(Options & (X_FILE_SYNCHRONOUS_IO_ALERT | X_FILE_SYNCHRONOUS_IO_NONALERT))) f->aio = new AsyncIoState;
    uint32_t handle = xaddr(f->h);
    EnterCriticalSection(&g_io_lock);
    g_files[handle] = f;
    LeaveCriticalSection(&g_io_lock);
    *FileHandle = handle;
    iosb->Status = X_STATUS_SUCCESS;
    iosb->Information = info;
    LOGF("NtCreateFile %s disp=%u opt=%x -> %08x (%s)", name.c_str(), Disposition, Options, handle, hpath.c_str());
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtCreateFile(uint32_t *FileHandle, uint32_t DesiredAccess, X_OBJECT_ATTRIBUTES *ObjectAttributes,
                                 X_IO_STATUS_BLOCK *IoStatusBlock, int64_t *AllocationSize, uint32_t FileAttributes,
                                 uint32_t ShareAccess, uint32_t CreateDisposition, uint32_t CreateOptions) {
    return create_file(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
}

KEXPORT uint32_t xk_NtOpenFile(uint32_t *FileHandle, uint32_t DesiredAccess, X_OBJECT_ATTRIBUTES *ObjectAttributes,
                               X_IO_STATUS_BLOCK *IoStatusBlock, uint32_t ShareAccess, uint32_t OpenOptions) {
    return create_file(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, 0, ShareAccess, XD_OPEN, OpenOptions);
}

typedef void(XSTDCALL *IoApc_t)(uint32_t ctx, uint32_t iosb, uint32_t reserved);
struct ApcPacket {
    uint32_t routine, ctx, iosb;
};
static void CALLBACK apc_trampoline(ULONG_PTR p) {
    ApcPacket *a = (ApcPacket *)p;
    ((IoApc_t)xptr<void>(a->routine))(a->ctx, a->iosb, 0);
    delete a;
}

static void complete_io(uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext, X_IO_STATUS_BLOCK *iosb,
                        HANDLE apc_thread = GetCurrentThread()) {
    if (Event) SetEvent(xptr<void>(Event));
    if (ApcRoutine) QueueUserAPC(apc_trampoline, apc_thread, (ULONG_PTR) new ApcPacket{ApcRoutine, ApcContext, xaddr(iosb)});
}

// Deferred completion of asynchronous reads. The data is read at once, but the request reports
// STATUS_PENDING and completes kAsyncReadLatencyUs later, as a disk read would. XACT depends on it:
// it issues a sound's first packet read, and if a short sound needs no second packet it asks
// whether any read is still in flight; with none it ends the stream before it ever plays (the
// one-packet menu sounds went silent when reads completed synchronously).
static const int64_t kAsyncReadLatencyUs = 2000;
struct DeferredIo {
    int64_t due_us;
    X_IO_STATUS_BLOCK *iosb;
    uint32_t status, info, event, apc, ctx;
    HANDLE thread; // issuing thread, for the APC
    AsyncIoState *aio;
};
static std::vector<DeferredIo> g_dio;
static CRITICAL_SECTION g_dio_lock;
static HANDLE g_dio_wake;

static int64_t now_us() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000000 / freq.QuadPart;
}

static DWORD WINAPI dio_thread(void *) {
    for (;;) {
        EnterCriticalSection(&g_dio_lock);
        int64_t wait_us = -1;
        if (!g_dio.empty()) wait_us = g_dio.front().due_us - now_us(); // FIFO: equal latency keeps order
        if (wait_us > 0 || g_dio.empty()) {
            LeaveCriticalSection(&g_dio_lock);
            WaitForSingleObject(g_dio_wake, wait_us < 0 ? INFINITE : (DWORD)((wait_us + 999) / 1000));
            continue;
        }
        DeferredIo d = g_dio.front();
        g_dio.erase(g_dio.begin());
        LeaveCriticalSection(&g_dio_lock);
        d.iosb->Information = d.info;
        MemoryBarrier();
        d.iosb->Status = d.status; // what pollers (HasOverlappedIoCompleted, XACT) look at
        complete_io(d.event, d.apc, d.ctx, d.iosb, d.thread);
        CloseHandle(d.thread);
        if (InterlockedDecrement(&d.aio->pending) == 0) SetEvent(d.aio->idle);
        aio_release(d.aio);
    }
}

static void defer_completion(AsyncIoState *aio, X_IO_STATUS_BLOCK *iosb, uint32_t status, uint32_t info, uint32_t event,
                             uint32_t apc, uint32_t ctx) {
    static const bool started = [] { // thread-safe one-time init
        InitializeCriticalSection(&g_dio_lock);
        g_dio_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CloseHandle(CreateThread(nullptr, 0, dio_thread, nullptr, 0, nullptr));
        return true;
    }();
    (void)started;
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    InterlockedIncrement(&aio->refs);
    if (InterlockedIncrement(&aio->pending) == 1) ResetEvent(aio->idle);
    EnterCriticalSection(&g_dio_lock);
    g_dio.push_back({now_us() + kAsyncReadLatencyUs, iosb, status, info, event, apc, ctx, thread, aio});
    LeaveCriticalSection(&g_dio_lock);
    SetEvent(g_dio_wake);
}

KEXPORT uint32_t xk_NtReadFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
                               X_IO_STATUS_BLOCK *IoStatusBlock, void *Buffer, uint32_t Length, int64_t *ByteOffset) {
    XFile *f = get_file(FileHandle);
    HANDLE h = f ? f->h : xptr<void>(FileHandle);
    OVERLAPPED ov = {};
    OVERLAPPED *pov = nullptr;
    if (ByteOffset && *ByteOffset >= 0) {
        ov.Offset = (DWORD)*ByteOffset;
        ov.OffsetHigh = (DWORD)(*ByteOffset >> 32);
        pov = &ov;
    }
    DWORD got = 0;
    nv2a_host_cpu_access(xaddr(Buffer), Length, true);
    BOOL ok = ReadFile(h, Buffer, Length, &got, pov);
    uint32_t st = X_STATUS_SUCCESS;
    if (!ok) st = win32_to_status(GetLastError());
    else if (got == 0 && Length) st = X_STATUS_END_OF_FILE;
    LOGF("NtReadFile %08x off=%lld len=%x -> %x st=%08x%s", FileHandle, ByteOffset ? *ByteOffset : -1, Length, got, st,
         f && f->aio && pov ? " (async)" : "");
    {
        // debug: XB_READ_STACK=<byte offset> logs the game's call stack for reads at that offset
        static const char *e = getenv("XB_READ_STACK");
        if (e && ByteOffset && *ByteOffset == _atoi64(e)) {
            uint32_t marker;
            logf("NtReadFile at %lld: event %08x apc %08x ctx %08x", *ByteOffset, Event, ApcRoutine, ApcContext);
            debug_log_guest_stack(&marker, 12);
        }
    }
    if (f && f->aio && pov) {
        // asynchronous handle: report the read as in flight and complete it shortly (see defer_completion)
        if (Event) ResetEvent(xptr<void>(Event));
        IoStatusBlock->Status = X_STATUS_PENDING;
        IoStatusBlock->Information = 0;
        defer_completion(f->aio, IoStatusBlock, st, got, Event, ApcRoutine, ApcContext);
        return X_STATUS_PENDING;
    }
    IoStatusBlock->Status = st;
    IoStatusBlock->Information = got;
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return st;
}

KEXPORT uint32_t xk_NtWriteFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
                                X_IO_STATUS_BLOCK *IoStatusBlock, void *Buffer, uint32_t Length, int64_t *ByteOffset) {
    XFile *f = get_file(FileHandle);
    HANDLE h = f ? f->h : xptr<void>(FileHandle);
    OVERLAPPED ov = {};
    OVERLAPPED *pov = nullptr;
    if (ByteOffset && *ByteOffset >= 0) {
        ov.Offset = (DWORD)*ByteOffset;
        ov.OffsetHigh = (DWORD)(*ByteOffset >> 32);
        pov = &ov;
    }
    DWORD put = 0;
    nv2a_host_cpu_access(xaddr(Buffer), Length, false);
    BOOL ok = WriteFile(h, Buffer, Length, &put, pov);
    uint32_t st = ok ? X_STATUS_SUCCESS : win32_to_status(GetLastError());
    IoStatusBlock->Status = st;
    IoStatusBlock->Information = put;
    LOGF("NtWriteFile %08x len=%x -> %x st=%08x", FileHandle, Length, put, st);
    complete_io(Event, ApcRoutine, ApcContext, IoStatusBlock);
    return st;
}

static int64_t ft64(const FILETIME &f) { return ((int64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

struct X_FILE_NETWORK_OPEN_INFORMATION {
    int64_t CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile;
    uint32_t FileAttributes, Reserved;
};

static void fill_netinfo(const WIN32_FILE_ATTRIBUTE_DATA &a, X_FILE_NETWORK_OPEN_INFORMATION *o) {
    o->CreationTime = ft64(a.ftCreationTime);
    o->LastAccessTime = ft64(a.ftLastAccessTime);
    o->LastWriteTime = ft64(a.ftLastWriteTime);
    o->ChangeTime = o->LastWriteTime;
    o->EndOfFile = ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    o->AllocationSize = (o->EndOfFile + 0x3FFF) & ~0x3FFFll;
    o->FileAttributes = a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
    if (!o->FileAttributes) o->FileAttributes = FILE_ATTRIBUTE_NORMAL;
}

KEXPORT uint32_t xk_NtQueryFullAttributesFile(X_OBJECT_ATTRIBUTES *ObjectAttributes, X_FILE_NETWORK_OPEN_INFORMATION *FileInformation) {
    std::string name = object_name(ObjectAttributes), xpath, hpath;
    if (!resolve_path(name, xpath, hpath)) return X_STATUS_OBJECT_PATH_NOT_FOUND;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(hpath.c_str(), GetFileExInfoStandard, &a)) {
        LOGF("NtQueryFullAttributesFile %s -> not found", name.c_str());
        return win32_to_status(GetLastError());
    }
    fill_netinfo(a, FileInformation);
    LOGF("NtQueryFullAttributesFile %s -> ok", name.c_str());
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtQueryInformationFile(uint32_t FileHandle, X_IO_STATUS_BLOCK *IoStatusBlock, uint8_t *FileInformation,
                                           uint32_t Length, uint32_t FileInformationClass) {
    XFile *f = get_file(FileHandle);
    HANDLE h = f ? f->h : xptr<void>(FileHandle);
    BY_HANDLE_FILE_INFORMATION bi;
    GetFileInformationByHandle(h, &bi);
    int64_t size = ((int64_t)bi.nFileSizeHigh << 32) | bi.nFileSizeLow;
    uint32_t info = 0;
    switch (FileInformationClass) {
    case 4: { // FileBasicInformation
        int64_t *t = (int64_t *)FileInformation;
        t[0] = ft64(bi.ftCreationTime);
        t[1] = ft64(bi.ftLastAccessTime);
        t[2] = ft64(bi.ftLastWriteTime);
        t[3] = t[2];
        *(uint32_t *)(FileInformation + 32) = bi.dwFileAttributes;
        info = 40;
        break;
    }
    case 5: { // FileStandardInformation
        int64_t *t = (int64_t *)FileInformation;
        t[0] = (size + 0x3FFF) & ~0x3FFFll;
        t[1] = size;
        *(uint32_t *)(FileInformation + 16) = 1;
        FileInformation[20] = 0;
        FileInformation[21] = (bi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        info = 24;
        break;
    }
    case 14: { // FilePositionInformation
        LARGE_INTEGER z = {}, pos;
        SetFilePointerEx(h, z, &pos, FILE_CURRENT);
        *(int64_t *)FileInformation = pos.QuadPart;
        info = 8;
        break;
    }
    case 34: { // FileNetworkOpenInformation
        WIN32_FILE_ATTRIBUTE_DATA a = {bi.dwFileAttributes, bi.ftCreationTime, bi.ftLastAccessTime, bi.ftLastWriteTime, bi.nFileSizeHigh, bi.nFileSizeLow};
        fill_netinfo(a, (X_FILE_NETWORK_OPEN_INFORMATION *)FileInformation);
        info = 56;
        break;
    }
    case 20: // FileEndOfFileInformation
    case 19: // FileAllocationInformation
        *(int64_t *)FileInformation = size;
        info = 8;
        break;
    case 16: // FileModeInformation
        *(uint32_t *)FileInformation = 0x20; // FILE_SYNCHRONOUS_IO_NONALERT
        info = 4;
        break;
    case 17: // FileAlignmentInformation
        *(uint32_t *)FileInformation = 0;
        info = 4;
        break;
    default:
        logf("NtQueryInformationFile: unhandled class %u", FileInformationClass);
        IoStatusBlock->Status = X_STATUS_INVALID_PARAMETER;
        return X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = info;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtSetInformationFile(uint32_t FileHandle, X_IO_STATUS_BLOCK *IoStatusBlock, uint8_t *FileInformation,
                                         uint32_t Length, uint32_t FileInformationClass) {
    XFile *f = get_file(FileHandle);
    HANDLE h = f ? f->h : xptr<void>(FileHandle);
    uint32_t st = X_STATUS_SUCCESS;
    switch (FileInformationClass) {
    case 14: { // FilePositionInformation
        LARGE_INTEGER pos;
        pos.QuadPart = *(int64_t *)FileInformation;
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) st = X_STATUS_INVALID_PARAMETER;
        break;
    }
    case 20:   // FileEndOfFileInformation
    case 19: { // FileAllocationInformation
        LARGE_INTEGER cur = {}, z = {}, pos;
        SetFilePointerEx(h, z, &cur, FILE_CURRENT);
        pos.QuadPart = *(int64_t *)FileInformation;
        if (FileInformationClass == 19) {
            // Allocation hint only: never truncate below the current end of file
            LARGE_INTEGER sz;
            GetFileSizeEx(h, &sz);
            if (pos.QuadPart <= sz.QuadPart) break;
        }
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        if (!SetEndOfFile(h)) st = win32_to_status(GetLastError());
        SetFilePointerEx(h, cur, nullptr, FILE_BEGIN);
        break;
    }
    case 13: { // FileDispositionInformation
        FILE_DISPOSITION_INFO di = {FileInformation[0] != 0};
        if (!SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof di)) st = win32_to_status(GetLastError());
        break;
    }
    case 4: { // FileBasicInformation
        FILE_BASIC_INFO bi;
        memcpy(&bi, FileInformation, 36);
        bi.FileAttributes = *(uint32_t *)(FileInformation + 32);
        SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof bi);
        break;
    }
    case 10: { // FileRenameInformation {BOOLEAN Replace; HANDLE Root; OBJECT_STRING Name}
        X_OBJECT_ATTRIBUTES oa = {*(uint32_t *)(FileInformation + 4), xaddr(FileInformation + 8), 0};
        std::string name = object_name(&oa), xpath, hpath;
        if (!f || !resolve_path(name, xpath, hpath)) {
            st = X_STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }
        // Must close our handle to rename on the host; reopen afterwards
        CloseHandle(f->h);
        if (!MoveFileExA(f->host_path.c_str(), hpath.c_str(), FileInformation[0] ? MOVEFILE_REPLACE_EXISTING : 0))
            st = win32_to_status(GetLastError());
        else {
            f->host_path = hpath;
            f->xbox_path = xpath;
        }
        f->h = CreateFileA(f->host_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, f->is_dir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
        // Handle value must stay stable for the Xbox side
        EnterCriticalSection(&g_io_lock);
        g_files.erase(FileHandle);
        HANDLE dup = xptr<void>(FileHandle);
        (void)dup;
        g_files[FileHandle] = f;
        LeaveCriticalSection(&g_io_lock);
        break;
    }
    default:
        logf("NtSetInformationFile: unhandled class %u", FileInformationClass);
        st = X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = st;
    IoStatusBlock->Information = 0;
    return st;
}

static bool mask_match(const char *mask, const char *name) {
    // FATX/NT wildcard: * and ?
    if (!*mask) return !*name;
    if (*mask == '*') {
        for (const char *p = name;; p++) {
            if (mask_match(mask + 1, p)) return true;
            if (!*p) return false;
        }
    }
    if (!*name) return false;
    if (*mask != '?' && tolower((unsigned char)*mask) != tolower((unsigned char)*name)) return false;
    return mask_match(mask + 1, name + 1);
}

KEXPORT uint32_t xk_NtQueryDirectoryFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
                                         X_IO_STATUS_BLOCK *IoStatusBlock, uint8_t *FileInformation, uint32_t Length,
                                         uint32_t FileInformationClass, X_ANSI_STRING *FileMask, uint32_t RestartScan) {
    XFile *f = get_file(FileHandle);
    if (!f || !f->is_dir) return X_STATUS_INVALID_HANDLE;
    if (FileMask && FileMask->Length) f->find_mask = ansi(FileMask);
    else if (f->find == INVALID_HANDLE_VALUE && f->find_mask.empty()) f->find_mask = "*";
    if ((RestartScan & 0xFF) && f->find != INVALID_HANDLE_VALUE) {
        FindClose(f->find);
        f->find = INVALID_HANDLE_VALUE;
    }
    bool first = f->find == INVALID_HANDLE_VALUE;
    for (;;) {
        if (f->find == INVALID_HANDLE_VALUE) {
            std::string pat = f->host_path + "\\*";
            f->find = FindFirstFileA(pat.c_str(), &f->fd);
            if (f->find == INVALID_HANDLE_VALUE) break;
            f->find_pending = true;
        } else if (!f->find_pending) {
            if (!FindNextFileA(f->find, &f->fd)) break;
            f->find_pending = true;
        }
        f->find_pending = false;
        if (!strcmp(f->fd.cFileName, ".") || !strcmp(f->fd.cFileName, "..")) continue;
        if (!mask_match(f->find_mask.c_str(), f->fd.cFileName)) continue;
        // FILE_DIRECTORY_INFORMATION (ANSI file name on Xbox)
        uint32_t nlen = (uint32_t)strlen(f->fd.cFileName);
        if (Length < 0x40 + nlen) {
            f->find_pending = true;
            return X_STATUS_BUFFER_OVERFLOW;
        }
        memset(FileInformation, 0, 0x40);
        int64_t *t = (int64_t *)(FileInformation + 8);
        t[0] = ft64(f->fd.ftCreationTime);
        t[1] = ft64(f->fd.ftLastAccessTime);
        t[2] = ft64(f->fd.ftLastWriteTime);
        t[3] = t[2];
        t[4] = ((int64_t)f->fd.nFileSizeHigh << 32) | f->fd.nFileSizeLow;
        t[5] = (t[4] + 0x3FFF) & ~0x3FFFll;
        *(uint32_t *)(FileInformation + 0x38) = f->fd.dwFileAttributes & 0x37 ? f->fd.dwFileAttributes & 0x37 : FILE_ATTRIBUTE_NORMAL;
        *(uint32_t *)(FileInformation + 0x3C) = nlen;
        memcpy(FileInformation + 0x40, f->fd.cFileName, nlen);
        IoStatusBlock->Status = X_STATUS_SUCCESS;
        IoStatusBlock->Information = 0x40 + nlen;
        LOGF("NtQueryDirectoryFile %s [%s] -> %s", f->host_path.c_str(), f->find_mask.c_str(), f->fd.cFileName);
        if (Event) SetEvent(xptr<void>(Event));
        return X_STATUS_SUCCESS;
    }
    uint32_t st = first ? X_STATUS_NO_SUCH_FILE : X_STATUS_NO_MORE_FILES;
    IoStatusBlock->Status = st;
    IoStatusBlock->Information = 0;
    return st;
}

KEXPORT uint32_t xk_NtQueryVolumeInformationFile(uint32_t FileHandle, X_IO_STATUS_BLOCK *IoStatusBlock, uint8_t *FsInformation,
                                                 uint32_t Length, uint32_t FsInformationClass) {
    uint32_t info = 0;
    switch (FsInformationClass) {
    case 3: { // FileFsSizeInformation
        int64_t *t = (int64_t *)FsInformation;
        t[0] = 0x80000;   // total clusters (8 GB with 16K clusters)
        t[1] = 0x40000;   // free clusters
        *(uint32_t *)(FsInformation + 16) = 32;  // sectors per cluster
        *(uint32_t *)(FsInformation + 20) = 512; // bytes per sector
        info = 24;
        break;
    }
    case 4: // FileFsDeviceInformation
        *(uint32_t *)FsInformation = 7; // FILE_DEVICE_DISK
        *(uint32_t *)(FsInformation + 4) = 0;
        info = 8;
        break;
    case 1: // FileFsVolumeInformation
        memset(FsInformation, 0, Length < 24 ? Length : 24);
        info = Length < 24 ? Length : 24;
        break;
    case 5: { // FileFsAttributeInformation
        memset(FsInformation, 0, 12);
        *(uint32_t *)(FsInformation + 4) = 255;
        *(uint32_t *)(FsInformation + 8) = 4;
        if (Length >= 16) memcpy(FsInformation + 12, "FATX", 4);
        info = 16;
        break;
    }
    default:
        logf("NtQueryVolumeInformationFile: class %u", FsInformationClass);
        return X_STATUS_INVALID_PARAMETER;
    }
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = info;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtFlushBuffersFile(uint32_t FileHandle, X_IO_STATUS_BLOCK *IoStatusBlock) {
    XFile *f = get_file(FileHandle);
    FlushFileBuffers(f ? f->h : xptr<void>(FileHandle));
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtDeleteFile(X_OBJECT_ATTRIBUTES *ObjectAttributes) {
    std::string name = object_name(ObjectAttributes), xpath, hpath;
    if (!resolve_path(name, xpath, hpath)) return X_STATUS_OBJECT_PATH_NOT_FOUND;
    DWORD a = GetFileAttributesA(hpath.c_str());
    BOOL ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryA(hpath.c_str()) : DeleteFileA(hpath.c_str());
    LOGF("NtDeleteFile %s -> %d", hpath.c_str(), ok);
    return ok ? X_STATUS_SUCCESS : win32_to_status(GetLastError());
}

KEXPORT uint32_t xk_NtFsControlFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
                                    X_IO_STATUS_BLOCK *IoStatusBlock, uint32_t FsControlCode, void *InputBuffer,
                                    uint32_t InputBufferLength, void *OutputBuffer, uint32_t OutputBufferLength) {
    logf("NtFsControlFile(%08x, code %08x)", FileHandle, FsControlCode);
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = 0;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtDeviceIoControlFile(uint32_t FileHandle, uint32_t Event, uint32_t ApcRoutine, uint32_t ApcContext,
                                          X_IO_STATUS_BLOCK *IoStatusBlock, uint32_t IoControlCode, void *InputBuffer,
                                          uint32_t InputBufferLength, uint8_t *OutputBuffer, uint32_t OutputBufferLength) {
    logf("NtDeviceIoControlFile(%08x, code %08x, in %u, out %u)", FileHandle, IoControlCode, InputBufferLength, OutputBufferLength);
    uint32_t info = 0;
    if (IoControlCode == 0x70000 && OutputBufferLength >= 24) { // IOCTL_DISK_GET_DRIVE_GEOMETRY
        memset(OutputBuffer, 0, 24);
        *(int64_t *)OutputBuffer = 0x1000;           // cylinders
        *(uint32_t *)(OutputBuffer + 8) = 12;        // FixedMedia
        *(uint32_t *)(OutputBuffer + 12) = 255;      // tracks/cyl
        *(uint32_t *)(OutputBuffer + 16) = 63;       // sectors/track
        *(uint32_t *)(OutputBuffer + 20) = 512;      // bytes/sector
        info = 24;
    } else if (OutputBuffer && OutputBufferLength) {
        memset(OutputBuffer, 0, OutputBufferLength);
    }
    IoStatusBlock->Status = X_STATUS_SUCCESS;
    IoStatusBlock->Information = info;
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------- symbolic links
KEXPORT uint32_t xk_IoCreateSymbolicLink(X_ANSI_STRING *SymbolicLinkName, X_ANSI_STRING *DeviceName) {
    std::string l = lower(ansi(SymbolicLinkName)), t = ansi(DeviceName);
    logf("IoCreateSymbolicLink %s -> %s", l.c_str(), t.c_str());
    EnterCriticalSection(&g_io_lock);
    g_symlinks[l] = t;
    LeaveCriticalSection(&g_io_lock);
    // Make sure the target directory exists on the host
    std::string xp, hp;
    if (resolve_path(t, xp, hp)) {
        for (size_t i = strlen(g_save_dir) + 1; i <= hp.size(); i++)
            if (i == hp.size() || hp[i] == '\\') CreateDirectoryA(hp.substr(0, i).c_str(), nullptr);
    }
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_IoDeleteSymbolicLink(X_ANSI_STRING *SymbolicLinkName) {
    std::string l = lower(ansi(SymbolicLinkName));
    logf("IoDeleteSymbolicLink %s", l.c_str());
    EnterCriticalSection(&g_io_lock);
    bool had = g_symlinks.erase(l) != 0;
    LeaveCriticalSection(&g_io_lock);
    return had ? X_STATUS_SUCCESS : X_STATUS_OBJECT_NAME_NOT_FOUND;
}

KEXPORT uint32_t xk_NtOpenSymbolicLinkObject(uint32_t *LinkHandle, X_OBJECT_ATTRIBUTES *ObjectAttributes) {
    std::string name = lower(object_name(ObjectAttributes));
    EnterCriticalSection(&g_io_lock);
    auto it = g_symlinks.find(name);
    if (it == g_symlinks.end()) {
        LeaveCriticalSection(&g_io_lock);
        logf("NtOpenSymbolicLinkObject %s -> not found", name.c_str());
        return X_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    XFile *f = new XFile;
    f->is_symlink = true;
    f->xbox_path = name;
    f->link_target = it->second;
    uint32_t h = new_fake_handle();
    f->h = xptr<void>(h);
    g_files[h] = f;
    LeaveCriticalSection(&g_io_lock);
    *LinkHandle = h;
    return X_STATUS_SUCCESS;
}

KEXPORT uint32_t xk_NtQuerySymbolicLinkObject(uint32_t LinkHandle, X_ANSI_STRING *LinkTarget, uint32_t *ReturnedLength) {
    XFile *f = get_file(LinkHandle);
    if (!f || !f->is_symlink) return X_STATUS_INVALID_HANDLE;
    uint32_t n = (uint32_t)f->link_target.size();
    if (ReturnedLength) *ReturnedLength = n;
    if (LinkTarget->MaximumLength < n) return X_STATUS_BUFFER_TOO_SMALL;
    memcpy(xptr<char>(LinkTarget->Buffer), f->link_target.data(), n);
    LinkTarget->Length = (uint16_t)n;
    return X_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------- driver model (unused by HLE)
KEXPORT uint32_t xk_IoCreateDevice(uint32_t DriverObject, uint32_t DeviceExtensionSize, uint32_t DeviceName,
                                   uint32_t DeviceType, uint32_t Exclusive, uint32_t *DeviceObject) {
    // Allocate a zeroed device object large enough for the extension; drivers are never called.
    uint8_t *d = (uint8_t *)calloc(1, 0x100 + DeviceExtensionSize);
    *(uint32_t *)(d + 0x18) = xaddr(d + 0x100); // DeviceExtension
    *DeviceObject = xaddr(d);
    logf("IoCreateDevice(type %x, ext %x) -> %p", DeviceType, DeviceExtensionSize, d);
    return X_STATUS_SUCCESS;
}
