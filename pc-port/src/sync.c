// SPDX-License-Identifier: GPL-2.0-or-later
// PS4 pthread mutexes, condition variables and read-write locks on Linux futexes.
// The guest holds pointers to our objects. Every object made here is entered in a set that is read
// without a lock (objects are never freed, so destroyed ones stay readable and are refused by their
// `active` flag), so a lock or unlock is one hash probe plus atomics on the object itself, and
// threads using different mutexes never touch shared state.
#include "runtime.h"
#include <stdio.h>
#include <string.h>

// The first field of every object: what it is, checked after the set says it is ours.
enum { KIND_ATTR = 0x52545441, KIND_MUTEX = 0x5854554d, KIND_COND = 0x444e4f43, KIND_RWLOCK = 0x4b4c5752 };

// ---- the set of our objects ---------------------------------------------------------------------
// Open addressing, at most half full. Readers probe the published table with no lock; a writer
// (registry held) inserts, or copies into a table twice the size and publishes that one. Old tables
// stay allocated, since a reader may still be probing one.
typedef struct { size_t mask; uintptr_t slots[]; } Table;
static Table *table;
static size_t table_count;
static pthread_mutex_t registry = PTHREAD_MUTEX_INITIALIZER; // creating objects only
static size_t slot_of(uintptr_t p, size_t mask) { return (size_t)(((uint64_t)(p >> 4) * UINT64_C(0x9e3779b97f4a7c15)) >> 32) & mask; }
static void table_put(Table *t, uintptr_t p) {
    size_t i = slot_of(p, t->mask);
    while (t->slots[i]) i = (i + 1) & t->mask;
    __atomic_store_n(&t->slots[i], p, __ATOMIC_RELEASE);
}
// Registry held. Returns object, or NULL (and frees it) when the set cannot grow.
static void *remember(void *object) {
    Table *t = table;
    if (!t || (table_count + 1) * 2 > t->mask + 1) {
        const size_t size = t ? (t->mask + 1) * 2 : 1024;
        Table *bigger = calloc(1, sizeof(Table) + size * sizeof(uintptr_t));
        if (!bigger) { free(object); return NULL; }
        bigger->mask = size - 1;
        if (t) for (size_t i = 0; i <= t->mask; ++i) if (t->slots[i]) table_put(bigger, t->slots[i]);
        __atomic_store_n(&table, bigger, __ATOMIC_RELEASE);
        t = bigger;
    }
    table_put(t, (uintptr_t)object); ++table_count; return object;
}
static void *known(const void *p, uint32_t kind) {
    const Table *t = __atomic_load_n(&table, __ATOMIC_ACQUIRE);
    if (!t || !p) return NULL;
    for (size_t i = slot_of((uintptr_t)p, t->mask);; i = (i + 1) & t->mask) {
        const uintptr_t s = __atomic_load_n(&t->slots[i], __ATOMIC_ACQUIRE);
        if (s == (uintptr_t)p) return *(const uint32_t *)p == kind ? (void *)p : NULL;
        if (!s) return NULL;
    }
}
static void *load_handle(void *const *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store_handle(void **p, void *value) { __atomic_store_n(p, value, __ATOMIC_RELEASE); }

// ---- lock words ---------------------------------------------------------------------------------
// A lock word: 0 free, 1 held, 2 held with sleepers. The sections it guards in the game are short,
// so a contended lock spins briefly before it sleeps.
static int word_try(uint32_t *w) { uint32_t c = 0; return __atomic_compare_exchange_n(w, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); }
static void word_lock(uint32_t *w) {
    if (word_try(w)) return;
    for (int spin = 0; spin < 100; ++spin) {
        __builtin_ia32_pause();
        if (!__atomic_load_n(w, __ATOMIC_RELAXED) && word_try(w)) return;
    }
    while (__atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE)) host_futex(w, FUTEX_WAIT_BITSET, 2, NULL);
}
static void word_unlock(uint32_t *w) { if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2) host_futex(w, FUTEX_WAKE, 1, NULL); }

// ---- read-write locks ---------------------------------------------------------------------------
// Each thread's read count is kept so a thread that writes while it reads (or reads while it
// writes) gets EDEADLK instead of hanging.
typedef struct Reader { uint32_t id; unsigned count; struct Reader *next; } Reader;
typedef struct Rwlock { uint32_t kind; int active; pthread_mutex_t lock; pthread_cond_t changed; uint32_t writer; unsigned readers, waiters; Reader *owners; } Rwlock;
static Rwlock *rw_create(void) { // registry held
    Rwlock *r = calloc(1, sizeof(*r)); if (!r) return NULL;
    r->kind = KIND_RWLOCK; r->active = 1;
    pthread_mutex_init(&r->lock, NULL); pthread_cond_init(&r->changed, NULL);
    return remember(r);
}
static int32_t GUEST_ABI rw_init(Rwlock **p,void *attr,const char *name){
    (void)name;if(!p||attr)return (int32_t)UINT32_C(0x80020016);
    pthread_mutex_lock(&registry);Rwlock *r=rw_create();if(r)store_handle((void **)p,r);pthread_mutex_unlock(&registry);
    return r?0:(int32_t)UINT32_C(0x8002000c);
}
static Rwlock *rw_find(Rwlock **p){
    if(!p)return NULL;
    Rwlock *r=load_handle((void **)p);
    if(!r){ // an unset lock is made on first use
        pthread_mutex_lock(&registry);r=*p;if(!r){r=rw_create();if(r)store_handle((void **)p,r);}pthread_mutex_unlock(&registry);
    }
    r=known(r,KIND_RWLOCK);return r&&__atomic_load_n(&r->active,__ATOMIC_ACQUIRE)?r:NULL;
}
static int32_t rw_acquire(Rwlock **p,int writing,int try_only){
    Rwlock *r=rw_find(p);uint32_t tid=host_thread_id();
    if(!r){printf("RW invalid: slot=%p value=%p tid=%u\n",(void *)p,p?(void *)*p:NULL,tid);return (int32_t)UINT32_C(0x80020016);}
    pthread_mutex_lock(&r->lock);
    Reader *owner=r->owners;while(owner&&owner->id!=tid)owner=owner->next;
    if(r->writer==tid||(writing&&owner&&owner->count)){printf("RW deadlock: slot=%p tid=%u writer=%u reads=%u local_reads=%u\n",(void *)p,tid,r->writer,r->readers,owner?owner->count:0);pthread_mutex_unlock(&r->lock);return (int32_t)UINT32_C(0x8002000b);}
    while(r->writer||(writing&&r->readers)){
        if(try_only){pthread_mutex_unlock(&r->lock);return (int32_t)UINT32_C(0x80020010);}
        ++r->waiters;pthread_cond_wait(&r->changed,&r->lock);--r->waiters;
    }
    if(writing)r->writer=tid;else{
        if(!owner){owner=calloc(1,sizeof(*owner));if(!owner){pthread_mutex_unlock(&r->lock);return (int32_t)UINT32_C(0x8002000c);}
            owner->id=tid;owner->next=r->owners;r->owners=owner;}
        ++owner->count;++r->readers;
    }
    pthread_mutex_unlock(&r->lock);return 0;
}
static int32_t GUEST_ABI rw_read(Rwlock **p){return rw_acquire(p,0,0);}
static int32_t GUEST_ABI rw_write(Rwlock **p){return rw_acquire(p,1,0);}
static int32_t GUEST_ABI rw_try_read(Rwlock **p){return rw_acquire(p,0,1);}
static int32_t GUEST_ABI rw_try_write(Rwlock **p){return rw_acquire(p,1,1);}
static int32_t GUEST_ABI rw_unlock(Rwlock **p){
    Rwlock *r=rw_find(p);if(!r)return (int32_t)UINT32_C(0x80020016);
    pthread_mutex_lock(&r->lock);uint32_t tid=host_thread_id();int result=0,wake=0;
    if(r->writer==tid){r->writer=0;wake=1;}else{Reader *owner=r->owners;while(owner&&owner->id!=tid)owner=owner->next;
        if(!owner||!owner->count)result=1;else{--owner->count;wake=!--r->readers;}}
    // Waiters only wait for the writer to leave, or for the last reader.
    if(wake&&r->waiters)pthread_cond_broadcast(&r->changed);
    pthread_mutex_unlock(&r->lock);return result?(int32_t)UINT32_C(0x80020001):0;
}
static int32_t GUEST_ABI rw_destroy(Rwlock **p){
    Rwlock *r=rw_find(p);if(!r)return (int32_t)UINT32_C(0x80020016);
    pthread_mutex_lock(&r->lock);int busy=r->writer||r->readers||r->waiters;
    if(!busy){__atomic_store_n(&r->active,0,__ATOMIC_RELEASE);store_handle((void **)p,(Rwlock *)1);}
    pthread_mutex_unlock(&r->lock);return busy?(int32_t)UINT32_C(0x80020010):0;
}

// ---- attributes and mutexes ---------------------------------------------------------------------
static int32_t orbis(int e) { return e ? (int32_t)(UINT32_C(0x80020000) | (unsigned)e) : 0; }
typedef struct Attr { uint32_t kind; int type, protocol, active; } Attr;
static Attr *attribute(Attr **p) {
    if (!p) return NULL;
    Attr *a = known(load_handle((void **)p), KIND_ATTR);
    return a && __atomic_load_n(&a->active, __ATOMIC_ACQUIRE) ? a : NULL;
}
static int GUEST_ABI attr_init(Attr **p) {
    if (!p) return 22;
    Attr *a = calloc(1, sizeof(*a)); if (!a) return 12;
    a->kind = KIND_ATTR; a->type = 1; a->active = 1;
    pthread_mutex_lock(&registry); a = remember(a); if (a) store_handle((void **)p, a); pthread_mutex_unlock(&registry);
    return a ? 0 : 12;
}
static int GUEST_ABI attr_type(Attr **p, int type) {
    Attr *a = attribute(p); if (!a || type < 1 || type > 4) return 22;
    a->type = type; return 0;
}
static int GUEST_ABI attr_protocol(Attr **p, int protocol) {
    Attr *a = attribute(p); if (!a || protocol < 0 || protocol > 2) return 22;
    a->protocol = protocol; return 0;
}
static int GUEST_ABI attr_destroy(Attr **p) {
    Attr *a = attribute(p); if (!a) return 22;
    __atomic_store_n(&a->active, 0, __ATOMIC_RELEASE); store_handle((void **)p, NULL); return 0;
}
// Types: 1 error checking, 2 recursive, 3 normal, 4 adaptive. `owner` and `depth` are written by
// the owner only; another thread reading `owner` never sees its own id there.
typedef struct Mutex { uint32_t kind, word, owner, waiters; unsigned depth; int type, active; } Mutex;
static Mutex *create_mutex(int type) { // registry held
    Mutex *m = calloc(1, sizeof(*m)); if (!m) return NULL;
    m->kind = KIND_MUTEX; m->type = type; m->active = 1; return remember(m);
}
static int GUEST_ABI mutex_init(Mutex **p, Attr **a, const char *name) {
    (void)name; if (!p) return 22;
    Attr *attr = a && *a ? attribute(a) : NULL;
    if (a && *a && !attr) return 22;
    pthread_mutex_lock(&registry);
    Mutex *m = create_mutex(attr ? attr->type : 1); if (m) store_handle((void **)p, m);
    pthread_mutex_unlock(&registry); return m ? 0 : 12;
}
static Mutex *mutex(Mutex **p) {
    if (!p) return NULL;
    Mutex *m = load_handle((void **)p);
    if ((uintptr_t)m < 2) { // the static initializers (0 error checking, 1 adaptive): made on first use
        pthread_mutex_lock(&registry);
        m = *p;
        if ((uintptr_t)m < 2) { Mutex *made = create_mutex(m ? 4 : 1); if (made) store_handle((void **)p, made); m = made; }
        pthread_mutex_unlock(&registry);
    }
    m = known(m, KIND_MUTEX);
    return m && __atomic_load_n(&m->active, __ATOMIC_ACQUIRE) ? m : NULL;
}
static void take(Mutex *m) { // blocking, ignores ownership
    if (word_try(&m->word)) return;
    __atomic_add_fetch(&m->waiters, 1, __ATOMIC_RELAXED);
    word_lock(&m->word);
    __atomic_sub_fetch(&m->waiters, 1, __ATOMIC_RELAXED);
}
static int acquire(Mutex **p, int try_only) {
    Mutex *m = mutex(p); if (!m) return 22;
    const uint32_t me = host_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == me) {
        if (m->type == 2) { ++m->depth; return 0; }
        if (try_only) return 16;
        if (m->type == 1 || m->type == 4) return 11;
        // A normal mutex locked again by its owner deadlocks, as on the PS4.
    }
    if (try_only) { if (!word_try(&m->word)) return 16; }
    else take(m);
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED); m->depth = 1; return 0;
}
static int GUEST_ABI mutex_lock(Mutex **p) { return acquire(p, 0); }
static int GUEST_ABI mutex_try(Mutex **p) { return acquire(p, 1); }
static void give_up(Mutex *m) { __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED); m->depth = 0; word_unlock(&m->word); }
static int GUEST_ABI mutex_unlock(Mutex **p) {
    Mutex *m = mutex(p); if (!m) return 22;
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != host_thread_id()) return 1;
    if (!--m->depth) give_up(m);
    return 0;
}
static int GUEST_ABI mutex_destroy(Mutex **p) {
    Mutex *m = mutex(p); if (!m) return 22;
    if (__atomic_load_n(&m->word, __ATOMIC_ACQUIRE) || __atomic_load_n(&m->waiters, __ATOMIC_ACQUIRE)) return 16;
    __atomic_store_n(&m->active, 0, __ATOMIC_RELEASE); store_handle((void **)p, (Mutex *)2); return 0;
}

// ---- condition variables ------------------------------------------------------------------------
// A wait reads `seq` before it unlocks the mutex and sleeps while `seq` still has that value, so a
// signal sent any time after the unlock (it bumps `seq`) is never lost.
typedef struct Cond { uint32_t kind, seq, waiters; int active; } Cond;
static Cond *condition(Cond **p, int create) {
    if (!p) return NULL;
    Cond *c = load_handle((void **)p);
    if (!c && create) { // made on first use
        pthread_mutex_lock(&registry);
        c = *p;
        if (!c) {
            c = calloc(1, sizeof(*c));
            if (c) { c->kind = KIND_COND; c->active = 1; c = remember(c); }
            if (c) store_handle((void **)p, c);
        }
        pthread_mutex_unlock(&registry);
    }
    c = known(c, KIND_COND);
    return c && __atomic_load_n(&c->active, __ATOMIC_ACQUIRE) ? c : NULL;
}
static int cond_init(Cond **p, void *attr, const char *name) {
    (void)name; if (!p || attr) return 22; store_handle((void **)p, NULL); return condition(p,1) ? 0 : 12;
}
// timeout_us: UINT64_MAX waits without a deadline.
static int cond_wait_impl(Cond **p, Mutex **mp, uint64_t timeout_us) {
    Cond *c = condition(p,1); Mutex *m = mutex(mp); if (!c || !m) return 22;
    const uint32_t me = host_thread_id();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != me) return 1;
    struct timespec deadline, *until = NULL;
    if (timeout_us != UINT64_MAX) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += (time_t)(timeout_us / 1000000);
        deadline.tv_nsec += (long)(timeout_us % 1000000) * 1000;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
        until = &deadline;
    }
    const uint32_t seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    const unsigned depth = m->depth;
    __atomic_add_fetch(&c->waiters, 1, __ATOMIC_SEQ_CST);
    give_up(m);
    int timed_out = 0;
    while (__atomic_load_n(&c->seq, __ATOMIC_ACQUIRE) == seq)
        if (host_futex(&c->seq, FUTEX_WAIT_BITSET, seq, until) && errno == ETIMEDOUT) { timed_out = 1; break; }
    __atomic_sub_fetch(&c->waiters, 1, __ATOMIC_SEQ_CST);
    take(m);
    __atomic_store_n(&m->owner, me, __ATOMIC_RELAXED); m->depth = depth;
    return timed_out ? 60 : 0;
}
static void wake(Cond *c, int count) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&c->waiters, __ATOMIC_SEQ_CST)) host_futex(&c->seq, FUTEX_WAKE, (uint32_t)count, NULL);
}
static int32_t GUEST_ABI orb_cond_init(Cond **p, void *a, const char *n) { return orbis(cond_init(p,a,n)); }
static int32_t GUEST_ABI orb_cond_wait(Cond **p, Mutex **m) { return orbis(cond_wait_impl(p,m,UINT64_MAX)); }
static int32_t GUEST_ABI orb_cond_timedwait(Cond **p, Mutex **m, uint64_t us) { return orbis(cond_wait_impl(p,m,us == UINT64_MAX ? us - 1 : us)); }
static int32_t GUEST_ABI orb_cond_signal(Cond **p) { Cond *c = condition(p,1); if (!c) return orbis(22); wake(c, 1); return 0; }
static int32_t GUEST_ABI orb_cond_broadcast(Cond **p) { Cond *c = condition(p,1); if (!c) return orbis(22); wake(c, INT_MAX); return 0; }
static int32_t GUEST_ABI orb_cond_destroy(Cond **p) {
    Cond *c = condition(p,0); if (!c) return orbis(22);
    if (__atomic_load_n(&c->waiters, __ATOMIC_ACQUIRE)) return orbis(16);
    __atomic_store_n(&c->active, 0, __ATOMIC_RELEASE); store_handle((void **)p, (Cond *)2); return 0;
}
#define WRAP1(n,t) static int32_t GUEST_ABI orb_##n(t p) { return orbis(n(p)); }
WRAP1(attr_init, Attr **) WRAP1(attr_destroy, Attr **) WRAP1(mutex_lock, Mutex **)
WRAP1(mutex_try, Mutex **) WRAP1(mutex_unlock, Mutex **) WRAP1(mutex_destroy, Mutex **)
static int32_t GUEST_ABI orb_attr_type(Attr **p, int n) { return orbis(attr_type(p,n)); }
static int32_t GUEST_ABI orb_attr_protocol(Attr **p, int n) { return orbis(attr_protocol(p,n)); }
static int32_t GUEST_ABI orb_mutex_init(Mutex **p, Attr **a, const char *n) { return orbis(mutex_init(p,a,n)); }
uintptr_t sync_resolve(const char *name) {
    const char *api = runtime_api_name(name);
    if (strstr(name,"#libkernel#1#libkernel#257")) {
        if (!strcmp(api,"scePthreadRwlockInit")) return (uintptr_t)rw_init;
        if (!strcmp(api,"scePthreadRwlockRdlock")) return (uintptr_t)rw_read;
        if (!strcmp(api,"scePthreadRwlockWrlock")) return (uintptr_t)rw_write;
        if (!strcmp(api,"scePthreadRwlockTryrdlock")) return (uintptr_t)rw_try_read;
        if (!strcmp(api,"scePthreadRwlockTrywrlock")) return (uintptr_t)rw_try_write;
        if (!strcmp(api,"scePthreadRwlockUnlock")) return (uintptr_t)rw_unlock;
        if (!strcmp(api,"scePthreadRwlockDestroy")) return (uintptr_t)rw_destroy;
        if (!strcmp(api,"scePthreadCondInit")) return (uintptr_t)orb_cond_init;
        if (!strcmp(api,"scePthreadCondWait")) return (uintptr_t)orb_cond_wait;
        if (!strcmp(api,"scePthreadCondTimedwait")) return (uintptr_t)orb_cond_timedwait;
        if (!strcmp(api,"scePthreadCondSignal")) return (uintptr_t)orb_cond_signal;
        if (!strcmp(api,"scePthreadCondBroadcast")) return (uintptr_t)orb_cond_broadcast;
        if (!strcmp(api,"scePthreadCondDestroy")) return (uintptr_t)orb_cond_destroy;
    }
#define RESOLVE(n,fn) if (!strcmp(name,n "#libkernel#1#libkernel#257")||!strcmp(name,n "#libScePosix#1#libkernel#257")) return (uintptr_t)fn
    RESOLVE("F8bUHwAG284",orb_attr_init); RESOLVE("n2MMpvU8igI",orb_attr_init);
    RESOLVE("iMp8QpE+XO4",orb_attr_type); RESOLVE("1FGvU0i9saQ",orb_attr_protocol);
    RESOLVE("smWEktiyyG0",orb_attr_destroy); RESOLVE("cmo1RIYva9o",orb_mutex_init);
    RESOLVE("9UK1vLZQft4",orb_mutex_lock); RESOLVE("upoVrzMHFeE",orb_mutex_try);
    RESOLVE("tn3VlD0hG60",orb_mutex_unlock); RESOLVE("2Of0f+3mhhE",orb_mutex_destroy);
    RESOLVE("dQHWEsJtoE4",attr_init); RESOLVE("mDmgMOGVUqg",attr_type);
    RESOLVE("5txKfcMUAok",attr_protocol); RESOLVE("HF7lK46xzjY",attr_destroy);
    RESOLVE("ttHNfU+qDBU",mutex_init); RESOLVE("7H0iTOciTLo",mutex_lock);
    RESOLVE("K-jXhbt2gn4",mutex_try); RESOLVE("2Z+PpY6CaJg",mutex_unlock);
    RESOLVE("ltCfaGr2JGE",mutex_destroy);
#undef RESOLVE
    return 0;
}
