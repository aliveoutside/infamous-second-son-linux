// SPDX-License-Identifier: GPL-2.0-or-later
#include "../runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef void *(GUEST_ABI *Entry)(void *);
typedef void (GUEST_ABI *Destructor)(void *);
typedef struct Attribute {
    uint64_t stacksize, affinity; int policy, priority, inherit, detached, active;
    void *stackaddr; struct Attribute *next;
} Attribute;
typedef struct Thread {
    pthread_t host; uint32_t id; Entry entry; void *arg, *result;
    Attribute attr; int joined, detached; char name[64]; struct Thread *next;
} Thread;
static HostLock registry = HOST_LOCK_INIT;
static Attribute *attrs;
static Thread *threads;
static _Thread_local Thread *current;
static struct { pthread_key_t slot; Destructor dtor; int active; } keys[1024];
static uint32_t key_count;
static int32_t error(int n) { return n ? (int32_t)(UINT32_C(0x80020000) | (unsigned)n) : 0; }
static Attribute defaults(void) {
    Attribute a = {0}; a.stacksize = 1024 * 1024; a.affinity = 127;
    a.priority = 700; a.policy = 1; a.inherit = 4; a.active = 1; return a;
}
static Attribute *attribute(Attribute **p) {
    if (!p || !*p) return NULL;
    host_lock_shared(&registry); Attribute *a = attrs;
    while (a && (a != *p || !a->active)) a = a->next;
    host_unlock_shared(&registry); return a;
}
static Thread *thread(Thread *p) {
    host_lock_shared(&registry); Thread *t = threads;
    while (t && t != p) t = t->next;
    host_unlock_shared(&registry); return t;
}
static Thread *GUEST_ABI self(void) {
    if (current) return current;
    Thread *t = calloc(1, sizeof(*t)); if (!t) _exit(23);
    t->attr = defaults(); t->id = host_thread_id();
    pthread_attr_t native; void *stack = NULL; size_t stack_size = 0;
    pthread_t me = pthread_self(); t->host = me;
    if (!pthread_getattr_np(me, &native)) { pthread_attr_getstack(&native, &stack, &stack_size); pthread_attr_destroy(&native); }
    t->attr.stackaddr = stack; t->attr.stacksize = stack_size;
    strcpy(t->name, "main");
    host_lock(&registry); t->next = threads; threads = t; current = t;
    host_unlock(&registry); runtime_set_thread_handle(t); return t;
}
static int32_t GUEST_ABI attr_init(Attribute **p) {
    if (!p) return error(22); Attribute *a = calloc(1, sizeof(*a)); if (!a) return error(12);
    *a = defaults(); host_lock(&registry); a->next = attrs; attrs = a; *p = a;
    host_unlock(&registry); return 0;
}
static int32_t GUEST_ABI attr_destroy(Attribute **p) {
    Attribute *a = attribute(p); if (!a) return error(22); a->active = 0; *p = NULL; return 0;
}
static int32_t GUEST_ABI attr_get(Thread *t, Attribute **p) {
    t = thread(t); Attribute *a = attribute(p); if (!t || !a) return error(22);
    Attribute *next = a->next; *a = t->attr; a->next = next; a->active = 1; return 0;
}
static int32_t GUEST_ABI attr_stack(Attribute **p, uint64_t size) {
    Attribute *a = attribute(p); if (!a || size < 16384 || size > UINT64_C(0x40000000)) return error(22);
    a->stacksize = size; return 0;
}
static int32_t GUEST_ABI attr_affinity(Attribute **p, uint64_t mask) {
    Attribute *a = attribute(p); if (!a || !mask) return error(22); a->affinity = mask; return 0;
}
static int32_t GUEST_ABI attr_get_affinity(Attribute **p, uint64_t *mask) {
    Attribute *a = attribute(p); if (!a || !mask) return error(22); *mask = a->affinity; return 0;
}
static int32_t GUEST_ABI attr_inherit(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || (n != 0 && n != 4)) return error(22); a->inherit = n; return 0;
}
static int32_t GUEST_ABI attr_policy(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || n < 1 || n > 3) return error(22); a->policy = n; return 0;
}
static int32_t GUEST_ABI attr_priority(Attribute **p, const int *n) {
    Attribute *a = attribute(p); if (!a || !n || *n < 256 || *n > 959) return error(22); a->priority = *n; return 0;
}
static int32_t GUEST_ABI attr_get_priority(Attribute **p, int *n) {
    Attribute *a = attribute(p); if (!a || !n) return error(22); *n = a->priority; return 0;
}
static int32_t GUEST_ABI attr_detach(Attribute **p, int n) {
    Attribute *a = attribute(p); if (!a || (n != 0 && n != 1)) return error(22); a->detached = n; return 0;
}
static void destructors(void) {
    for (int pass = 0; pass < 4; ++pass) {
        int called = 0;
        for (uint32_t i = 0; i < key_count; ++i) {
            host_lock_shared(&registry);
            pthread_key_t slot = keys[i].slot; Destructor fn = keys[i].dtor;
            int active = keys[i].active; host_unlock_shared(&registry);
            if (!active || !fn) continue;
            void *value = pthread_getspecific(slot);
            if (value) { pthread_setspecific(slot, NULL); fn(value); called = 1; }
        }
        if (!called) break;
    }
}
static void *start(void *arg) {
    Thread *t = arg; current = t; runtime_tls_attach(); runtime_set_thread_handle(t);
    t->result = t->entry(t->arg); destructors(); runtime_tls_detach(); return NULL;
}
static int32_t GUEST_ABI create(Thread **out, Attribute **attr, Entry entry, void *arg, const char *name) {
    if (!out || !entry) return error(22);
    Attribute *a = attr && *attr ? attribute(attr) : NULL;
    if (attr && *attr && !a) return error(22);
    Thread *t = calloc(1, sizeof(*t)); if (!t) return error(12);
    t->attr = a ? *a : defaults(); t->detached = t->attr.detached;
    t->entry = entry; t->arg = arg;
    if (t->attr.stackaddr) { free(t); return error(45); } // custom guest stacks require a stack bridge.
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);
    pthread_attr_t native; pthread_attr_init(&native);
    pthread_attr_setstacksize(&native, (size_t)(t->attr.stacksize + 256 * 1024));
    host_lock(&registry); t->next = threads; threads = t; *out = t;
    int rc = pthread_create(&t->host, &native, start, t);
    if (rc) { threads = t->next; *out = NULL; host_unlock(&registry); pthread_attr_destroy(&native); free(t); return error(35); }
    host_unlock(&registry); pthread_attr_destroy(&native);
    printf("Created native guest thread: %s\n", t->name); return 0;
}
static int32_t GUEST_ABI join(Thread *p, void **result) {
    Thread *t = thread(p); if (!t || t->detached || t->joined) return error(22);
    if (t == self()) return error(11);
    if (pthread_join(t->host, NULL)) return error(22);
    if (result) *result = t->result; t->joined = 1; return 0;
}
static int32_t GUEST_ABI detach(Thread *p) {
    Thread *t = thread(p); if (!t || t->detached || t->joined) return error(22); t->detached = 1; pthread_detach(t->host); return 0;
}
static void GUEST_ABI exit_thread(void *result) {
    self()->result = result; destructors(); runtime_tls_detach(); pthread_exit(NULL);
}
static int GUEST_ABI equal(Thread *a, Thread *b) { return a == b; }
static int GUEST_ABI yield(void) { sched_yield(); return 0; }
static int32_t GUEST_ABI key_create(uint32_t *out, Destructor dtor) {
    if (!out) return error(22); pthread_key_t slot; if (pthread_key_create(&slot, NULL)) return error(35);
    host_lock(&registry);
    if (key_count == 1024) { host_unlock(&registry); pthread_key_delete(slot); return error(35); }
    uint32_t key = key_count++; keys[key].slot = slot; keys[key].dtor = dtor; keys[key].active = 1; *out = key;
    host_unlock(&registry); return 0;
}
static int32_t GUEST_ABI key_delete(uint32_t key) {
    host_lock(&registry);
    if (key >= key_count || !keys[key].active) { host_unlock(&registry); return error(22); }
    keys[key].active = 0; pthread_key_delete(keys[key].slot); host_unlock(&registry); return 0;
}
static void *GUEST_ABI get_specific(uint32_t key) {
    host_lock_shared(&registry); void *value = key < key_count && keys[key].active ? pthread_getspecific(keys[key].slot) : NULL;
    host_unlock_shared(&registry); return value;
}
static int32_t GUEST_ABI set_specific(uint32_t key, void *value) {
    host_lock_shared(&registry); int valid = key < key_count && keys[key].active;
    int result = valid && !pthread_setspecific(keys[key].slot, value);
    host_unlock_shared(&registry); return result ? 0 : error(22);
}
static int32_t GUEST_ABI sched_yield_host(void){sched_yield();return 0;}
uintptr_t thread_resolve(const char *identity) {
    if (!strstr(identity,"#libkernel#1#libkernel#257")) return 0;
    const char *api = runtime_api_name(identity);
#define API(n,f) if (!strcmp(api,n)) return (uintptr_t)f
    API("sched_yield",sched_yield_host);
    API("scePthreadAttrInit",attr_init); API("scePthreadAttrDestroy",attr_destroy); API("scePthreadAttrGet",attr_get);
    API("scePthreadAttrSetstacksize",attr_stack); API("scePthreadAttrSetaffinity",attr_affinity);
    API("scePthreadAttrGetaffinity",attr_get_affinity); API("scePthreadAttrSetinheritsched",attr_inherit);
    API("scePthreadAttrSetschedpolicy",attr_policy); API("scePthreadAttrSetschedparam",attr_priority);
    API("scePthreadAttrGetschedparam",attr_get_priority); API("scePthreadAttrSetdetachstate",attr_detach);
    API("scePthreadSelf",self); API("scePthreadCreate",create); API("scePthreadJoin",join);
    API("scePthreadDetach",detach); API("scePthreadExit",exit_thread); API("scePthreadEqual",equal);
    API("scePthreadYield",yield); API("scePthreadKeyCreate",key_create); API("scePthreadKeyDelete",key_delete);
    API("scePthreadGetspecific",get_specific); API("scePthreadSetspecific",set_specific);
#undef API
    return 0;
}
