// SPDX-License-Identifier: GPL-2.0-or-later
// Save data is isolated under the new runtime's own user directory.
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "save_pattern.h"
typedef struct { char name[32]; } DirName;
typedef struct { char name[16]; } Point;
typedef struct { char title[128],subtitle[128],detail[1024];uint32_t user_param,pad;int64_t mtime;char reserved[32]; } Param;
typedef struct { int32_t user,pad;const DirName *dir;uint64_t blocks;uint32_t mode;char reserved[32];int32_t pad2; } Mount;
typedef struct { Point point;uint64_t required;uint32_t unused,status;char reserved[28];int32_t pad; } MountResult;
typedef struct { int32_t user,pad;const void *title;const DirName *dir;uint32_t key,order;char reserved[32]; } Search;
typedef struct { uint64_t blocks,free_blocks;char reserved[32]; } SearchInfo;
typedef struct { uint32_t hits,pad;DirName *names;uint32_t capacity,count;Param *params;SearchInfo *infos;char reserved[12];int32_t pad2; } Result;
_Static_assert(sizeof(Param)==1328&&sizeof(Mount)==64&&sizeof(MountResult)==64&&sizeof(Result)==56,"SaveData ABI");
static HostLock lock=HOST_LOCK_INIT;
static int initialized;
static struct { int active,readonly;char path[1200],meta[1200]; } slots[16];
static int32_t err(unsigned n){return (int32_t)(UINT32_C(0x809f0000)|n);}
static int name_valid(const DirName *dir){if(!dir||!memchr(dir->name,0,32)||!dir->name[0])return 0;
    return !strchr(dir->name,'/')&&!strchr(dir->name,'\\')&&!strchr(dir->name,':')&&strcmp(dir->name,".")&&strcmp(dir->name,"..");}
static int root(int user,char *path,size_t size){
    if(user<0)return 0;char base[1200];snprintf(base,sizeof(base),"%s",runtime_save_root());mkdir(base,0777);
    snprintf(path,size,"%s/%d",base,user);mkdir(path,0777);
    snprintf(path,size,"%s/%d/CUSA00004",base,user);return !mkdir(path,0777)||errno==EEXIST;
}
static int32_t GUEST_ABI initialize(const void *p){(void)p;char path[1200];if(!root(1,path,sizeof(path)))return err(11);initialized=1;return 0;}
static int32_t GUEST_ABI terminate(void){if(!initialized)return err(1);initialized=0;return 0;}
static int32_t GUEST_ABI mount_save(const Mount *m,MountResult *out){
    if(!initialized)return err(1);if(!m||!out||!name_valid(m->dir))return err(0);if(m->user<0)return err(17);
    char base[1200],path[1200],meta[1200];if(!root(m->user,base,sizeof(base)))return err(11);
    snprintf(path,sizeof(path),"%s/%s",base,m->dir->name);snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,m->dir->name);
    struct stat st;int exists=!stat(path,&st)&&S_ISDIR(st.st_mode);
    if(exists&&(m->mode&4))return err(7);if(!exists&&!(m->mode&36))return err(8);
    host_lock(&lock);int slot=-1;
    for(int i=0;i<16;++i){if(slots[i].active&&!strcmp(slots[i].path,path)){host_unlock(&lock);return err(13);}if(!slots[i].active&&slot<0)slot=i;}
    if(slot<0){host_unlock(&lock);return err(12);}
    if(!exists&&(mkdir(path,0777)||mkdir(meta,0777))){host_unlock(&lock);return err(11);}
    slots[slot].active=1;slots[slot].readonly=!!(m->mode&1);strcpy(slots[slot].path,path);strcpy(slots[slot].meta,meta);
    memset(out,0,sizeof(*out));snprintf(out->point.name,16,"/savedata%d",slot);out->status=!exists;
    runtime_file_mount((unsigned)slot,out->point.name,path,slots[slot].readonly);host_unlock(&lock);
    printf("Mounted save '%s' at %s (%s)\n",m->dir->name,out->point.name,exists?"existing":"created");return 0;
}
static int slot_of(const Point *p){if(!p||!memchr(p->name,0,16))return -1;for(int i=0;i<16;++i){char n[16];snprintf(n,16,"/savedata%d",i);if(slots[i].active&&!strcmp(n,p->name))return i;}return -1;}
static int32_t GUEST_ABI umount_save(const Point *p){host_lock(&lock);int slot=slot_of(p);if(slot>=0){runtime_file_unmount((unsigned)slot);slots[slot].active=0;}host_unlock(&lock);return slot>=0?0:err(4);}
static void read_param(const char *meta,Param *p){memset(p,0,sizeof(*p));char name[1400];snprintf(name,sizeof(name),"%s/param.bin",meta);FILE *f=fopen(name,"rb");if(f){fread(p,1,sizeof(*p),f);fclose(f);}
    struct stat st;if(!stat(name,&st))p->mtime=(int64_t)st.st_mtim.tv_sec;}
typedef struct {DirName dir;Param param;SearchInfo info;} SearchRow;
static int compare_rows(const void *left,const void *right,void *context){
    const Search *query=context;const SearchRow *a=left,*b=right;int result=0;
    if(query->key==1)result=(a->param.user_param>b->param.user_param)-(a->param.user_param<b->param.user_param);
    else if(query->key==3)result=(a->param.mtime>b->param.mtime)-(a->param.mtime<b->param.mtime);
    else if(query->key==2)result=(a->info.blocks>b->info.blocks)-(a->info.blocks<b->info.blocks);
    else if(query->key==5)result=(a->info.free_blocks>b->info.free_blocks)-(a->info.free_blocks<b->info.free_blocks);
    if(!result)result=strcmp(a->dir.name,b->dir.name);return query->order?-result:result;
}
static int32_t GUEST_ABI set_param(const Point *point,uint32_t type,const void *buf,uint64_t size){
    if(!buf)return err(0);host_lock(&lock);int slot=slot_of(point);if(slot<0){host_unlock(&lock);return err(4);}if(slots[slot].readonly){host_unlock(&lock);return err(0);}
    Param p;read_param(slots[slot].meta,&p);int valid=1;
    switch(type){case 0:if(size<sizeof(p))valid=0;else memcpy(&p,buf,sizeof(p));break;
    case 1:snprintf(p.title,128,"%.*s",(int)(size>127?127:size),(const char *)buf);break;
    case 2:snprintf(p.subtitle,128,"%.*s",(int)(size>127?127:size),(const char *)buf);break;
    case 3:snprintf(p.detail,1024,"%.*s",(int)(size>1023?1023:size),(const char *)buf);break;
    case 4:if(size<4)valid=0;else memcpy(&p.user_param,buf,4);break;
    case 5:if(size<8)valid=0;else memcpy(&p.mtime,buf,8);break;default:valid=0;}
    char name[1400];snprintf(name,sizeof(name),"%s/param.bin",slots[slot].meta);FILE *f=valid?fopen(name,"wb"):NULL;
    int success=f&&fwrite(&p,1,sizeof(p),f)==sizeof(p);if(f)fclose(f);host_unlock(&lock);return success?0:err(valid?11:0);
}
static int32_t GUEST_ABI search(const Search *query,Result *out){
    if(!initialized)return err(1);if(!query||!out||query->key>5||query->order>1||(out->capacity&&!out->names))return err(0);char base[1200];if(!root(query->user,base,sizeof(base)))return err(11);
    if(query->dir&&(!memchr(query->dir->name,0,32)||strchr(query->dir->name,'/')||strchr(query->dir->name,'\\')||strchr(query->dir->name,':')))return err(0);
    const char *pattern=query->dir&&query->dir->name[0]?query->dir->name:"%";
    fprintf(stderr,"SAVE_SEARCH user=%d pattern='%s' key=%u order=%u capacity=%u\n",query->user,pattern,query->key,query->order,out->capacity);
    DIR *d=opendir(base);out->hits=out->count=0;if(!d)return 0;
    SearchRow *rows=NULL;size_t capacity=0;
    for(struct dirent *e;(e=readdir(d));){const char *name=e->d_name;char entry[1500];struct stat st;snprintf(entry,sizeof(entry),"%s/%s",base,name);
        if(stat(entry,&st)||!S_ISDIR(st.st_mode)||!strcmp(name,".")||!strcmp(name,"..")||strstr(name,".sce_sys"))continue;
        if(strlen(name)>=32)continue;
        if(!save_pattern_matches(name,pattern))continue;
        char meta[1400],metadata[1500];snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,name);snprintf(metadata,sizeof(metadata),"%s/param.bin",meta);
        if(stat(metadata,&st))continue;
        if(out->hits==capacity){size_t next=capacity?capacity*2:16;SearchRow *grown=realloc(rows,next*sizeof(*rows));if(!grown){free(rows);closedir(d);return err(11);}rows=grown;capacity=next;}
        SearchRow *row=&rows[out->hits++];memset(row,0,sizeof(*row));strcpy(row->dir.name,name);read_param(meta,&row->param);
        char data_path[1500];snprintf(data_path,sizeof(data_path),"%s/%s/dis.dat",base,name);
        if(!stat(data_path,&st))row->info.blocks=((uint64_t)st.st_size+32767)/32768;
    }
    closedir(d);
    if(out->hits)qsort_r(rows,out->hits,sizeof(*rows),compare_rows,(void *)query);
    out->count=out->hits<out->capacity?out->hits:out->capacity;
    for(uint32_t i=0;i<out->count;++i){out->names[i]=rows[i].dir;if(out->params)out->params[i]=rows[i].param;if(out->infos)out->infos[i]=rows[i].info;
        fprintf(stderr,"SAVE_SEARCH found='%s' index=%u mtime=%lld\n",rows[i].dir.name,i,(long long)rows[i].param.mtime);}
    free(rows);return 0;
}
typedef struct {const void *buffer;uint64_t buffer_size,data_size;uint8_t reserved[32];} Icon;
static int32_t GUEST_ABI save_icon(const Point *point,const Icon *icon){
    if(!initialized)return err(1);if(!icon||!icon->buffer||!icon->data_size||icon->data_size>icon->buffer_size)return err(0);
    host_lock(&lock);int slot=slot_of(point);if(slot<0){host_unlock(&lock);return err(4);}
    if(slots[slot].readonly){host_unlock(&lock);return err(0);}
    mkdir(slots[slot].meta,0777);
    char path[1400],temporary[1400];snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);snprintf(temporary,sizeof(temporary),"%s/icon0.pending",slots[slot].meta);
    FILE *f=fopen(temporary,"wb");int ok=f&&fwrite(icon->buffer,1,(size_t)icon->data_size,f)==icon->data_size;if(f){fflush(f);fclose(f);}
    if(ok)ok=rename(temporary,path)==0;
    host_unlock(&lock);return ok?0:err(11);
}
uintptr_t save_resolve(const char *identity){
    if(!strstr(identity,"#libSceSaveData#1#libSceSaveData#257"))return 0;const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceSaveDataInitialize3",initialize);API("sceSaveDataTerminate",terminate);API("sceSaveDataMount2",mount_save);
    API("sceSaveDataUmount",umount_save);API("sceSaveDataSetParam",set_param);API("sceSaveDataDirNameSearch",search);
    API("sceSaveDataSaveIcon",save_icon);
#undef API
    return 0;
}
