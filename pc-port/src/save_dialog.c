// SPDX-License-Identifier: GPL-2.0-or-later
// ABI adapted from shadPS4 save_data/dialog/savedatadialog_ui.h (GPL-2.0-or-later).
// The dialog selects a slot; the original game performs the actual save/load.
// It is drawn by libiss-gpu.so over the game (gpu/save_dialog_ui.cpp, PS4 style, controller,
// keyboard and mouse); without it the game sees the dialog cancelled. While it is up the
// game's pad reads idle (save_dialog_blocking, src/pad.c), as on the console.
#include "runtime.h"
#include "../gpu/iss_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {char name[32];} SdDir;
typedef struct {char title[128],subtitle[128],detail[1024];uint32_t user,pad;int64_t mtime;char reserved[32];} SdMeta;
typedef struct {const char *title;void *icon;uint64_t size;char reserved[32];} SdNew;
typedef struct {int32_t user,pad;const void *title;const SdDir *dirs;uint32_t count,pad2;const SdNew *new_item;uint32_t focus,pad3;const SdDir *focus_dir;uint32_t style;char reserved[32];} SdItems;
typedef struct {uint32_t buttons,type;const char *text;char reserved[32];} SdUser;
typedef struct {uint32_t type,pad;uint64_t value;char reserved[32];} SdSystem;
typedef struct {uint64_t base_size;char base_reserved[36];uint32_t magic;int32_t size;uint32_t mode,type,pad;
    void *animation;const SdItems *items;const SdUser *user;const SdSystem *system;const uint32_t *error;const void *progress;void *data;const void *option;char reserved[24];} SdParams;
typedef struct {uint32_t mode,result,button,pad;SdDir *dir;SdMeta *meta;void *data;char reserved[32];} SdResult;
_Static_assert(sizeof(SdParams)==152&&sizeof(SdItems)==96&&sizeof(SdResult)==72&&sizeof(SdMeta)==1328,"Save dialog ABI");
typedef struct {SdDir dir;SdMeta meta;char label[320];int is_new;} Row;
typedef struct {uint32_t mode,type,result,button;void *data;char text[2048];int count,focus,yesno,passive,cancel_result,ui,user;
    Row rows[128];SdDir selected;SdMeta meta;void *list;char new_title[128];const void *new_icon;uint32_t new_icon_size;} Job;
static volatile int32_t sd_status,cancel_requested;

static Job *job;
extern int runtime_common_dialog_initialized(void);
static int32_t error(unsigned n){return (int32_t)(UINT32_C(0x80b80000)|n);}
static int valid_name(const char *p){return p&&memchr(p,0,32)&&*p&&!strchr(p,'/')&&!strchr(p,'\\')&&!strchr(p,':')&&strcmp(p,".")&&strcmp(p,"..");}
// The game's pad reads idle while the dialog is up (src/pad.c).
int save_dialog_blocking(void){return __atomic_load_n(&sd_status,__ATOMIC_SEQ_CST)==2;}
static void add_row(Job *j,int user,const SdDir *dir){
    if(j->count>=128||!valid_name(dir->name))return;
    char path[1400];snprintf(path,sizeof(path),"%s/%d/CUSA00004/%s",runtime_save_root(),user,dir->name);
    struct stat st;if(stat(path,&st)||!S_ISDIR(st.st_mode))return;
    Row *row=&j->rows[j->count];row->dir=*dir;
    snprintf(path,sizeof(path),"%s/%d/CUSA00004/%s.sce_sys/param.bin",runtime_save_root(),user,dir->name);
    FILE *f=fopen(path,"rb");if(f){fread(&row->meta,1,sizeof(row->meta),f);fclose(f);}
    snprintf(row->label,sizeof(row->label),"%s - %.127s",dir->name,row->meta.subtitle[0]?row->meta.subtitle:row->meta.title);++j->count;
}
// The result the game reads: list choice, or the button of a message.
static void set_result(Job *j,int canceled,int index,int no){
    if(j->mode==1&&!canceled&&index>=0&&index<j->count){j->selected=j->rows[index].dir;j->meta=j->rows[index].meta;}
    j->result=canceled||j->cancel_result?1:0;j->button=canceled||j->mode==1?0:no?2:1;
    fprintf(stderr,"SAVE_DIALOG finished mode=%u result=%u button=%u directory=%s\n",j->mode,j->result,j->button,j->selected.name);
}

// ---- The GPU library's dialog -------------------------------------------------------------------
static int open_ui(Job *j){
    if(!gpu_dialog_available())return -1;
    IssDialog *d=calloc(1,sizeof(*d));if(!d)return -1;
    d->mode=j->mode;d->type=j->type;d->focus=(uint32_t)j->focus;
    d->buttons=j->mode==5||j->passive?IssDialogNone:j->yesno?IssDialogYesNo:IssDialogOk;
    snprintf(d->text,sizeof(d->text),"%s",j->text);
    for(int i=0;i<j->count&&i<ISS_DIALOG_MAX_ITEMS;++i){
        const Row *r=&j->rows[i];IssDialogItem *it=&d->items[d->count++];
        if(r->is_new){snprintf(it->title,sizeof(it->title),"%s",j->new_title);it->icon_png=j->new_icon;it->icon_png_size=j->new_icon_size;continue;}
        snprintf(it->dir,sizeof(it->dir),"%s",r->dir.name);
        snprintf(it->title,sizeof(it->title),"%s",r->meta.title[0]?r->meta.title:r->dir.name);
        snprintf(it->subtitle,sizeof(it->subtitle),"%s",r->meta.subtitle);
        snprintf(it->detail,sizeof(it->detail),"%.255s",r->meta.detail);
        it->mtime=r->meta.mtime;
        snprintf(it->icon_path,sizeof(it->icon_path),"%s/%d/CUSA00004/%s.sce_sys/icon0.png",runtime_save_root(),j->user,r->dir.name);
    }
    const int ok=gpu_dialog_open(d)==0;free(d);return ok?0:-1;
}
static void poll_ui(void){
    if(!job||!job->ui||__atomic_load_n(&sd_status,__ATOMIC_SEQ_CST)!=2)return;
    IssDialogResult r;if(gpu_dialog_poll(&r)!=1)return;
    set_result(job,r.canceled,r.index,r.button==2);__atomic_exchange_n(&sd_status,3,__ATOMIC_SEQ_CST);
}


static void reap(void){
    free(job);job=NULL;}
static int32_t GUEST_ABI initialize(void){if(!runtime_common_dialog_initialized())return error(1);return __sync_val_compare_and_swap(&sd_status,0,1)?error(4):0;}
static int32_t GUEST_ABI status(void){poll_ui();return __atomic_load_n(&sd_status,__ATOMIC_SEQ_CST);}
static int32_t GUEST_ABI terminate(void){if(!sd_status)return error(3);__atomic_exchange_n(&cancel_requested,1,__ATOMIC_SEQ_CST);
    if(job&&job->ui)gpu_dialog_close();
    reap();__atomic_exchange_n(&sd_status,0,__ATOMIC_SEQ_CST);return 0;}
static int32_t GUEST_ABI get_result(SdResult *out){poll_ui();if(sd_status!=3)return error(5);if(!out)return error(13);
    out->mode=job->mode;out->result=job->result;out->button=job->button;out->data=job->data;if(out->dir)*out->dir=job->selected;if(out->meta)*out->meta=job->meta;return 0;}
static int32_t GUEST_ABI open_dialog(const SdParams *p){
    if(sd_status!=1&&sd_status!=3)return error(6);if(!p)return error(13);if(p->base_size!=48||p->size!=sizeof(*p))return error(10);
    if(p->mode<1||p->mode>5||p->type<1||p->type>3)return error(10);
    if((p->mode==1&&!p->items)||(p->mode==2&&!p->user)||(p->mode==3&&!p->system)||(p->mode==4&&!p->error))return error(13);
    if(p->items&&(p->items->user<0||p->items->count>127||(p->items->count&&!p->items->dirs)))return error(10);
    if(job&&job->ui)gpu_dialog_close();
    reap();job=calloc(1,sizeof(*job));if(!job)return error(9);Job *j=job;j->mode=p->mode;j->type=p->type;j->data=p->data;j->result=1;
    if(p->items){const SdItems *items=p->items;j->user=items->user;
        if(p->mode==1&&p->type==1&&items->new_item){
            snprintf(j->new_title,sizeof(j->new_title),"%.127s",items->new_item->title?items->new_item->title:"New Saved Data");
            j->new_icon=items->new_item->icon;j->new_icon_size=(uint32_t)items->new_item->size;
            snprintf(j->rows[0].label,sizeof(j->rows[0].label),"%.300s",j->new_title);j->rows[0].is_new=1;j->count=1;}
        for(uint32_t i=0;i<items->count;++i)add_row(j,items->user,&items->dirs[i]);
        if(items->focus==1||items->focus==3)j->focus=j->count?j->count-1:0;
        if(items->focus==6&&items->focus_dir)for(int i=0;i<j->count;++i)if(!strncmp(j->rows[i].dir.name,items->focus_dir->name,32))j->focus=i;
        if(p->mode!=1&&j->count){j->selected=j->rows[0].dir;j->meta=j->rows[0].meta;}}
    const char *op=p->type==1?"save":p->type==2?"load":"delete";
    if(p->mode==1)snprintf(j->text,sizeof(j->text),"%s",p->type==1?"Save":p->type==2?"Load":"Delete");
    if(p->mode==2){snprintf(j->text,sizeof(j->text),"%.2000s",p->user->text?p->user->text:"");j->yesno=p->user->buttons==1;j->passive=p->user->buttons==2;}
    if(p->mode==3){uint32_t type=p->system->type;j->yesno=type==2||type==3;j->passive=type==5;j->cancel_result=type==1||type==4||type==6||type==7||type==8;
        char confirm[96];snprintf(confirm,sizeof(confirm),"Do you want to %s this saved data?",op);
        const char *text=type==1?"There is no saved data.":type==2?(p->type==1?"Do you want to save?":confirm):
            type==3?"Do you want to overwrite this saved data?":type==5?(p->type==1?"Saving...":p->type==2?"Loading...":"Deleting..."):
            type==7?"Completed.":type==6?"This saved data is corrupted.":"There is a problem with the saved data.";
        snprintf(j->text,sizeof(j->text),"%s",text);fprintf(stderr,"SAVE_DIALOG system_type=%u value=%llu\n",type,(unsigned long long)p->system->value);}
    if(p->mode==4)snprintf(j->text,sizeof(j->text),"An error occurred with the saved data. (0x%08X)",*p->error);
    if(p->mode==5){strcpy(j->text,"Please wait...");j->passive=1;}
    fprintf(stderr,"SAVE_DIALOG open mode=%u type=%u rows=%d text=%.200s\n",p->mode,p->type,j->count,j->text);
    __atomic_exchange_n(&cancel_requested,0,__ATOMIC_SEQ_CST);
    if(open_ui(j)==0){j->ui=1;__atomic_exchange_n(&sd_status,2,__ATOMIC_SEQ_CST);return 0;}
    // Without the overlay there is no native dialog: the game sees the dialog cancelled.
    fputs("SAVE_DIALOG overlay unavailable, answering cancel\n",stderr);
    j->result=1;__atomic_exchange_n(&sd_status,3,__ATOMIC_SEQ_CST);return 0;
}
uintptr_t save_dialog_resolve(const char *identity){
    if(!strstr(identity,"#libSceSaveDataDialog#1#libSceSaveDataDialog#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceSaveDataDialogInitialize",initialize);API("sceSaveDataDialogOpen",open_dialog);API("sceSaveDataDialogGetResult",get_result);
    API("sceSaveDataDialogUpdateStatus",status);API("sceSaveDataDialogTerminate",terminate);
#undef API
    return 0;
}
