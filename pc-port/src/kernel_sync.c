// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <string.h>
typedef struct {HostLock lock;HostCond changed;uint64_t value;int active,kind,max;} Object;
static Object objects[1024];static HostLock registry=HOST_LOCK_INIT;static uint64_t next_id=1;
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80020000)|n);}
static Object *lookup(uint64_t id,int kind){if(!id||id>=1024||!objects[id].active||objects[id].kind!=kind)return NULL;return &objects[id];}
static int32_t create(uint64_t *out,uint64_t initial,int kind,int max){
    if(!out)return err(22);host_lock(&registry);if(next_id>=1024){host_unlock(&registry);return err(12);}
    uint64_t id=next_id++;Object *o=&objects[id];host_lock_init(&o->lock);host_cond_init(&o->changed);
    o->value=initial;o->kind=kind;o->max=max;o->active=1;*out=id;host_unlock(&registry);return 0;
}
static int32_t GUEST_ABI event_create(uint64_t *out,const char *name,uint32_t attrs,uint64_t bits,const void *options){
    if(!name||options||(attrs&~0x33u))return err(22);return create(out,bits,1,0);
}
static int32_t GUEST_ABI sema_create(uint64_t *out,const char *name,uint32_t attrs,int count,int maximum,const void *options){
    if(!name||options||attrs>2||count<0||maximum<=0||count>maximum)return err(22);return create(out,(uint64_t)count,2,maximum);
}
// Whole milliseconds, rounded up.
static uint64_t wait_us(uint64_t deadline){uint64_t now=runtime_process_time_us();if(now>=deadline)return 0;return (deadline-now+999)/1000*1000;}
static int32_t GUEST_ABI event_wait(uint64_t id,uint64_t bits,uint32_t mode,uint64_t *out,uint32_t *timeout){
    Object *o=lookup(id,1);if(!o)return err(3);if(!bits||((mode&3)!=1&&(mode&3)!=2)||(mode&~0x33u)||((mode&0x30)==0x30))return err(22);
    uint64_t end=timeout?runtime_process_time_us()+*timeout:0;host_lock(&o->lock);
    while(o->active&&((mode&3)==1?(o->value&bits)!=bits:!(o->value&bits))){
        uint64_t us=timeout?wait_us(end):HOST_WAIT_FOREVER;
        if(!us||!host_cond_wait(&o->changed,&o->lock,us,0)){if(timeout)*timeout=0;host_unlock(&o->lock);return err(60);}
    }
    if(!o->active){host_unlock(&o->lock);return err(3);}
    if(out)*out=o->value;if(mode&0x10)o->value=0;else if(mode&0x20)o->value&=~bits;
    if(timeout){uint64_t now=runtime_process_time_us();*timeout=(uint32_t)(now>=end?0:end-now);}
    host_unlock(&o->lock);return 0;
}
static int32_t GUEST_ABI event_set(uint64_t id,uint64_t bits){Object *o=lookup(id,1);if(!o)return err(3);host_lock(&o->lock);o->value|=bits;host_cond_broadcast(&o->changed);host_unlock(&o->lock);return 0;}
static int32_t GUEST_ABI sema_wait(uint64_t id,int need,uint32_t *timeout){
    Object *o=lookup(id,2);if(!o)return err(3);if(need<=0||need>o->max)return err(22);
    uint64_t end=timeout?runtime_process_time_us()+*timeout:0;host_lock(&o->lock);
    while(o->active&&o->value<(uint64_t)need){uint64_t us=timeout?wait_us(end):HOST_WAIT_FOREVER;if(!us||!host_cond_wait(&o->changed,&o->lock,us,0)){if(timeout)*timeout=0;host_unlock(&o->lock);return err(60);}}
    if(!o->active){host_unlock(&o->lock);return err(3);}o->value-=(uint64_t)need;
    if(timeout){uint64_t now=runtime_process_time_us();*timeout=(uint32_t)(now>=end?0:end-now);}host_unlock(&o->lock);return 0;
}
static int32_t GUEST_ABI sema_signal(uint64_t id,int count){Object *o=lookup(id,2);if(!o)return err(3);host_lock(&o->lock);if(count<=0||(uint64_t)count>(uint64_t)o->max-o->value){host_unlock(&o->lock);return err(22);}o->value+=(uint64_t)count;host_cond_broadcast(&o->changed);host_unlock(&o->lock);return 0;}
static int32_t destroy(uint64_t id,int kind){Object *o=lookup(id,kind);if(!o)return err(3);host_lock(&o->lock);o->active=0;host_cond_broadcast(&o->changed);host_unlock(&o->lock);return 0;}
static int32_t GUEST_ABI event_destroy(uint64_t id){return destroy(id,1);}
static int32_t GUEST_ABI sema_destroy(uint64_t id){return destroy(id,2);}
uintptr_t kernel_sync_resolve(const char *identity){
    if(!strstr(identity,"#libkernel#1#libkernel#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceKernelCreateEventFlag",event_create);API("sceKernelWaitEventFlag",event_wait);API("sceKernelSetEventFlag",event_set);API("sceKernelDeleteEventFlag",event_destroy);
    API("sceKernelCreateSema",sema_create);API("sceKernelWaitSema",sema_wait);API("sceKernelSignalSema",sema_signal);API("sceKernelDeleteSema",sema_destroy);
#undef API
    return 0;
}
