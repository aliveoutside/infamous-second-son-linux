// SPDX-License-Identifier: GPL-2.0-or-later
// Offline HTTP/SSL object lifecycle: requests never leave the machine (every send fails with the
// same error).
#include "../runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct {int active,kind,parent;void *arena;} Object;
static Object objects[512];static HostLock lock=HOST_LOCK_INIT;static int next=1;
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80431000)|n);}
static int allocate(int kind,int parent,void *arena){host_lock(&lock);if(next>=512){host_unlock(&lock);return -1;}int id=next++;objects[id]=(Object){.active=1,.kind=kind,.parent=parent,.arena=arena};host_unlock(&lock);return id;}
static Object *object(int id,int kind){return id>0&&id<512&&objects[id].active&&(!kind||objects[id].kind==kind)?&objects[id]:NULL;}
static int32_t GUEST_ABI ssl_init(uint64_t size){if(!size||size>0x40000000)return (int32_t)UINT32_C(0x80435022);void *p=malloc((size_t)size);if(!p)return (int32_t)UINT32_C(0x8043500c);int id=allocate(1,0,p);if(id<0){free(p);return (int32_t)UINT32_C(0x8043500c);}return id;}
static int32_t GUEST_ABI release(int id){Object *o=object(id,0);if(!o)return err(22);free(o->arena);o->active=0;return 0;}
static int32_t GUEST_ABI http_init(int net,int ssl,uint64_t size){(void)net;if(!object(ssl,1)||!size||size>0x40000000)return err(22);void *p=malloc((size_t)size);if(!p)return err(12);int id=allocate(2,ssl,p);if(id<0){free(p);return err(12);}return id;}
static int32_t GUEST_ABI create_template(int ctx,const char *agent,int version,int proxy){(void)proxy;if(!object(ctx,2)||!agent||version<1||version>2)return err(22);int id=allocate(3,ctx,NULL);return id<0?err(12):id;}
static int32_t GUEST_ABI create_connection(int tid,const char *url,int keep){(void)keep;if(!object(tid,3)||!url)return err(22);int id=allocate(4,tid,NULL);return id<0?err(12):id;}
static int32_t GUEST_ABI create_request(int cid,int method,const char *url,uint64_t length){(void)length;if(!object(cid,4)||method<0||method>7||!url)return err(22);int id=allocate(5,cid,NULL);return id<0?err(12):id;}
static int32_t GUEST_ABI send_request(int id,const void *data,uint64_t size){(void)data;(void)size;return object(id,5)?err(99):err(22);}
static int32_t GUEST_ABI status_code(int id,int *code){return object(id,5)&&code?err(99):err(22);}
static int32_t GUEST_ABI read_data(int id,void *data,uint64_t size){(void)size;return object(id,5)&&data?err(99):err(22);}
static int32_t GUEST_ABI response_headers(int id,char **headers,uint64_t *size){return object(id,5)&&headers&&size?err(99):err(22);}
static int32_t GUEST_ABI timeout_setting(int id,uint32_t us){(void)us;return object(id,0)?0:err(22);}
static int32_t GUEST_ABI chunked(int id,int enabled){(void)enabled;return object(id,0)?0:err(22);}
static int32_t GUEST_ABI add_header(int id,const char *name,const char *value,uint32_t mode){(void)mode;return object(id,5)&&name&&value?0:err(22);}
static int32_t GUEST_ABI load_cert(int id,int count,const void *ca,const void *cert,const void *key){if(!object(id,2)||count||ca||cert||key)return err(22);return 0;}
uintptr_t http_resolve(const char *identity){const char *api=runtime_api_name(identity);
    if(strstr(identity,"#libSceSsl#1#libSceSsl#257")){if(!strcmp(api,"sceSslInit"))return (uintptr_t)ssl_init;if(!strcmp(api,"sceSslTerm"))return (uintptr_t)release;}
    if(!strstr(identity,"#libSceHttp#1#libSceHttp#257"))return 0;
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceHttpInit",http_init);API("sceHttpTerm",release);API("sceHttpCreateTemplate",create_template);API("sceHttpCreateConnectionWithURL",create_connection);API("sceHttpCreateRequestWithURL",create_request);
    API("sceHttpDeleteTemplate",release);API("sceHttpDeleteConnection",release);API("sceHttpDeleteRequest",release);API("sceHttpSendRequest",send_request);API("sceHttpReadData",read_data);API("sceHttpGetStatusCode",status_code);API("sceHttpGetAllResponseHeaders",response_headers);
    API("sceHttpSetConnectTimeOut",timeout_setting);API("sceHttpSetRecvTimeOut",timeout_setting);API("sceHttpSetSendTimeOut",timeout_setting);API("sceHttpSetChunkedTransferEnabled",chunked);API("sceHttpAddRequestHeader",add_header);API("sceHttpsLoadCert",load_cert);
#undef API
    return 0;
}
