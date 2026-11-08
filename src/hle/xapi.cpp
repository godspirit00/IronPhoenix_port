// Misc XAPI replacements (utility drive, network, ...)
#include "hle.h"
#include "../kernel/kernel.h"
#include "../cpu/patch.h"
#include "../symbols.gen.h"
#include <windows.h>
#include <string>

KEXPORT uint32_t xk_IoCreateSymbolicLink(X_ANSI_STRING *SymbolicLinkName, X_ANSI_STRING *DeviceName);

static void delete_tree(const std::string &dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string p = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            delete_tree(p);
            RemoveDirectoryA(p.c_str());
        } else {
            DeleteFileA(p.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// BOOL XMountUtilityDrive(BOOL fFormatClean): the cache partition for this title becomes Z:
static XSTDCALL uint32_t hle_XMountUtilityDrive(uint32_t fFormatClean) {
    std::string host = std::string(g_save_dir) + "\\Partition3";
    CreateDirectoryA(host.c_str(), nullptr);
    if (fFormatClean) delete_tree(host);
    static char link[] = "\\??\\Z:", target[] = "\\Device\\Harddisk0\\Partition3";
    X_ANSI_STRING l = {sizeof(link) - 1, sizeof(link), xaddr(link)};
    X_ANSI_STRING t = {sizeof(target) - 1, sizeof(target), xaddr(target)};
    xk_IoCreateSymbolicLink(&l, &t);
    logf("XMountUtilityDrive(format=%u) -> Z: = %s", fFormatClean, host.c_str());
    return 1;
}

// Xbox Live is gone. Behave like a console with the network cable unplugged: the stack
// starts fine, the link is down and creating sockets fails with WSAENETDOWN.
static XSTDCALL int hle_XNetStartup(uint32_t params) {
    logf("XNetStartup -> ok (no link)");
    return 0;
}
struct X_WSADATA {
    uint16_t wVersion, wHighVersion;
    char szDescription[257];
    char szSystemStatus[129];
    uint16_t iMaxSockets, iMaxUdpDg;
    uint32_t lpVendorInfo;
};
static XSTDCALL int hle_WSAStartup(uint32_t ver, X_WSADATA *data) {
    if (data) {
        memset(data, 0, sizeof *data);
        data->wVersion = data->wHighVersion = 0x0202;
        strcpy(data->szDescription, "XNet");
        data->iMaxSockets = 32;
    }
    logf("WSAStartup(%x) -> ok", ver);
    return 0;
}
static uint32_t g_wsa_error;
static XSTDCALL uint32_t hle_socket(int af, int type, int protocol) {
    g_wsa_error = 10050; // WSAENETDOWN
    return 0xFFFFFFFF;   // INVALID_SOCKET
}
static XSTDCALL int hle_sock_fail3(uint32_t s, uint32_t a, uint32_t b) {
    g_wsa_error = 10038; // WSAENOTSOCK
    return -1;
}
static XSTDCALL uint32_t hle_XNetGetEthernetLinkStatus() { return 0; } // cable unplugged

void hle_xapi_install() {
    HOOK(SYM_XAPILIB_XMountUtilityDrive, hle_XMountUtilityDrive);
    // Networking: the real XNet stack runs against the emulated NIC (src/net, link down).
    (void)hle_XNetStartup; (void)hle_WSAStartup; (void)hle_XNetGetEthernetLinkStatus;
    (void)hle_socket; (void)hle_sock_fail3;
}
