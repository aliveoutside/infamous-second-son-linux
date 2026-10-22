// SPDX-License-Identifier: GPL-2.0-or-later
// Linux stand-ins for the few Win32 calls the renderer glue makes. Force-included on Linux, so
// the Windows sources compile unchanged: <windows.h> and <intrin.h> resolve to the wrappers next to this file.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define __declspec(x) __attribute__((visibility("default")))
#define WINAPI
#define CALLBACK
#define __stdcall
#define MAX_PATH 4096
#define INFINITE 0xffffffffu

using DWORD = uint32_t;
using ULONG = uint32_t;
using UINT = uint32_t;
using USHORT = uint16_t;
using LONG = int32_t;
using LONGLONG = int64_t;
using BOOL = int;
using HANDLE = void*;
using HMODULE = void*;
using LPCSTR = const char*;
constexpr BOOL TRUE = 1, FALSE = 0;

enum : DWORD {
    MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000,
    PAGE_NOACCESS = 1, PAGE_READONLY = 2, PAGE_READWRITE = 4, PAGE_EXECUTE_READ = 0x20,
    PAGE_EXECUTE_READWRITE = 0x40,
};

union LARGE_INTEGER { struct { uint32_t LowPart; int32_t HighPart; }; int64_t QuadPart; };

inline int win32_prot(DWORD p) {
    switch (p) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE | PROT_EXEC;
    }
}
inline void* VirtualAlloc(void* at, size_t size, DWORD, DWORD protect) {
    void* p = mmap(at, size, win32_prot(protect), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
inline BOOL VirtualProtect(void* at, size_t size, DWORD protect, DWORD* old) {
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    const uintptr_t begin = uintptr_t(at) & ~(page - 1), end = (uintptr_t(at) + size + page - 1) & ~(page - 1);
    if (old) *old = PAGE_EXECUTE_READ; // callers restore the code pages they patched
    return mprotect(reinterpret_cast<void*>(begin), end - begin, win32_prot(protect)) == 0;
}
inline BOOL FlushInstructionCache(HANDLE, const void*, size_t) { return TRUE; }
inline HANDLE GetCurrentProcess() { return nullptr; }
inline HANDLE GetCurrentThread() { return nullptr; }
inline DWORD GetCurrentProcessId() { return DWORD(getpid()); }
inline DWORD GetCurrentThreadId() { return DWORD(syscall(SYS_gettid)); }
inline BOOL SwitchToThread() { return sched_yield() == 0; }
inline void Sleep(DWORD ms) { usleep(ms * 1000u); }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* t) {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec; return TRUE;
}
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000; return TRUE; }
inline long SetThreadDescription(HANDLE, const wchar_t* name) {
    std::string narrow;
    for (; *name && narrow.size() < 15; ++name) narrow.push_back(char(*name));
    prctl(PR_SET_NAME, narrow.c_str());
    return 0;
}
inline BOOL TerminateProcess(HANDLE, unsigned code) { _exit(int(code)); }

#include <cerrno>
#include <ctime>
inline DWORD GetLastError() { return DWORD(errno); }
#define _ReturnAddress() __builtin_return_address(0)
#define _AddressOfReturnAddress() __builtin_frame_address(0)
inline int localtime_s(tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : errno; }
constexpr DWORD GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS = 4, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT = 2;
inline BOOL GetModuleHandleExA(DWORD, LPCSTR address, HMODULE* module) {
    Dl_info info;
    if (!dladdr(address, &info)) return FALSE;
    *module = info.dli_fbase;
    return TRUE;
}

#include <cstdio>
#include <cstdlib>
#define _fseeki64 fseeko64
#define _ftelli64 ftello64
inline void* _aligned_malloc(size_t size, size_t alignment) { return aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1)); }
inline void _aligned_free(void* p) { free(p); }
inline uint64_t GetTickCount64() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
