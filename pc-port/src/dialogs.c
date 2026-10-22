// SPDX-License-Identifier: GPL-2.0-or-later
// Message dialogs as SDL message boxes; the status follows the box being closed.
#include "runtime.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct { uint32_t type,pad; const char *message; void *buttons; char reserved[24]; } UserMessage;
typedef struct {
    uint64_t base_size; char base_reserved[36]; uint32_t magic;
    uint64_t size; uint32_t mode,pad; UserMessage *user;
    void *progress,*system; int32_t user_id; char reserved[40]; uint32_t final_pad;
} Params;
static volatile int32_t initialized,status;
int runtime_common_dialog_initialized(void){return initialized!=0;}
extern uintptr_t save_dialog_resolve(const char *identity);
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x80b80000)|n);}
static int32_t GUEST_ABI common_initialize(void){return __atomic_exchange_n(&initialized,1,__ATOMIC_SEQ_CST)?err(2):0;}
static int32_t GUEST_ABI message_initialize(void){
    if(!initialized)return err(1);return __sync_val_compare_and_swap(&status,0,1)==0?0:err(4);
}
typedef struct { char message[8192]; unsigned flags; } UiMessage;
static void *show(void *arg){UiMessage *m=arg;SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,"inFAMOUS Second Son",m->message,NULL);free(m);__atomic_exchange_n(&status,3,__ATOMIC_SEQ_CST);return NULL;}
static int32_t GUEST_ABI message_open(const Params *p){
    if(status!=1&&status!=3)return err(6);if(!p)return err(13);
    if(p->size!=sizeof(*p)||p->base_size!=48)return err(10);
    if(p->mode!=1||!p->user||!p->user->message)return err(15);
    UiMessage *m=calloc(1,sizeof(*m));if(!m)return err(9);
    strncpy(m->message,p->user->message,sizeof(m->message)-1);
    printf("Opening game message dialog: %.500s\n",m->message);
    fprintf(stderr,"DIALOG tick_ms=%llu thread=%lu caller=%p message=%.500s\n",(unsigned long long)host_monotonic_ms(),host_thread_id(),__builtin_return_address(0),m->message);
    fflush(stdout);fflush(stderr);
    __atomic_exchange_n(&status,2,__ATOMIC_SEQ_CST);
    pthread_t t;if(pthread_create(&t,NULL,show,m)){free(m);__atomic_exchange_n(&status,1,__ATOMIC_SEQ_CST);return err(9);}pthread_detach(t);
    return 0;
}
static int32_t GUEST_ABI message_status(void){return __atomic_load_n(&status,__ATOMIC_SEQ_CST);}
uintptr_t dialog_resolve(const char *identity){
    uintptr_t save=save_dialog_resolve(identity);if(save)return save;
    const char *api=runtime_api_name(identity);
    if(strstr(identity,"#libSceCommonDialog#1#libSceCommonDialog#257")&&!strcmp(api,"sceCommonDialogInitialize"))return (uintptr_t)common_initialize;
    if(!strstr(identity,"#libSceMsgDialog#1#libSceMsgDialog#257"))return 0;
    if(!strcmp(api,"sceMsgDialogInitialize"))return (uintptr_t)message_initialize;
    if(!strcmp(api,"sceMsgDialogOpen"))return (uintptr_t)message_open;
    if(!strcmp(api,"sceMsgDialogUpdateStatus"))return (uintptr_t)message_status;
    return 0;
}
