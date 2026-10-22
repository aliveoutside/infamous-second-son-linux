// SPDX-License-Identifier: GPL-2.0-or-later
#include "../runtime.h"
#include <x86intrin.h>
#include <stdio.h>
#include <string.h>
typedef struct { int64_t seconds, nanoseconds; } TimeSpec;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static uint64_t tsc_frequency, initial_tsc;
static uint64_t tick(void) { _mm_lfence(); uint64_t t = __rdtsc(); _mm_lfence(); return t; }
static uint64_t raw_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void initialize(void) {
    uint64_t begin = raw_ns(), first = tick();
    struct timespec nap = {0, 40000000}; nanosleep(&nap, NULL);
    uint64_t last = tick(), end = raw_ns();
    tsc_frequency = (uint64_t)((double)(last - first) * 1e9 / (double)(end - begin));
    initial_tsc = tick();
    printf("Calibrated invariant TSC: %llu Hz\n", (unsigned long long)tsc_frequency);
}
static uint64_t GUEST_ABI frequency(void) { pthread_once(&once,initialize); return tsc_frequency; }
static uint64_t GUEST_ABI process_time(void) {
    uint64_t f = frequency(), d = tick()-initial_tsc;
    return (d/f)*1000000 + ((d%f)*1000000)/f;
}
uint64_t runtime_tsc_frequency(void){return frequency();}
uint64_t runtime_process_time_us(void){return process_time();}
static int32_t GUEST_ABI guest_clock_gettime(uint32_t id, TimeSpec *out) {
    if (!out) return (int32_t)UINT32_C(0x80020016);
    struct timespec t;
    if (id == 0 || id == 9 || id == 10 || id == 13) {
        clock_gettime(CLOCK_REALTIME, &t);
        out->seconds = t.tv_sec; out->nanoseconds = id == 13 ? 0 : t.tv_nsec;
        return 0;
    }
    if (id == 4 || id == 5 || id == 7 || id == 8 || id == 11 || id == 12) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        out->seconds = t.tv_sec; out->nanoseconds = t.tv_nsec; return 0;
    }
    return (int32_t)UINT32_C(0x80020016);
}
static int GUEST_ABI posix_clock_gettime(uint32_t id,TimeSpec *out){int32_t r=guest_clock_gettime(id,out);if(r){*runtime_errno()=r&65535;return -1;}return 0;}
static int32_t GUEST_ABI guest_usleep(uint32_t us) {
    if (!us) { sched_yield(); return 0; }
    struct timespec t = {us / 1000000, (long)(us % 1000000) * 1000L};
    while (clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &t) == EINTR) {}
    return 0;
}
uintptr_t time_resolve(const char *identity) {
    if (!strstr(identity,"#libkernel#1#libkernel#257")&&!strstr(identity,"#libScePosix#1#libkernel#257")) return 0;
    const char *api = runtime_api_name(identity);
    if (!strcmp(api,"sceKernelGetTscFrequency")) return (uintptr_t)frequency;
    if (!strcmp(api,"sceKernelGetProcessTime")) return (uintptr_t)process_time;
    if (!strcmp(api,"sceKernelClockGettime")) return (uintptr_t)guest_clock_gettime;
    if (!strcmp(api,"sceKernelUsleep")) return (uintptr_t)guest_usleep;
    if (!strcmp(api,"clock_gettime")||!strcmp(api,"_clock_gettime")) return (uintptr_t)posix_clock_gettime;
    return 0;
}
