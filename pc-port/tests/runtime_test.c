// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <signal.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "save_pattern.h"
#include "../gpu/sparse_copy.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char fixture[8192];
typedef void *(GUEST_ABI *TlsGet)(const uint64_t *);
typedef void *(GUEST_ABI *GetParam)(void);
static TlsGet get_tls;
static pthread_barrier_t start_gate;
static pthread_barrier_t ready_gate;
static unsigned char sparse_source[49152];
static unsigned sparse_lookups;
static unsigned char *sparse_backing(uint64_t address,uint64_t size){
    ++sparse_lookups;if(address>=0x10000&&address<0x18000&&size<=0x18000-address)return sparse_source+address-0x10000;
    if(address>=0x1c000&&address<0x20000&&size<=0x20000-address)return sparse_source+32768+address-0x1c000;
    return NULL;
}
static void test_sparse_copy(void){
    unsigned char out[65538];for(unsigned i=0;i<sizeof(sparse_source);++i)sparse_source[i]=(unsigned char)(i*29+7);
    memset(out,0xcc,sizeof(out));sparse_lookups=0;
    iss_copy_sparse(0x10000,out+1,32768,sparse_backing);CHECK(sparse_lookups==1&&!memcmp(out+1,sparse_source,32768));CHECK(out[0]==0xcc&&out[32769]==0xcc);
    memset(out,0xcc,sizeof(out));iss_copy_sparse(0x10000,out+1,65536,sparse_backing);
    CHECK(!memcmp(out+1,sparse_source,32768));for(unsigned i=32769;i<49153;++i)CHECK(out[i]==0);
    CHECK(!memcmp(out+49153,sparse_source+32768,16384));CHECK(out[0]==0xcc&&out[65537]==0xcc);
    memset(out,0xcc,sizeof(out));iss_copy_sparse(0x10013,out+1,123,sparse_backing);CHECK(!memcmp(out+1,sparse_source+19,123)&&out[124]==0xcc);
    sparse_lookups=0;iss_copy_sparse(0,out,0,sparse_backing);CHECK(sparse_lookups==0);
    puts("PASS: bulk mapped copies, sparse holes, unaligned ranges and bounds.");
}
static unsigned char *io_page;
static unsigned io_faults;
static void io_handler(int sig,siginfo_t *info,void *ctx){
    (void)sig;(void)ctx;uintptr_t address=(uintptr_t)info->si_addr;
    if(address<(uintptr_t)io_page||address>=(uintptr_t)io_page+8192)_exit(99);
    CHECK(!mprotect(io_page,8192,PROT_READ|PROT_WRITE));++io_faults;
}
static void test_protected_io(void){
    typedef int32_t(GUEST_ABI *Open)(const char *,int32_t,uint16_t);
    typedef int32_t(GUEST_ABI *Close)(int);
    typedef int64_t(GUEST_ABI *Transfer)(int,void *,uint64_t);
    typedef int64_t(GUEST_ABI *Pread)(int,void *,uint64_t,int64_t);
    typedef int64_t(GUEST_ABI *Seek)(int,int64_t,int);
    Open open=(Open)file_resolve("1G3lF1Gg1k8#libkernel#1#libkernel#257");
    Close close=(Close)file_resolve("UK2Tl2DWUns#libkernel#1#libkernel#257");
    Transfer write=(Transfer)file_resolve("4wSze92BhLI#libkernel#1#libkernel#257");
    Pread pread=(Pread)file_resolve("+r3rMFwItV4#libkernel#1#libkernel#257");
    Seek seek=(Seek)file_resolve("oib76F-12fk#libkernel#1#libkernel#257");
    CHECK(open&&close&&write&&pread&&seek);
    char guest[128];snprintf(guest,sizeof(guest),"/temp0/io-contract-%lu.bin",(unsigned long)getpid());
    int fd=open(guest,2|0x200|0x400,0600);CHECK(fd>=3);
    unsigned char source[8192];for(unsigned i=0;i<sizeof(source);++i)source[i]=(unsigned char)(i*37);
    CHECK(write(fd,source,sizeof(source))==sizeof(source));CHECK(seek(fd,123,0)==123);
    io_page=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(io_page!=MAP_FAILED);
    struct sigaction sa={0},previous;sa.sa_sigaction=io_handler;sa.sa_flags=SA_SIGINFO|SA_NODEFER;sigemptyset(&sa.sa_mask);CHECK(!sigaction(SIGSEGV,&sa,&previous));
    CHECK(!mprotect(io_page,8192,PROT_NONE));
    CHECK(pread(fd,io_page,8192,0)==8192);CHECK(io_faults==1);CHECK(!memcmp(io_page,source,8192));
    CHECK(seek(fd,0,1)==123);memset(io_page,0,8192);
    CHECK(pread(fd,io_page,8192,8000)==192);CHECK(!memcmp(io_page,source+8000,192)&&io_page[192]==0);
    CHECK(pread(fd,io_page,1,8192)==0);CHECK(pread(fd,io_page,1,-1)<0);CHECK(seek(fd,0,1)==123);
    CHECK(!mprotect(io_page,8192,PROT_NONE));CHECK(write(fd,io_page,192)==192);CHECK(io_faults==2);
    CHECK(close(fd)==0);sigaction(SIGSEGV,&previous,NULL);munmap(io_page,8192);io_page=NULL;
    char host[1200];snprintf(host,sizeof(host),"%s/temp0/io-contract-%lu.bin",runtime_user_root(),(unsigned long)getpid());CHECK(!unlink(host));
    puts("PASS: protected file buffers, EOF partial copy, positioned offsets and protected writes.");
}
typedef struct { unsigned char value; } WorkerData;
static void *worker(void *argument) {
    WorkerData *data = argument;
    uint64_t index[2] = {2, 0};
    unsigned char *tls = get_tls(index);
    CHECK(*tls == 0x22);
    *tls = data->value;
    pthread_barrier_wait(&ready_gate);
    pthread_barrier_wait(&start_gate);
    CHECK(*tls == data->value);
    runtime_tls_detach();
    return NULL;
}

int main(void) {
    test_sparse_copy();
    CHECK(save_pattern_matches("infamousssUser0","infamousss%"));
    CHECK(save_pattern_matches("infamousssAuto1","INFAMOUSSSAUTO_"));
    CHECK(save_pattern_matches("infamousss","infamousss%"));
    CHECK(save_pattern_matches("abc","%a%c%"));
    CHECK(!save_pattern_matches("infamousssUser0","infamousssAuto%"));
    CHECK(!save_pattern_matches("infamousssAuto12","infamousssAuto_"));
    CHECK(!save_pattern_matches("infamousssUser0","infamousss*"));
    test_protected_io();
    fixture[4096] = 0x11; fixture[4352] = 0x22;
    GuestModule modules[] = {
        {0, 8192, 0, 4096, 1, 48, 8, 1},
        {0, 8192, 0, 4352, 1, 128, 16, 2},
        {0, 8192, 0, 4608, 0, 64, 8, 3}
    };
    runtime_configure(fixture, sizeof(fixture), modules, 3, 1024);
    get_tls = (TlsGet)runtime_resolve("vNe1w4diLCs#libkernel#1#libkernel#257", 0);
    CHECK(get_tls);
    CHECK(!runtime_resolve("vNe1w4diLCs#libkernel#2#libkernel#257", 0));
    uint64_t index[2] = {1, 0};
    CHECK(*(unsigned char *)get_tls(index) == 0x11);
    index[0] = 3; index[1] = 63;
    CHECK(*(unsigned char *)get_tls(index) == 0);
    GetParam get_param = (GetParam)runtime_resolve("959qrazPIrg#libkernel#1#libkernel#257", 0);
    CHECK(get_param() == fixture + 1024);
    index[0] = 2; index[1] = 0;
    unsigned char *main_tls = get_tls(index);
    *main_tls = 0x77;
    pthread_barrier_init(&start_gate, NULL, 3);
    pthread_barrier_init(&ready_gate, NULL, 3);
    WorkerData work[2] = {{0x33}, {0x44}};
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, worker, &work[0]) && !pthread_create(&threads[1], NULL, worker, &work[1]));
    pthread_barrier_wait(&ready_gate);
    pthread_barrier_wait(&start_gate);
    for (int i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
    CHECK(*main_tls == 0x77);

    {
        // fs: -> gs: rewrite of guest code
        unsigned char code[] = {0x64,0x48,0x8b,0x04,0x25,0,0,0,0, 0x90, 0x64,0x48,0x33,0x04,0x25,40,0,0,0, 0xc3};
        CHECK(runtime_patch_tcb_accesses(code, sizeof(code)) == 2);
        CHECK(code[0] == 0x65 && code[10] == 0x65 && code[9] == 0x90);
    }
    CHECK(memory_direct_size() == UINT64_C(0x120000000));
    int64_t physical = -1;
    CHECK(memory_allocate_direct(0, (int64_t)memory_direct_size(), 1, 16384, 0, &physical) != 0 && physical == -1);
    CHECK(memory_allocate_direct(0, (int64_t)memory_direct_size(), 65536, 65536, 0, &physical) == 0 && !physical);
    void *a = NULL, *b = NULL;
    const uint64_t mapping_before=memory_mapping_epoch();
    CHECK(memory_mapping_lock_shared()==mapping_before);
    memory_mapping_unlock_shared();
    CHECK(memory_map_direct(&a, 65536, 3, 0, physical, 65536) == 0);
    CHECK(memory_mapping_epoch()==mapping_before+1);
    CHECK(memory_map_direct(&b, 65536, 3, 0, physical, 65536) == 0);
    CHECK(memory_mapping_epoch()==mapping_before+2);
    for(uint64_t offset=0;offset<65536;offset+=31){
        CHECK(memory_clamp((uintptr_t)a+offset,UINT64_MAX)==65536-offset);
        CHECK(memory_clamp((uintptr_t)a+offset,1)==1);
        CHECK(memory_clamp((uintptr_t)a+offset,0)==0);
        CHECK(memory_clamp((uintptr_t)b+offset,UINT64_MAX)==65536-offset);
    }
    CHECK(memory_clamp(UINT64_MAX,UINT64_MAX)==0);
    CHECK(a != b);
    ((unsigned char *)a)[49152] = 0x5a;
    CHECK(((unsigned char *)b)[49152] == 0x5a);
    ((unsigned char *)b)[123] = 0x7d;
    CHECK(((unsigned char *)a)[123] == 0x7d);
    for(uint64_t offset=0;offset<65536;offset+=4096){
        uint8_t *from_a=memory_backing((uintptr_t)a+offset,4096);
        uint8_t *from_b=memory_backing((uintptr_t)b+offset,4096);
        CHECK(from_a&&from_b&&from_a==from_b);
    }
    CHECK(memory_backing((uintptr_t)a+65535,2)==NULL);
    CHECK(memory_backing((uintptr_t)a+123,1)&&
          *memory_backing((uintptr_t)a+123,1)==0x7d);
    void *collision = a;
    CHECK(memory_map_direct(&collision, 65536, 3, 0x10, physical, 65536) != 0 && collision == a);
    CHECK(((unsigned char *)a)[123] == 0x7d);
    int64_t second, third;
    CHECK(memory_allocate_direct(0, (int64_t)memory_direct_size(), 16384, 0, 0, &second) == 0 && second == 65536);
    CHECK(memory_allocate_direct(0, (int64_t)memory_direct_size(), 16384, 0, 0, &third) == 0 && third == 81920);
    void *partial = NULL;
    CHECK(memory_map_direct(&partial, 16384, 3, 0, third, 16384) == 0);
    CHECK(memory_mapping_epoch()==mapping_before+3);
    CHECK((uintptr_t)partial % 16384 == 0);
    ((unsigned char *)partial)[16383] = 0x61;
    CHECK(((unsigned char *)partial)[16383] == 0x61);
    void *invalid = NULL;
    CHECK(memory_map_direct(&invalid, 65536, 3, 0, (int64_t)memory_direct_size(), 65536) != 0 && !invalid);
    CHECK(!munmap(a, 65536) && !munmap(b, 65536));
    CHECK(!munmap((void *)((uintptr_t)partial & ~(uintptr_t)65535), 65536));
    runtime_tls_detach();
    puts("PASS: concurrent TLS/TCB/DTV, MOV/XOR/CMP flags, direct-memory ranges, shared aliases and fixed-map collision.");
    return 0;
}
