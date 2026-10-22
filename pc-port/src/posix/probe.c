// SPDX-License-Identifier: GPL-2.0-or-later
// iss-probe: loads the prepared inFAMOUS eboot image, resolves its imports and enters it.
#include "../runtime.h"
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
typedef void (__attribute__((sysv_abi)) *GuestEntry)(void *, void *);
static unsigned char *image, *traps, *objects;
static uint64_t image_size, import_count;
static char (*names)[128];
static GuestModule modules[8];
static uint64_t module_count;
static const char *phase = "mapping";
static uint64_t active_module = UINT64_MAX;

static void fail(const char *message) {
    fprintf(stderr, "PROBE_ERROR: %s (errno %d)\n", message, errno);
    exit(1);
}
static void *checked_calloc(uint64_t n, size_t size) {
    void *p = calloc(n ? (size_t)n : 1, size);
    if (!p) fail("allocation failed");
    return p;
}
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated metadata");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t page_round(uint64_t n) { return ((size_t)n + 4095) & ~(size_t)4095; }
static int mapped(const Segment *s, uint64_t ns, uint64_t addr, uint64_t bytes, int exec) {
    for (uint64_t i = 0; i < ns; ++i)
        if ((!exec || (s[i].flags & 1)) && addr >= s[i].address &&
            addr - s[i].address <= s[i].size && bytes <= s[i].size - (addr - s[i].address)) return 1;
    return 0;
}
static void protect(void *p, size_t size, int mode) {
    if (mprotect(p, size, mode)) fail("mprotect failed");
}
static int permissions(uint64_t flags) {
    int mode = 0;
    if (flags & 1) mode |= PROT_EXEC;
    if (flags & 2) mode |= PROT_READ | PROT_WRITE;
    if (flags & 4) mode |= PROT_READ;
    return mode;
}
static void *low_allocate(size_t size, int mode) {
    // Game code may store addresses in 32-bit fields. Keep the probe below 4 GiB.
    for (uintptr_t address = 0x10000000; address + size <= UINT64_C(0xf0000000); address += 0x1000000) {
        void *p = mmap((void *)address, size, mode, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == (void *)address) return p;
        if (p != MAP_FAILED) munmap(p, size);
    }
    fail("cannot allocate low guest address space");
    return NULL;
}

// Faults of a signal: page tracking of the renderer first, then import traps and guest crashes.
static uintptr_t step_ring[64];
static unsigned step_count;
static void fault(int signal_number, siginfo_t *info, void *context) {
    ucontext_t *uc = context;
    if (signal_number == SIGTRAP && info->si_code == 2) {
        const uintptr_t at = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        static uintptr_t slot, expected, watch_rip, previous;
        static int armed = -1;
        if (armed < 0) { armed = getenv("ISS_WATCH_RIP") != NULL; if (armed) watch_rip = strtoull(getenv("ISS_WATCH_RIP"), NULL, 16); }
        if (armed == 1 && at == watch_rip) { slot = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP]; expected = *(uintptr_t *)slot; }
        if (slot && *(uintptr_t *)slot != expected) {
            fprintf(stderr, "WATCH slot %lx changed %lx -> %lx after rip %lx (now %lx)\n", slot, expected, *(uintptr_t *)slot, previous, at);
            expected = *(uintptr_t *)slot;
        }
        previous = at;
        step_ring[step_count++ & 63] = at;
        return;
    }
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (signal_number == SIGSEGV && gpu_handle_fault(context, info->si_addr)) return;
    uintptr_t address = rip, accessed = 0;
    uint64_t id = UINT64_MAX;
    const char *status = "guest_fault";
    int exit_code = 21;
    if (signal_number == SIGTRAP) {
        address = rip - 1; // the saved rip follows the int3
        if (address >= (uintptr_t)traps && address - (uintptr_t)traps < import_count * 16 && (address - (uintptr_t)traps) % 16 == 0) {
            id = (address - (uintptr_t)traps) / 16;
            status = "unsupported_import";
            exit_code = 20;
        }
    } else if (signal_number == SIGSEGV) {
        accessed = (uintptr_t)info->si_addr;
        if (accessed >= (uintptr_t)objects && accessed - (uintptr_t)objects < import_count * 4096) {
            id = (accessed - (uintptr_t)objects) / 4096;
            status = "unsupported_data_import";
            exit_code = 20;
        }
    }
    uint64_t caller = 0;
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    if (sp > 4096) memcpy(&caller, (void *)sp, sizeof(caller));
    printf("PROBE_EVENT {\"status\":\"%s\",\"exception\":\"signal %d\",", status, signal_number);
    printf("\"phase\":\"%s\",", phase);
    if (active_module != UINT64_MAX) printf("\"active_module\":%llu,", (unsigned long long)active_module);
    printf("\"rip\":\"0x%llx\",\"image_base\":\"0x%llx\",", (unsigned long long)address, (unsigned long long)(uintptr_t)image);
    if (address >= (uintptr_t)image && address - (uintptr_t)image < image_size)
        printf("\"guest_offset\":\"0x%llx\",", (unsigned long long)(address - (uintptr_t)image));
    if (caller >= (uintptr_t)image && caller - (uintptr_t)image < image_size)
        printf("\"caller_offset\":\"0x%llx\",", (unsigned long long)(caller - (uintptr_t)image));
    for (uint64_t m = 0; m < module_count; ++m)
        if (address >= (uintptr_t)image + modules[m].base &&
            address - ((uintptr_t)image + modules[m].base) < modules[m].size)
            printf("\"module_index\":%llu,\"module_offset\":\"0x%llx\",",
                   (unsigned long long)m, (unsigned long long)(address - ((uintptr_t)image + modules[m].base)));
    printf("\"tls_reads_emulated\":%llu,", (unsigned long long)runtime_tls_fault_count());
    if (id < import_count)
        printf("\"import_index\":%llu,\"symbol\":\"%s\",", (unsigned long long)id, names[id]);
    printf("\"rax\":\"0x%llx\",\"tid\":%u,", (unsigned long long)uc->uc_mcontext.gregs[REG_RAX], host_thread_id());
    printf("\"rdi\":\"0x%llx\",\"rsi\":\"0x%llx\"}\n",
           (unsigned long long)uc->uc_mcontext.gregs[REG_RDI], (unsigned long long)uc->uc_mcontext.gregs[REG_RSI]);
    fflush(stdout);
    if (step_count) {
        fprintf(stderr, "LAST STEPS of %u:", step_count);
        for (unsigned k = step_count > 64 ? step_count - 64 : 0; k < step_count; ++k) fprintf(stderr, " %lx", (unsigned long)step_ring[k & 63]);
        fprintf(stderr, "\n");
    }
    if (getenv("ISS_FAULT_DUMP")) {
        fprintf(stderr, "FAULT si_addr=%p code=%d\n", info->si_addr, info->si_code);
        for (int r = 0; r < 23; ++r) fprintf(stderr, "  greg[%d]=%llx\n", r, (unsigned long long)uc->uc_mcontext.gregs[r]);
        for (int q = 0; q < 16 && sp > 4096; ++q) fprintf(stderr, "  [rsp+%d]=%llx\n", q * 8, (unsigned long long)((uint64_t *)sp)[q]);
        if (caller > 0x10000000 && caller < 0xf0000000) {
            fprintf(stderr, "CODE before caller:");
            for (int b = -48; b < 8; ++b) fprintf(stderr, " %02x", ((unsigned char *)caller)[b]);
            fprintf(stderr, "\n");
        }
        if (getenv("ISS_DUMP_ADDR")) {
            unsigned char *at = (unsigned char *)strtoull(getenv("ISS_DUMP_ADDR"), NULL, 16);
            fprintf(stderr, "DUMP %p:", at);
            for (int b = 0; b < 96; ++b) fprintf(stderr, " %02x", at[b]);
            fprintf(stderr, "\n");
        }
        FILE *maps = fopen("/proc/self/maps", "r");
        char line[256];
        while (maps && fgets(line, sizeof(line), maps)) fputs(line, stderr);
    }
    // A fatal diagnostic trap cannot unwind concurrent guest/GPU workers safely.
    _exit(exit_code);
}
static void install_fault_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    const int signals[] = {SIGSEGV, SIGTRAP, SIGILL, SIGBUS, SIGFPE};
    for (unsigned i = 0; i < sizeof(signals) / sizeof(*signals); ++i)
        if (sigaction(signals[i], &action, NULL)) fail("cannot install diagnostic signal handler");
}

int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (argc != 3 || (strcmp(argv[2], "--map-only") && strcmp(argv[2], "--entry"))) {
        fprintf(stderr, "Usage: iss-probe boot.bin --map-only|--entry\n");
        return 2;
    }
    settings_apply(argc, argv); // problems are printed; the defaults stand in for bad values
    if (sysconf(_SC_PAGESIZE) != 4096) fail("unsupported host page size");
    char data_dir[4096];  // the game data sits next to the prepared image
    snprintf(data_dir, sizeof(data_dir), "%s", argv[1]);
    char *slash = strrchr(data_dir, '/');
    if (slash) *slash = 0; else strcpy(data_dir, ".");
    if (content_load(data_dir)) fail("game data missing: prepare the game folder again (scripts/linux/setup.sh)");
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open prepared image");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "ISSPRB01", 8) && memcmp(magic, "ISSPRB02", 8))) fail("bad boot magic");
    int linked = !memcmp(magic, "ISSPRB02", 8);
    image_size = read64(f);
    uint64_t entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    if (!image_size || image_size > 512 * 1024 * 1024 || !ns || ns > 64 ||
        nr > 1000000 || import_count > 10000) fail("metadata limits exceeded");
    uint64_t binding_count = 0, procparam = 0;
    uint64_t *bindings = checked_calloc(import_count, sizeof(*bindings));
    uint64_t *binding_kinds = checked_calloc(import_count, sizeof(*binding_kinds));
    if (linked) {
        module_count = read64(f); binding_count = read64(f); procparam = read64(f);
        if (!module_count || module_count > 8 || binding_count > import_count) fail("invalid module/binding count");
        for (uint64_t i = 0; i < module_count; ++i) {
            uint64_t *fields = (uint64_t *)&modules[i];
            for (int j = 0; j < 8; ++j) fields[j] = read64(f);
            GuestModule *m = &modules[i];
            if (m->base >= image_size || !m->size || m->size > image_size - m->base ||
                m->tls_id != i + 1 || m->tls_filesz > m->tls_memsz || m->tls_memsz > 1024 * 1024 ||
                !m->tls_align || m->tls_align > 4096 || (m->tls_align & (m->tls_align - 1))) fail("invalid module metadata");
        }
        for (uint64_t i = 0; i < binding_count; ++i) {
            uint64_t index = read64(f), address = read64(f), kind = read64(f);
            if (index >= import_count || address >= image_size || (kind != 1 && kind != 2) || binding_kinds[index])
                fail("invalid native binding");
            bindings[index] = address; binding_kinds[index] = kind;
        }
    }
    Segment *segments = checked_calloc(ns, sizeof(*segments));
    Reloc *relocs = checked_calloc(nr, sizeof(*relocs));
    names = checked_calloc(import_count, sizeof(*names));
    for (uint64_t i = 0; i < ns; ++i) {
        Segment *s = &segments[i];
        s->address = read64(f); s->size = read64(f); s->flags = read64(f);
        if (s->address > image_size || !s->size || s->size > image_size - s->address ||
            s->address % 4096 || s->flags > 7) fail("invalid segment");
        for (uint64_t j = 0; j < i; ++j)
            if (s->address < segments[j].address + page_round(segments[j].size) &&
                segments[j].address < s->address + page_round(s->size)) fail("overlapping segment pages");
    }
    if (!mapped(segments, ns, entry, 1, 1)) fail("entry is not executable");
    if (linked) {
        if (!mapped(segments, ns, procparam, 64, 0)) fail("invalid procparam");
        for (uint64_t i = 0; i < module_count; ++i) {
            GuestModule *m = &modules[i];
            if ((m->init != UINT64_MAX && !mapped(segments, ns, m->init, 1, 1)) ||
                !mapped(segments, ns, m->tls_address, m->tls_memsz, 0)) fail("invalid module init/TLS mapping");
        }
        for (uint64_t i = 0; i < import_count; ++i)
            if (binding_kinds[i] && !mapped(segments, ns, bindings[i], 1, binding_kinds[i] == 1))
                fail("invalid native binding mapping");
    }
    if (fread(names, 128, (size_t)import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i) {
        if (!names[i][0] || !memchr(names[i], 0, 128)) fail("invalid import name");
        for (char *p = names[i]; *p; ++p)
            if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '#' || *p == '_'))
                fail("unexpected import spelling");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        r->target = read64(f); r->kind = read64(f); r->value = read64(f); r->addend = read64(f);
        if (!mapped(segments, ns, r->target, 8, 0) || r->kind > 2 ||
            (r->kind && r->value >= import_count) || (!r->kind && r->value >= image_size) ||
            (r->kind != 2 && r->addend) || r->addend >= 4096) fail("invalid relocation");
    }
    image = memory_allocate_image(image_size);
    if(!image)fail("cannot allocate shared image backing");
    if (fread(image, 1, (size_t)image_size, f) != image_size || fgetc(f) != EOF) fail("wrong image payload length");
    fclose(f);
    size_t trap_bytes = page_round(import_count ? import_count * 16 : 1);
    traps = low_allocate(trap_bytes, PROT_READ | PROT_WRITE);
    memset(traps, 0xcc, trap_bytes);
    for (uint64_t i = 0; i < import_count; ++i) {
        traps[i * 16] = 0xcc; // INT3: the SIGTRAP handler identifies the exact import.
        traps[i * 16 + 1] = 0x0f; traps[i * 16 + 2] = 0x0b; // UD2 if unexpectedly resumed.
    }
    objects = low_allocate(page_round(import_count ? import_count * 4096 : 1), PROT_NONE);
    if (linked) runtime_configure(image, image_size, modules, module_count, procparam);
    for(uint64_t i=0;i<ns;++i)memory_register_image(segments[i].address,segments[i].size,(uint32_t)segments[i].flags);
    install_fault_handlers();
    if(linked&&!strcmp(argv[2],"--entry")){
        uint32_t sdk;memcpy(&sdk,image+procparam+16,4);
        if(gpu_start(sdk,(uint64_t)(uintptr_t)image))fail("graphics initialization failed");
    }
    for (uint64_t i = 0; i < nr; ++i) {
        Reloc *r = &relocs[i];
        uintptr_t value = r->kind == 0 ? (uintptr_t)image + r->value :
                          r->kind == 1 ? (uintptr_t)traps + r->value * 16 :
                                         (uintptr_t)objects + r->value * 4096 + r->addend;
        if (linked && r->kind) {
            if (binding_kinds[r->value]) {
                if (binding_kinds[r->value] != r->kind) fail("native binding type mismatch");
                value = (uintptr_t)image + bindings[r->value] + r->addend;
            }
            uintptr_t host = runtime_resolve(names[r->value], r->kind == 2);
            if (host) value = host + r->addend;
        }
        memcpy(image + r->target, &value, 8);
    }
    if (linked) {
        // Guest fs: reads become gs: reads (the guest GS base is its TCB), before the code is sealed.
        for (uint64_t i = 0; i < ns; ++i)
            if (segments[i].flags & 1) runtime_patch_tcb_accesses(image + segments[i].address, segments[i].size);
    }
    protect(image, page_round(image_size), PROT_NONE);
    for (uint64_t i = 0; i < ns; ++i)
        protect(image + segments[i].address, page_round(segments[i].size), permissions(segments[i].flags));
    protect(traps, trap_bytes, PROT_READ | PROT_EXEC);
    printf("Mapped eboot: base=0x%llx entry=+0x%llx bytes=%llu relocations=%llu imports=%llu\n",
           (unsigned long long)(uintptr_t)image, (unsigned long long)entry, (unsigned long long)image_size,
           (unsigned long long)nr, (unsigned long long)import_count);
    if (!strcmp(argv[2], "--map-only")) {
        puts("PROBE_EVENT {\"status\":\"mapped_only\",\"guest_executed\":false}");
        munmap(objects, page_round(import_count ? import_count * 4096 : 1)); munmap(traps, trap_bytes);
        free(names); free(relocs); free(segments); free(bindings); free(binding_kinds);
        return 0;
    }
    if (linked) {
        if(native_install_hooks(image,image_size)<0)fail("native hook validation failed");
        runtime_tls_attach();
        phase = "module_init";
        for (uint64_t m = 1; m < module_count; ++m) {
            if (modules[m].init == UINT64_MAX) continue;
            active_module = m;
            printf("Starting native module %llu at +0x%llx\n", (unsigned long long)m, (unsigned long long)modules[m].init);
            fflush(stdout);
            typedef int (GUEST_ABI *ModuleInit)(uint64_t, void *, void *);
            if (getenv("ISS_STEP_TRACE")) __asm__ volatile("pushfq; orq $0x100, (%%rsp); popfq" ::: "memory", "cc");
            int result = ((ModuleInit)(image + modules[m].init))(0, NULL, NULL);
            printf("Native module %llu initializer returned %d\n", (unsigned long long)m, result);
            if (result) fail("module initializer failed");
        }
    }
    phase = "guest_entry";
    active_module = UINT64_MAX;
    if(linked&&getenv("ISS_CHECK_SAVE_NAMES")){
        // Read-only diagnostic of this identity-gated eboot, verified in Ghidra.
        typedef int (GUEST_ABI *SlotIndex)(const char *);
        SlotIndex slot_index=(SlotIndex)(image+0x1e50a0);
        printf("SAVE_NATIVE_INDEX auto0=%d auto1=%d user0=%d\n",slot_index("infamousssAuto0"),slot_index("infamousssAuto1"),slot_index("infamousssUser0"));
    }
    struct { uint64_t count; const char *args[2]; } guest_args = {1, {"/app0/eboot.bin", NULL}};
    puts("Entering original x86-64 eboot; unresolved imports remain diagnostic traps.");
    fflush(stdout);
    // The eboot uses the SysV ABI: startup reads RDI/RSI.
    // Initial probe uses the host thread's stack; no guest TLS is installed yet.
    ((GuestEntry)(image + entry))(&guest_args, NULL);
    puts("PROBE_EVENT {\"status\":\"entry_returned\"}");
    return 22;
}
