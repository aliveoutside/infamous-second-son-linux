// SPDX-License-Identifier: GPL-2.0-or-later
// Host primitives the runtime files share: a reader-writer lock, a condition variable, the thread
// id and a millisecond clock, on Linux futexes.
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static inline long host_futex(uint32_t *word, int op, uint32_t value, const struct timespec *deadline) {
    // FUTEX_WAIT_BITSET takes an absolute CLOCK_MONOTONIC deadline (NULL: none).
    return syscall(SYS_futex, word, op | FUTEX_PRIVATE_FLAG, value, deadline, NULL, FUTEX_BITSET_MATCH_ANY);
}

// A reader-writer lock; taking a free one is one atomic. Lock word: bit 31 a writer holds it,
// bit 30 someone sleeps on it, the rest the reader count. Readers are never held back by waiting
// writers: shared acquisition may recurse. Whoever clears the sleeper bit wakes every sleeper, so
// the bit is set whenever anyone sleeps.
#define HOST_LOCK_WRITER 0x80000000u
#define HOST_LOCK_SLEEPERS 0x40000000u
typedef struct { uint32_t state; } HostLock;
#define HOST_LOCK_INIT {0}
static inline void host_lock_init(HostLock *l) { l->state = 0; }
// Spins a little, then sleeps while the state still shows a holder in `blocked_mask`.
static inline void host_lock_wait(HostLock *l, uint32_t blocked_mask, int *spins) {
    uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
    if (!(s & blocked_mask)) return;
    if (*spins < 100) { ++*spins; __builtin_ia32_pause(); return; }
    if (!(s & HOST_LOCK_SLEEPERS) &&
        !__atomic_compare_exchange_n(&l->state, &s, s | HOST_LOCK_SLEEPERS, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    host_futex(&l->state, FUTEX_WAIT_BITSET, s | HOST_LOCK_SLEEPERS, NULL);
}
static inline void host_lock_wake(HostLock *l) { host_futex(&l->state, FUTEX_WAKE, INT_MAX, NULL); }
static inline void host_lock_shared(HostLock *l) {
    for (int spins = 0;;) {
        uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & HOST_LOCK_WRITER) &&
            __atomic_compare_exchange_n(&l->state, &s, s + 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        host_lock_wait(l, HOST_LOCK_WRITER, &spins);
    }
}
static inline void host_unlock_shared(HostLock *l) {
    uint32_t s = __atomic_sub_fetch(&l->state, 1, __ATOMIC_RELEASE);
    // The last reader left and someone sleeps (a writer): clear the bit and wake them.
    if (s == HOST_LOCK_SLEEPERS &&
        __atomic_compare_exchange_n(&l->state, &s, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) host_lock_wake(l);
}
static inline void host_lock(HostLock *l) {
    for (int spins = 0;;) {
        uint32_t s = __atomic_load_n(&l->state, __ATOMIC_RELAXED);
        if (!(s & ~HOST_LOCK_SLEEPERS) &&
            __atomic_compare_exchange_n(&l->state, &s, s | HOST_LOCK_WRITER, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        host_lock_wait(l, ~HOST_LOCK_SLEEPERS, &spins);
    }
}
static inline void host_unlock(HostLock *l) {
    if (__atomic_exchange_n(&l->state, 0, __ATOMIC_RELEASE) & HOST_LOCK_SLEEPERS) host_lock_wake(l);
}

// A condition variable used with HostLock. A waiter reads `seq` before it lets go of the lock and
// sleeps while `seq` keeps that value, so a wake sent any time after the release (it bumps `seq`)
// is never lost.
typedef struct { uint32_t seq, sleepers; } HostCond;
#define HOST_COND_INIT {0, 0}
#define HOST_WAIT_FOREVER UINT64_MAX
static inline void host_cond_init(HostCond *c) { c->seq = c->sleepers = 0; }
static inline void host_cond_wake(HostCond *c, int count) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->sleepers, __ATOMIC_SEQ_CST)) host_futex(&c->seq, FUTEX_WAKE, (uint32_t)count, NULL);
}
static inline void host_cond_signal(HostCond *c) { host_cond_wake(c, 1); }
static inline void host_cond_broadcast(HostCond *c) { host_cond_wake(c, INT_MAX); }
// Waits up to `timeout_us` (HOST_WAIT_FOREVER: no limit) with `l` held, shared when `shared`.
// Returns 1 when woken, 0 when the time ran out; either way `l` is held again on return.
static inline int host_cond_wait(HostCond *c, HostLock *l, uint64_t timeout_us, int shared) {
    struct timespec deadline, *until = NULL;
    if (timeout_us != HOST_WAIT_FOREVER) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(timeout_us / 1000000);
        deadline.tv_nsec += (long)(timeout_us % 1000000) * 1000L;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
        until = &deadline;
    }
    const uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&c->sleepers, 1, __ATOMIC_SEQ_CST);
    if (shared) host_unlock_shared(l); else host_unlock(l);
    int woken = 1;
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seq)
        if (host_futex(&c->seq, FUTEX_WAIT_BITSET, seq, until) && errno == ETIMEDOUT) { woken = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) != seq; break; }
    __atomic_sub_fetch(&c->sleepers, 1, __ATOMIC_SEQ_CST);
    if (shared) host_lock_shared(l); else host_lock(l);
    return woken;
}

// The kernel's id of the calling thread, read once per thread.
static inline uint32_t host_thread_id(void) {
    static __thread uint32_t id;
    if (!id) id = (uint32_t)syscall(SYS_gettid);
    return id;
}
static inline uint64_t host_monotonic_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
