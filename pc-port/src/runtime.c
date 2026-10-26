// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <sys/random.h>
#include <sys/syscall.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_MODULES = 8 };
typedef struct {
    void *allocations[MAX_MODULES];
    unsigned char *blocks[MAX_MODULES];
    uint64_t dtv[MAX_MODULES + 2];
    uint64_t *tcb;
} GuestTls;
static GuestModule layout[MAX_MODULES];
static uint64_t module_count, source_size, process_param, canary;
static unsigned char *source_image;
static _Thread_local GuestTls *host_tls;
static int tls_configured;
#define TLS_CONFIGURED() (tls_configured)
#define TLS_GET() (host_tls)
static volatile int64_t tls_faults;
static void **application_heap_api;
static _Thread_local int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
// Layout/contract checked against shadPS4 libc_internal_memory.cpp (GPL-2.0+).
typedef struct { uint64_t size; uint32_t flags, segment_info; uint64_t *mask, *table; } HeapTraceInfo;
static uint64_t heap_trace_mask, heap_trace_table[64];

static void stop(const char *reason) {
    fprintf(stderr, "RUNTIME_ERROR: %s\n", reason);
    fflush(stderr);
    _exit(23);
}
static uint64_t aligned(uint64_t value, uint64_t align) { return (value + align - 1) & ~(align - 1); }

void runtime_configure(unsigned char *image, uint64_t image_size, const GuestModule *modules,
                       uint64_t count, uint64_t procparam) {
    if (!count || count > MAX_MODULES || procparam >= image_size) stop("bad runtime metadata");
    if (TLS_CONFIGURED()) stop("runtime already configured");
    source_image = image; source_size = image_size; module_count = count;
    process_param = (uintptr_t)image + procparam;
    memcpy(layout, modules, (size_t)count * sizeof(*modules));
    for (uint64_t i = 0; i < count; ++i) {
        GuestModule *m = &layout[i];
        if (m->tls_id != i + 1 || m->tls_filesz > m->tls_memsz || m->tls_memsz > 1024 * 1024 ||
            !m->tls_align || m->tls_align > 4096 || (m->tls_align & (m->tls_align - 1)) ||
            m->tls_address > image_size || m->tls_filesz > image_size - m->tls_address)
            stop("bad TLS module metadata");
    }
    if (getrandom(&canary, sizeof(canary), 0) != (ssize_t)sizeof(canary)) stop("cannot generate stack canary");
    canary &= ~UINT64_C(255);
    tls_configured = 1;
}

void runtime_tls_attach(void) {
    if (!TLS_CONFIGURED()) stop("TLS is not configured");
    if (TLS_GET()) return;
    GuestTls *state = calloc(1, sizeof(*state));
    if (!state) stop("cannot allocate per-thread TLS state");
    for (uint64_t i = 0; i < module_count; ++i) {
        const GuestModule *m = &layout[i];
        uint64_t static_size = aligned(m->tls_memsz, m->tls_align);
        size_t bytes = (size_t)(static_size + (i == 0 ? 64 : 0));
        size_t alignment = (size_t)(m->tls_align < 64 ? 64 : m->tls_align);
        unsigned char *block = aligned_alloc(alignment, ((bytes ? bytes : 1) + alignment - 1) / alignment * alignment);
        if (!block) stop("cannot allocate per-module TLS block");
        memset(block, 0, bytes ? bytes : 1);
        if (m->tls_filesz) memcpy(block, source_image + m->tls_address, (size_t)m->tls_filesz);
        state->allocations[i] = block;
        state->blocks[i] = block;
        state->dtv[i + 2] = (uintptr_t)block;
        if (i == 0) state->tcb = (uint64_t *)(block + static_size);
    }
    state->dtv[0] = 1; state->dtv[1] = module_count;
    state->tcb[0] = (uintptr_t)state->tcb;
    state->tcb[1] = (uintptr_t)state->dtv;
    state->tcb[2] = (uintptr_t)state; // Thread handle services remain unimplemented.
    state->tcb[5] = canary;
    // Patched guest code reads its TCB through gs: (fs: belongs to the host's libc).
    host_tls = state;
    if (syscall(SYS_arch_prctl, 0x1001 /* ARCH_SET_GS */, (unsigned long)state->tcb)) stop("cannot set the guest GS base");
}

void runtime_tls_detach(void) {
    if (!TLS_CONFIGURED()) return;
    GuestTls *state = TLS_GET();
    if (!state) return;
    host_tls = NULL;
    for (uint64_t i = 0; i < module_count; ++i) free(state->allocations[i]);
    free(state);
}
static GuestTls *current_tls(void) {
    runtime_tls_attach();
    return TLS_GET();
}
void runtime_set_thread_handle(void *handle) { current_tls()->tcb[2] = (uintptr_t)handle; }
static void *GUEST_ABI guest_tls_get_addr(const uint64_t *index) {
    if (!index || !index[0] || index[0] > module_count || index[1] >= layout[index[0] - 1].tls_memsz)
        stop("invalid dynamic TLS module or offset");
    return current_tls()->blocks[index[0] - 1] + index[1];
}
static void *GUEST_ABI guest_procparam(void) { return (void *)(uintptr_t)process_param; }
static void GUEST_ABI guest_set_heap_api(void **api) {
    if (!api) stop("null application heap API");
    application_heap_api = api;
    printf("Registered native application heap API at 0x%llx\n", (unsigned long long)(uintptr_t)application_heap_api);
}
static void GUEST_ABI guest_heap_trace_info(HeapTraceInfo *info) {
    if (!info || info->size < sizeof(*info)) stop("invalid heap trace info size");
    info->segment_info = 0;
    info->mask = &heap_trace_mask;
    info->table = heap_trace_table;
}
static void *GUEST_ABI guest_memset(void *p,int value,uint64_t n){return memset(p,value,(size_t)n);}
static void *GUEST_ABI guest_memcpy(void *p,const void *s,uint64_t n){return memcpy(p,s,(size_t)n);}
static void *GUEST_ABI guest_memmove(void *p,const void *s,uint64_t n){return memmove(p,s,(size_t)n);}
static int GUEST_ABI guest_memcmp(const void *p,const void *s,uint64_t n){return memcmp(p,s,(size_t)n);}
static uint64_t GUEST_ABI guest_strlen(const char *p){return strlen(p);}
static int GUEST_ABI guest_strcmp(const char *p,const char *s){return strcmp(p,s);}
static int GUEST_ABI guest_strncmp(const char *p,const char *s,uint64_t n){return strncmp(p,s,(size_t)n);}
static int GUEST_ABI guest_neo_mode(void){return 0;}

uintptr_t runtime_resolve(const char *identity, int data) {
    if (data) {
        if (!strcmp(identity, "f7uOxY9mM1U#libkernel#1#libkernel#257")) return (uintptr_t)&canary;
        return 0;
    }
    if(strstr(identity,"#libSceLibcInternal#1#libSceLibcInternal#257")){
        const char *api=runtime_api_name(identity);
        if(!strcmp(api,"memset"))return (uintptr_t)guest_memset;
        if(!strcmp(api,"memcpy"))return (uintptr_t)guest_memcpy;
        if(!strcmp(api,"memmove"))return (uintptr_t)guest_memmove;
        if(!strcmp(api,"memcmp"))return (uintptr_t)guest_memcmp;
        if(!strcmp(api,"strlen"))return (uintptr_t)guest_strlen;
        if(!strcmp(api,"strcmp"))return (uintptr_t)guest_strcmp;
        if(!strcmp(api,"strncmp"))return (uintptr_t)guest_strncmp;
    }
    if (!strcmp(identity, "vNe1w4diLCs#libkernel#1#libkernel#257")) return (uintptr_t)guest_tls_get_addr;
    if (!strcmp(runtime_api_name(identity), "__error") || !strcmp(runtime_api_name(identity), "__errno_location")) return (uintptr_t)runtime_errno;
    if (!strcmp(identity, "959qrazPIrg#libkernel#1#libkernel#257")) return (uintptr_t)guest_procparam;
    if (!strcmp(runtime_api_name(identity),"sceKernelIsNeoMode")) return (uintptr_t)guest_neo_mode;
    if (!strcmp(identity, "p5EcQeEeJAE#libkernel#1#libkernel#257")) return (uintptr_t)guest_set_heap_api;
    if (!strcmp(identity, "NWtTN10cJzE#libSceLibcInternalExt#1#libSceLibcInternal#257")) return (uintptr_t)guest_heap_trace_info;
    if (!strcmp(identity, "pO96TwzOm5E#libkernel#1#libkernel#257")) return (uintptr_t)memory_direct_size;
    if (!strcmp(identity, "rTXw65xmLIA#libkernel#1#libkernel#257")) return (uintptr_t)memory_allocate_direct;
    if (!strcmp(identity, "L-Q3LEjIbgA#libkernel#1#libkernel#257")) return (uintptr_t)memory_map_direct;
    uintptr_t fn = sync_resolve(identity);
    if (!fn) fn = thread_resolve(identity);
    if (!fn) fn = time_resolve(identity);
    if (!fn) fn = file_resolve(identity);
    if (!fn) fn = service_resolve(identity);
    if (!fn) fn = dialog_resolve(identity);
    if (!fn) fn = np_resolve(identity);
    if (!fn) fn = network_resolve(identity);
    if (!fn) fn = save_resolve(identity);
    if (!fn) fn = trophy_resolve(identity);
    if (!fn) fn = pad_resolve(identity);
    if (!fn) fn = audio_resolve(identity);
    if (!fn) fn = kernel_sync_resolve(identity);
    if (!fn) fn = http_resolve(identity);
    return fn ? fn : gpu_resolve(identity);
}

uint64_t runtime_tls_fault_count(void) { return 0; }
