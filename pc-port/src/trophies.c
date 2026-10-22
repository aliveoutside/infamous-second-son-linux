// SPDX-License-Identifier: GPL-2.0-or-later
// Local unlock flags only; never changes the game's source files or PSN state.
#include "runtime.h"
#include <stdio.h>
#include <string.h>
static HostLock lock=HOST_LOCK_INIT;
static int handles[16];
static struct {int active,registered,user;uint32_t flags[4];char file[1400];} contexts[8];
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80551600)|n);}
static int valid(int c,int h){return c>0&&c<=8&&contexts[c-1].active&&h>0&&h<=16&&handles[h-1];}
static int32_t GUEST_ABI create_handle(int *out){if(!out)return err(4);host_lock(&lock);for(int i=0;i<16;++i)if(!handles[i]){handles[i]=1;*out=i+1;host_unlock(&lock);return 0;}host_unlock(&lock);return err(5);}
static int32_t GUEST_ABI create_context(int *out,int user,uint32_t label,uint64_t options){
    if(!out||user<0||label||options)return err(4);host_lock(&lock);
    for(int i=0;i<8;++i)if(!contexts[i].active){memset(&contexts[i],0,sizeof(contexts[i]));contexts[i].active=1;contexts[i].user=user;*out=i+1;host_unlock(&lock);return 0;}
    host_unlock(&lock);return err(5);
}
static int32_t GUEST_ABI register_context(int c,int h,uint64_t options){
    if(options)return err(4);host_lock(&lock);if(!valid(c,h)){host_unlock(&lock);return err(9);}
    char root[1300];snprintf(root,sizeof(root),"%s/trophies",runtime_user_root());mkdir(root,0777);
    snprintf(contexts[c-1].file,sizeof(contexts[c-1].file),"%s/CUSA00004-user%d.bin",root,contexts[c-1].user);
    FILE *f=fopen(contexts[c-1].file,"rb");if(f){fread(contexts[c-1].flags,1,16,f);fclose(f);}contexts[c-1].registered=1;
    host_unlock(&lock);return 0;
}
static int32_t GUEST_ABI unlock_state(int c,int h,uint32_t *flags,uint32_t *count){
    if(!flags||!count)return err(4);host_lock_shared(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){host_unlock_shared(&lock);return err(15);}
    memcpy(flags,contexts[c-1].flags,16);*count=content_trophy_count;host_unlock_shared(&lock);return 0;
}
static int32_t GUEST_ABI unlock(int c,int h,int id,int *platinum){
    if(!platinum)return err(4);int known=0;for(unsigned i=0;i<content_trophy_count;++i)if(content_trophy_ids[i]==id)known=1;
    if(!known)return err(7);host_lock(&lock);
    if(!valid(c,h)||!contexts[c-1].registered){host_unlock(&lock);return err(15);}
    contexts[c-1].flags[id/32]|=1u<<(id%32);*platinum=-1;
    FILE *f=fopen(contexts[c-1].file,"wb");int ok=f&&fwrite(contexts[c-1].flags,1,16,f)==16;if(f)fclose(f);
    host_unlock(&lock);return ok?0:err(1);
}
static int32_t GUEST_ABI destroy_handle(int h){if(h<1||h>16)return err(8);host_lock(&lock);handles[h-1]=0;host_unlock(&lock);return 0;}
static int32_t GUEST_ABI destroy_context(int c){if(c<1||c>8)return err(9);host_lock(&lock);contexts[c-1].active=0;host_unlock(&lock);return 0;}
uintptr_t trophy_resolve(const char *identity){
    if(!strstr(identity,"#libSceNpTrophy#1#libSceNpTrophy#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceNpTrophyCreateHandle",create_handle);API("sceNpTrophyCreateContext",create_context);
    API("sceNpTrophyRegisterContext",register_context);API("sceNpTrophyGetTrophyUnlockState",unlock_state);
    API("sceNpTrophyUnlockTrophy",unlock);API("sceNpTrophyDestroyHandle",destroy_handle);API("sceNpTrophyDestroyContext",destroy_context);
#undef API
    return 0;
}
