// SPDX-License-Identifier: GPL-2.0-or-later
// Include the production implementation so append-only overlap fixtures can be
// installed without allocating/mapping guest memory or loading the game.
#include "../src/posix/memory.c"
#include <inttypes.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static uint64_t oracle(uint64_t address,uint64_t size){
    for(unsigned i=0;i<mapping_count;++i){
        const Mapping *m=&mappings[i];
        if(address>=m->address&&address-m->address<m->size){
            const uint64_t available=m->size-(address-m->address);
            return size<available?size:available;
        }
    }
    return 0;
}
static unsigned checks;
static int verify(uint64_t address){
    static const uint64_t sizes[]={0,1,15,16,4096,65535,65536,65537,UINT64_MAX};
    for(unsigned s=0;s<sizeof(sizes)/sizeof(sizes[0]);++s){
        CHECK(memory_clamp(address,sizes[s])==oracle(address,sizes[s]));++checks;
    }
    return 0;
}
int main(void){
    // Flags are inherited separately for each process by the runner. The same
    // production implementation is tested with either cache ordering and index.
    for(unsigned i=0;i<768;++i)
        register_mapping(UINT64_C(0x100000)+UINT64_C(0x20000)*i,65536,NULL,3);
    CHECK(!verify(0));CHECK(!verify(UINT64_MAX));
    for(unsigned i=0;i<768;++i){
        uint64_t start=UINT64_C(0x100000)+UINT64_C(0x20000)*i;
        CHECK(!verify(start));CHECK(!verify(start+1));CHECK(!verify(start+65535));
        CHECK(!verify(start+65536));CHECK(!verify(start-1));
    }
    // A miss must not survive a later append at exactly that address.
    const uint64_t future=UINT64_C(0x80000000);
    CHECK(!verify(future));register_mapping(future,32768,NULL,3);CHECK(!verify(future));
    // A later larger/smaller overlap never changes first-match clamping, even
    // when it invalidates the sorted index and forces the linear fallback.
    CHECK(!verify(UINT64_C(0x100800)));
    register_mapping(UINT64_C(0x100000),UINT64_C(0x40000),NULL,3);
    register_mapping(UINT64_C(0x100800),1024,NULL,3);
    CHECK(!verify(UINT64_C(0x100800)));CHECK(!verify(UINT64_C(0x110000)));
    for(unsigned i=0;i<25000;++i){
        uint64_t address=UINT64_C(0x100000)+((UINT64_C(11400714819323198485)*i)>>37);
        CHECK(!verify(address));
    }
    // Collision-heavy reuse must retain the full address tag, including low bits.
    for(unsigned i=0;i<10000;++i){
        uint64_t address=UINT64_C(0x100000)+((i*487u)%768u)*UINT64_C(0x20000)+(i&15);
        CHECK(!verify(address));
    }
    printf("PASS memory_clamp: %u checks, first-match overlaps, miss/appends, boundaries and collisions\n",checks);
    return 0;
}
