// SPDX-License-Identifier: GPL-2.0-or-later
// Offline NP state and request lifecycle. No PSN login or invented online identity.
#include "runtime.h"
#include <string.h>
typedef void (GUEST_ABI *StateCallback)(int user,int state,void *data);
static HostLock lock=HOST_LOCK_INIT;
static struct { StateCallback fn; void *data; int pending; } callbacks[8];
static struct { void *fn,*data; } presence[8];
static struct { int active,complete; int32_t result; } requests[128];
static unsigned char registered_title[16],registered_secret[128];
typedef struct {uint64_t size;int8_t default_age;uint8_t padding[3];uint32_t count;const void *entries;} Restriction;
static unsigned char restriction_entries[256*8];
static int restriction_age;
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80550000)|n);}
static int32_t GUEST_ABI set_restriction(const Restriction *p){
    if(!p||p->size!=sizeof(*p)||p->default_age<0||p->count>256||(p->count&&!p->entries))return err(3);
    host_lock(&lock);restriction_age=p->default_age;if(p->count)memcpy(restriction_entries,p->entries,p->count*8);host_unlock(&lock);return 0;
}
static int32_t GUEST_ABI set_title(const void *title,const void *secret){
    if(!title||!secret)return err(3);host_lock(&lock);
    memcpy(registered_title,title,sizeof(registered_title));memcpy(registered_secret,secret,sizeof(registered_secret));
    host_unlock(&lock);return 0;
}
static int32_t GUEST_ABI get_state(int user,int *out){if(user<0||!out)return err(3);*out=0;return 0;}
static int32_t GUEST_ABI offline_identity(int user,void *out){if(user<0||!out)return err(3);return err(6);}
static int32_t GUEST_ABI register_state(StateCallback fn,void *data){
    if(!fn)return err(3);host_lock(&lock);
    for(int i=0;i<8;++i)if(!callbacks[i].fn){callbacks[i].fn=fn;callbacks[i].data=data;callbacks[i].pending=1;host_unlock(&lock);return i+1;}
    host_unlock(&lock);return err(8);
}
static int32_t GUEST_ABI register_presence(void *fn,void *data){
    if(!fn)return err(3);host_lock(&lock);
    for(int i=0;i<8;++i)if(!presence[i].fn){presence[i].fn=fn;presence[i].data=data;host_unlock(&lock);return i+1;}
    host_unlock(&lock);return err(8);
}
static int32_t GUEST_ABI check_callbacks(void){
    for(int i=0;i<8;++i){host_lock(&lock);
        StateCallback fn=callbacks[i].pending?callbacks[i].fn:NULL;void *data=callbacks[i].data;callbacks[i].pending=0;
        host_unlock(&lock);if(fn)fn(1,0,data);}return 0;
}
static int32_t GUEST_ABI create_request(const uint64_t *param){
    if(!param)return err(3);if(param[0]!=24)return err(17);
    host_lock(&lock);for(int i=0;i<128;++i)if(!requests[i].active){
        requests[i].active=1;requests[i].complete=0;requests[i].result=err(6);host_unlock(&lock);return 0x20000001+i;}
    host_unlock(&lock);return err(19);
}
static int32_t GUEST_ABI availability(int id,int user){
    int i=id-0x20000001;if(user<0)return err(3);if(i<0||i>=128)return err(20);
    host_lock(&lock);if(!requests[i].active){host_unlock(&lock);return err(20);}
    requests[i].complete=1;requests[i].result=err(6);host_unlock(&lock);return 0;
}
static int32_t GUEST_ABI poll(int id,int32_t *out){
    int i=id-0x20000001;if(!out)return err(3);if(i<0||i>=128)return err(20);
    host_lock_shared(&lock);int32_t r=!requests[i].active?err(20):!requests[i].complete?err(21):0;
    if(!r)*out=requests[i].result;host_unlock_shared(&lock);return r;
}
static int32_t GUEST_ABI delete_request(int id){
    int i=id-0x20000001;if(i<0||i>=128)return err(20);host_lock(&lock);
    int32_t r=requests[i].active?0:err(20);requests[i].active=0;host_unlock(&lock);return r;
}
uintptr_t np_resolve(const char *identity){
    if(!strstr(identity,"#libSceNpManager#1#libSceNpManager#257"))return 0;
    const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceNpSetNpTitleId",set_title);API("sceNpGetState",get_state);
    API("sceNpSetContentRestriction",set_restriction);
    API("sceNpGetOnlineId",offline_identity);API("sceNpGetAccountIdA",offline_identity);
    API("sceNpGetAccountCountryA",offline_identity);API("sceNpGetGamePresenceStatusA",offline_identity);
    API("sceNpRegisterStateCallbackA",register_state);API("sceNpRegisterGamePresenceCallbackA",register_presence);
    API("sceNpCheckCallback",check_callbacks);API("sceNpCreateAsyncRequest",create_request);
    API("sceNpCheckNpAvailabilityA",availability);API("sceNpPollAsync",poll);API("sceNpDeleteRequest",delete_request);
#undef API
    return 0;
}
