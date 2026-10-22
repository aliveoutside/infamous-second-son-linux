// SPDX-License-Identifier: GPL-2.0-or-later
// Bootstrap direct-memory pool: shared section backing preserves CPU aliases.
#include "../runtime.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY UINT64_C(0x120000000)
#define ERR_INVALID ((int32_t)UINT32_C(0x80020016))
#define ERR_AGAIN ((int32_t)UINT32_C(0x80020023))
#define ERR_NOMEM ((int32_t)UINT32_C(0x8002000c))
typedef struct { uint64_t start, length; } Allocation;
static Allocation allocations[1024];
static uint64_t allocation_count;
static HostLock memory_lock = HOST_LOCK_INIT;
static int backing=-1;
static unsigned char *physical_shadow,*image_guest,*image_shadow;
typedef struct {uint64_t address,size;unsigned char *shadow;uint32_t permissions;} Mapping;
static Mapping mappings[4096];
static unsigned mapping_count;
static uint64_t mapping_epoch=1;
static void (*map_hook)(uint64_t,uint64_t,uint8_t *);

static uint64_t round_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
static int ensure_backing(void) {
    // A sparse memfd: pages exist once touched.
    if (backing < 0) {
        backing = memfd_create("iss-direct", MFD_CLOEXEC);
        if (backing >= 0 && ftruncate(backing, (off_t)CAPACITY)) { close(backing); backing = -1; }
    }
    if (backing >= 0 && !physical_shadow) {
        void *view = mmap(NULL, (size_t)CAPACITY, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0);
        physical_shadow = view == MAP_FAILED ? NULL : view;
    }
    return backing >= 0 && physical_shadow != NULL;
}
// A fixed-address shared mapping that never replaces anything already mapped there.
static void *map_at(void *where, size_t length, int prot, int fd, off_t offset) {
    void *view = mmap(where, length, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, offset);
    if (view == MAP_FAILED) return NULL;
    if (view != where) { munmap(view, length); return NULL; }
    return view;
}
static void register_mapping_locked(uint64_t address,uint64_t size,unsigned char *shadow,uint32_t permissions){
    if(mapping_count==4096){fputs("Mapping table exhausted\n",stderr);_exit(23);}
    mappings[mapping_count++]=(Mapping){address,size,shadow,permissions};
    ++mapping_epoch;
}
static void register_mapping(uint64_t address,uint64_t size,unsigned char *shadow,uint32_t permissions){
    host_lock(&memory_lock);
    register_mapping_locked(address,size,shadow,permissions);
    void (*hook)(uint64_t,uint64_t,uint8_t *)=map_hook;
    host_unlock(&memory_lock);if(hook)hook(address,size,shadow);
}
uint64_t memory_mapping_lock_shared(void){host_lock_shared(&memory_lock);return mapping_epoch;}
void memory_mapping_unlock_shared(void){host_unlock_shared(&memory_lock);}
uint64_t memory_mapping_epoch(void){uint64_t epoch=memory_mapping_lock_shared();memory_mapping_unlock_shared();return epoch;}
void *memory_allocate_image(uint64_t size){
    size=round_up(size,65536);
    int fd=memfd_create("iss-image",MFD_CLOEXEC);
    if(fd<0||ftruncate(fd,(off_t)size)){if(fd>=0)close(fd);return NULL;}
    void *shadow=mmap(NULL,(size_t)size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    image_shadow=shadow==MAP_FAILED?NULL:shadow;
    for(uintptr_t address=0x10000000;image_shadow&&!image_guest&&address+size<0xf0000000;address+=0x1000000)
        image_guest=map_at((void *)address,(size_t)size,PROT_READ|PROT_WRITE|PROT_EXEC,fd,0);
    close(fd);return image_guest;
}
void memory_register_image(uint64_t offset,uint64_t size,uint32_t flags){
    uint32_t permissions=((flags&4)?1u:0)|((flags&2)?2u:0)|((flags&1)?4u:0);
    register_mapping((uintptr_t)image_guest+offset,size,image_shadow+offset,permissions);
}
uint8_t *memory_backing(uint64_t address,uint64_t size){
    // Mappings are append-only. A positive first-match range remains valid
    // after later registrations, so sequential sparse copies can avoid a
    // lock and a linear scan for every 16 KiB page. Bound the cached range
    // before any earlier registration that starts inside it, preserving the
    // original first-match rule even when mappings overlap.
    typedef struct {uint64_t start,end;uint8_t *shadow;} BackingCache;
    static _Thread_local BackingCache cache;
    if(size&&address>=cache.start&&address<cache.end&&size<=cache.end-address)
        return cache.shadow+(address-cache.start);
    uint8_t *p=NULL;host_lock_shared(&memory_lock);
    for(unsigned i=0;i<mapping_count;++i){Mapping *m=&mappings[i];
        if(address>=m->address&&address-m->address<=m->size&&size<=m->size-(address-m->address)){
            p=m->shadow+(address-m->address);
            uint64_t end=m->address+m->size;
            for(unsigned prior=0;prior<i;++prior)
                if(mappings[prior].address>address&&mappings[prior].address<end)
                    end=mappings[prior].address;
            cache=(BackingCache){address,end,p};
            break;
        }}
    host_unlock_shared(&memory_lock);return p;
}
// Mapping index (ISS_MAPPING_INDEX=1): per thread, the mappings sorted by address, rebuilt when the
// append-only table grew; looked up without the lock by binary search. Only used while no two
// mappings overlap, so the containing mapping is the one the linear scan finds first.
typedef struct {unsigned count,usable;uint64_t start[4096],end[4096];} MappingIndex;
static int index_order(const void *a,const void *b){
    const uint64_t x=((const uint64_t *)a)[0],y=((const uint64_t *)b)[0];return x<y?-1:x>y;
}
static int mapping_index_find(uint64_t address,uint64_t *available){
    static _Thread_local MappingIndex *index;
    static int enabled;
    if(!enabled){const char *value=getenv("ISS_MAPPING_INDEX");enabled=value&&value[0]=='1'?1:-1;}
    if(enabled<0)return 0;
    const unsigned count=*(volatile unsigned *)&mapping_count;
    if(!index){index=calloc(1,sizeof(*index));if(!index){enabled=-1;return 0;}}
    if(index->count!=count){
        static _Thread_local uint64_t pairs[4096][2];
        host_lock_shared(&memory_lock);
        const unsigned n=mapping_count;
        for(unsigned i=0;i<n;++i){pairs[i][0]=mappings[i].address;pairs[i][1]=mappings[i].address+mappings[i].size;}
        host_unlock_shared(&memory_lock);
        qsort(pairs,n,sizeof(pairs[0]),index_order);
        index->usable=1;
        for(unsigned i=0;i<n;++i){
            index->start[i]=pairs[i][0];index->end[i]=pairs[i][1];
            if(i&&pairs[i][0]<pairs[i-1][1])index->usable=0;
        }
        index->count=n;
    }
    if(!index->usable)return 0;
    unsigned lo=0,hi=index->count;
    while(lo<hi){const unsigned mid=(lo+hi)/2;if(index->start[mid]<=address)lo=mid+1;else hi=mid;}
    *available=lo&&address<index->end[lo-1]?index->end[lo-1]-address:0;
    return 1;
}
uint64_t memory_clamp(uint64_t address,uint64_t size){
    // The mapping table is append-only. An exact-address hit for the first
    // containing mapping cannot change when later entries are appended. Never
    // cache misses. Add invalidation here before implementing unmap/replacement.
    typedef struct {uint64_t address,available;} ClampEntry;
    static _Thread_local ClampEntry cache[1024];
    static _Thread_local int enabled;
    static _Thread_local int cache_first;
    if(!cache_first){const char *value=getenv("ISS_CLAMP_CACHE_FIRST");cache_first=value&&value[0]=='1'?1:-1;}
    // Keep the original index-first route for A/B. With both approved mapping
    // options on it otherwise returns before consulting or populating this cache.
    if(cache_first<0){uint64_t available;if(mapping_index_find(address,&available))return available<size?available:size;}
    if(!enabled){const char *value=getenv("ISS_MAPPING_CACHE");enabled=value&&value[0]=='1'?1:-1;}
    ClampEntry *entry=&cache[((address>>4)*UINT64_C(0x9e3779b97f4a7c15))>>54];
    if(enabled>0&&entry->available&&entry->address==address)return size<entry->available?size:entry->available;
    if(cache_first>0){
        uint64_t available;
        if(mapping_index_find(address,&available)){
            if(enabled>0&&available)*entry=(ClampEntry){address,available};
            return available<size?available:size;
        }
    }
    uint64_t n=0;host_lock_shared(&memory_lock);
    for(unsigned i=0;i<mapping_count;++i){Mapping *m=&mappings[i];if(address>=m->address&&address-m->address<m->size){n=m->size-(address-m->address);break;}}
    host_unlock_shared(&memory_lock);
    if(enabled>0&&n)*entry=(ClampEntry){address,n};
    return n<size?n:size;
}
void memory_visit(void (*visit)(uint64_t,uint64_t,uint8_t *)){
    Mapping snapshot[4096];unsigned count;host_lock_shared(&memory_lock);count=mapping_count;memcpy(snapshot,mappings,count*sizeof(Mapping));host_unlock_shared(&memory_lock);
    for(unsigned i=0;i<count;++i)visit(snapshot[i].address,snapshot[i].size,snapshot[i].shadow);
}
void memory_set_map_hook(void (*hook)(uint64_t,uint64_t,uint8_t *)){host_lock(&memory_lock);map_hook=hook;host_unlock(&memory_lock);}
int memory_gpu_protect(uint64_t address,uint64_t size,uint32_t permissions){
    if(!size)return 1;uint32_t executable=0;
    host_lock_shared(&memory_lock);
    for(unsigned i=0;i<mapping_count;++i)if(address>=mappings[i].address&&address-mappings[i].address<mappings[i].size){executable=mappings[i].permissions&4;break;}
    host_unlock_shared(&memory_lock);
    int mode=(permissions&2)?PROT_READ|PROT_WRITE:(permissions&1)?PROT_READ:PROT_NONE;
    if(executable)mode|=PROT_EXEC;
    const uint64_t first=address&~UINT64_C(4095),end=(address+size+4095)&~UINT64_C(4095);
    return mprotect((void *)(uintptr_t)first,(size_t)(end-first),mode)==0;
}
uint64_t GUEST_ABI memory_direct_size(void) { return CAPACITY; }

int32_t GUEST_ABI memory_allocate_direct(int64_t start, int64_t end, uint64_t length,
                                        uint64_t alignment, int32_t type, int64_t *result) {
    if (start < 0 || end < 0 || !result || !length || length % 16384 || type < 0 || type > 10 ||
        (alignment && (alignment < 16384 || alignment & (alignment - 1)))) return ERR_INVALID;
    if ((uint64_t)end > CAPACITY) end = (int64_t)CAPACITY;
    if (end <= start || length > (uint64_t)(end - start)) return ERR_AGAIN;
    if (!alignment) alignment = 16384;
    if (alignment > CAPACITY) return ERR_AGAIN;
    host_lock(&memory_lock);
    uint64_t candidate = round_up((uint64_t)start, alignment);
    int found = 0;
    while (candidate <= (uint64_t)end && length <= (uint64_t)end - candidate) {
        int overlap = 0;
        for (uint64_t i = 0; i < allocation_count; ++i) {
            Allocation *a = &allocations[i];
            if (candidate < a->start + a->length && a->start < candidate + length) {
                candidate = round_up(a->start + a->length, alignment);
                overlap = 1;
                break;
            }
        }
        if (!overlap) { found = 1; break; }
    }
    if (!found || allocation_count == 1024 || !ensure_backing()) {
        host_unlock(&memory_lock);
        return ERR_AGAIN;
    }
    allocations[allocation_count++] = (Allocation){candidate, length};
    *result = (int64_t)candidate;
    host_unlock(&memory_lock);
    printf("Direct allocation: physical=0x%llx bytes=%llu alignment=%llu\n",
           (unsigned long long)candidate, (unsigned long long)length, (unsigned long long)alignment);
    return 0;
}

int32_t GUEST_ABI memory_map_direct(void **address, uint64_t length, int32_t prot,
                                   int32_t flags, int64_t physical, uint64_t alignment) {
    // Unknown modes remain explicit errors. Fixed address bit 0x10 is supported;
    // executable, mixed/flexible mappings and release/protect APIs remain separate work.
    if (!address || !length || length % 16384 || physical < 0 || (uint64_t)physical % 16384 ||
        (prot & ~0x33) || (flags & ~0x10) ||
        (alignment && (alignment < 16384 || alignment & (alignment - 1)))) return ERR_INVALID;
    if (!alignment) alignment = 16384;
    if (alignment > UINT64_C(0x100000000)) return ERR_INVALID;
    host_lock(&memory_lock);
    int allocated = 0;
    for (uint64_t i = 0; i < allocation_count; ++i)
        if ((uint64_t)physical >= allocations[i].start &&
            (uint64_t)physical - allocations[i].start <= allocations[i].length &&
            length <= allocations[i].length - ((uint64_t)physical - allocations[i].start)) allocated = 1;
    if (!allocated) { host_unlock(&memory_lock); return ERR_INVALID; }
    const int mode = (prot & 2) ? PROT_READ | PROT_WRITE : (prot & 1) ? PROT_READ : PROT_NONE;
    void *view = NULL;
    uintptr_t requested = (uintptr_t)*address;
    if (requested && requested % alignment == 0)
        view = map_at((void *)requested, (size_t)length, mode, backing, (off_t)physical);
    if (!view && !(flags & 0x10)) {
        for (uintptr_t base = 0x20000000; !view && base + length <= UINT64_C(0xf0000000); base += 0x1000000) {
            if (base % alignment) continue;
            view = map_at((void *)base, (size_t)length, mode, backing, (off_t)physical);
        }
        // Large GPU arenas cannot fit below 4 GiB; keep them within the PS4's 40-bit GPU address range.
        for(uintptr_t base=UINT64_C(0x200000000);!view&&base+length<UINT64_C(0x10000000000);base+=UINT64_C(0x10000000)){
            if(base%alignment)continue;
            view=map_at((void *)base,(size_t)length,mode,backing,(off_t)physical);
        }
    }
    if (!view) { host_unlock(&memory_lock); return ERR_NOMEM; }
    *address = view;
    unsigned char *shadow=physical_shadow+(uint64_t)physical;
    register_mapping_locked((uintptr_t)*address,length,shadow,(uint32_t)prot&3);
    void (*hook)(uint64_t,uint64_t,uint8_t *)=map_hook;
    host_unlock(&memory_lock);
    if(hook)hook((uintptr_t)*address,length,shadow);
    printf("Direct mapping: physical=0x%llx virtual=0x%llx bytes=%llu\n",
           (unsigned long long)(uint64_t)physical, (unsigned long long)(uintptr_t)*address, (unsigned long long)length);
    return 0;
}
