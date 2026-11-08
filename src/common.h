// Iron Phoenix native runtime - common definitions
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

void log_init(const char *path);
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_backtrace(const char *tag);
[[noreturn]] void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Verbose channel toggles (set from config / command line)
extern bool g_log_kernel;   // log every kernel call made through implemented exports
extern bool g_log_files;    // log file system activity

#define LOGK(...) do { if (g_log_kernel) logf(__VA_ARGS__); } while (0)
#define LOGF(...) do { if (g_log_files) logf(__VA_ARGS__); } while (0)

template <typename T> static inline T *xptr(uint32_t a) { return reinterpret_cast<T *>(static_cast<uintptr_t>(a)); }
static inline uint32_t xaddr(const void *p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }

// Paths configured at startup
extern char g_game_dir[512];   // directory holding default.xbe and game data (D:\)
extern char g_save_dir[512];   // directory emulating the HDD partitions (E:\, T:\, U:\, cache)
