// SPDX-License-Identifier: GPL-2.0-or-later
// HostLock and HostCond (src/host.h).
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL host_lock line %d: %s\n",__LINE__,#x);_exit(1);}}while(0)

static HostLock lock = HOST_LOCK_INIT;
static volatile int stage;
static void wait_stage(int n) { while (__atomic_load_n(&stage, __ATOMIC_ACQUIRE) < n) usleep(100); }
static void set_stage(int n) { __atomic_store_n(&stage, n, __ATOMIC_RELEASE); }

// A reader takes the lock again while a writer waits: it must not be held back.
static void *writer_waits(void *arg) {
    (void)arg; set_stage(1); host_lock(&lock); CHECK(stage == 3); host_unlock(&lock); return NULL;
}

// Readers check that the two values match; writers change both.
static HostLock pair_lock = HOST_LOCK_INIT;
static long first, second, reads;
static void *pair_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 20000; ++i) { host_lock(&pair_lock); ++first; ++second; host_unlock(&pair_lock); }
    return NULL;
}
static void *pair_reader(void *arg) {
    (void)arg;
    for (int i = 0; i < 20000; ++i) {
        host_lock_shared(&pair_lock); CHECK(first == second); host_unlock_shared(&pair_lock);
        __atomic_add_fetch(&reads, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

// Producer/consumer on a condition variable, the way kernel_sync.c uses it.
static HostLock queue_lock = HOST_LOCK_INIT;
static HostCond queue_changed;
static int queued, consumed;
static void *consumer(void *arg) {
    (void)arg;
    for (;;) {
        host_lock(&queue_lock);
        while (!queued) host_cond_wait(&queue_changed, &queue_lock, HOST_WAIT_FOREVER, 0);
        int item = queued--; ++consumed;
        host_unlock(&queue_lock);
        if (item < 0) return NULL;
    }
}

// Shared-mode waiters, woken together.
static int gate_open, gate_passed;
static void *gate_waiter(void *arg) {
    (void)arg;
    host_lock_shared(&queue_lock);
    while (!__atomic_load_n(&gate_open, __ATOMIC_ACQUIRE)) host_cond_wait(&queue_changed, &queue_lock, HOST_WAIT_FOREVER, 1);
    __atomic_add_fetch(&gate_passed, 1, __ATOMIC_RELAXED);
    host_unlock_shared(&queue_lock);
    return NULL;
}

int main(void) {
    // Recursive shared acquisition with a writer waiting.
    host_lock_shared(&lock);
    pthread_t w; CHECK(pthread_create(&w, NULL, writer_waits, NULL) == 0);
    wait_stage(1); usleep(20000);
    host_lock_shared(&lock); set_stage(2);
    host_unlock_shared(&lock); set_stage(3); host_unlock_shared(&lock);
    pthread_join(w, NULL);

    // Readers never see a half-made change.
    pthread_t t[8];
    for (int i = 0; i < 8; ++i) CHECK(pthread_create(&t[i], NULL, i < 3 ? pair_writer : pair_reader, NULL) == 0);
    for (int i = 0; i < 8; ++i) pthread_join(t[i], NULL);
    CHECK(first == 60000 && second == 60000 && reads == 100000);

    // A timed wait with nothing to wake it times out and says so.
    host_cond_init(&queue_changed);
    host_lock(&queue_lock);
    CHECK(!host_cond_wait(&queue_changed, &queue_lock, 5000, 0));
    host_unlock(&queue_lock);

    // Every item reaches a consumer; the -1 item stops them.
    for (int i = 0; i < 4; ++i) CHECK(pthread_create(&t[i], NULL, consumer, NULL) == 0);
    int sent = 0;
    for (int i = 0; i < 20000; ++i) {
        host_lock(&queue_lock);
        if (queued < 4) { ++queued; ++sent; host_cond_signal(&queue_changed); }
        host_unlock(&queue_lock);
    }
    for (;;) { host_lock(&queue_lock); int left = queued; host_unlock(&queue_lock); if (!left) break; usleep(100); }
    host_lock(&queue_lock); CHECK(consumed == sent); queued = -4; host_cond_broadcast(&queue_changed); host_unlock(&queue_lock);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);

    // Shared waiters all pass once the gate opens.
    for (int i = 0; i < 4; ++i) CHECK(pthread_create(&t[i], NULL, gate_waiter, NULL) == 0);
    usleep(20000);
    host_lock(&queue_lock); __atomic_store_n(&gate_open, 1, __ATOMIC_RELEASE); host_cond_broadcast(&queue_changed); host_unlock(&queue_lock);
    for (int i = 0; i < 4; ++i) pthread_join(t[i], NULL);
    CHECK(gate_passed == 4);
    puts("PASS: recursive reads past a waiting writer, reader/writer exclusion, timed wait, producer/consumer, shared waiters.");
    return 0;
}
