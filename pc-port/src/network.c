// SPDX-License-Identifier: GPL-2.0-or-later
#include <arpa/inet.h>
#include <sys/socket.h>
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
static _Thread_local int32_t net_errno_value;
static HostLock lock=HOST_LOCK_INIT;
static struct { void *memory; uint32_t size; } pools[128];
static int32_t GUEST_ABI net_init(void){return 0;}
static int32_t GUEST_ABI net_term(void){return 0;} // Winsock remains process-owned for conversion helpers.
static int32_t *GUEST_ABI errno_location(void){return &net_errno_value;}
static uint16_t GUEST_ABI host_to_net(uint16_t n){return htons(n);}
static uint16_t GUEST_ABI net_to_host(uint16_t n){return ntohs(n);}
static int family(int f){return f==2?AF_INET:f==28?AF_INET6:-1;}
static const char *GUEST_ABI guest_inet_ntop(int af,const void *src,char *dst,uint32_t size){
    int f=family(af);if(f<0||!src||!dst||!size){net_errno_value=22;return NULL;}net_init();
    const char *result=inet_ntop(f,src,dst,size);
    if(!result)net_errno_value=22;return result;
}
static int GUEST_ABI guest_inet_pton(int af,const char *src,void *dst){
    int f=family(af);if(f<0||!src||!dst){net_errno_value=22;return -1;}net_init();
    return inet_pton(f,src,dst);
}
static int32_t GUEST_ABI pool_create(const char *name,int size,int flags){
    (void)flags;if(!name||size<=0)return (int32_t)UINT32_C(0x80410116);void *p=malloc((size_t)size);
    if(!p)return (int32_t)UINT32_C(0x8041010c);host_lock(&lock);
    for(int i=0;i<128;++i)if(!pools[i].memory){pools[i].memory=p;pools[i].size=(uint32_t)size;host_unlock(&lock);return i+1;}
    host_unlock(&lock);free(p);return (int32_t)UINT32_C(0x80410118);
}
static int32_t GUEST_ABI pool_destroy(int id){
    if(id<1||id>128)return (int32_t)UINT32_C(0x80410116);host_lock(&lock);
    void *p=pools[id-1].memory;pools[id-1].memory=NULL;host_unlock(&lock);
    if(!p)return (int32_t)UINT32_C(0x80410116);free(p);return 0;
}
uintptr_t network_resolve(const char *identity){
    if(!strstr(identity,"#libSceNet#1#libSceNet#257"))return 0;
    const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceNetInit",net_init);API("sceNetTerm",net_term);API("sceNetErrnoLoc",errno_location);
    API("sceNetHtons",host_to_net);API("sceNetNtohs",net_to_host);API("sceNetInetNtop",guest_inet_ntop);
    API("sceNetInetPton",guest_inet_pton);API("sceNetPoolCreate",pool_create);API("sceNetPoolDestroy",pool_destroy);
#undef API
    return 0;
}
