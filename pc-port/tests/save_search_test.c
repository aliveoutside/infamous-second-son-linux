// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"SAVE SEARCH FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef struct {char name[32];} Name;
typedef struct {int32_t user,pad;const void *title;const Name *pattern;uint32_t key,order;char reserved[32];} Query;
typedef struct {uint32_t hits,pad;Name *names;uint32_t capacity,count;void *params,*infos;char reserved[12];int32_t pad2;} Result;
typedef int32_t(GUEST_ABI *Init)(const void*);
typedef int32_t(GUEST_ABI *Search)(const Query*,Result*);
int main(void){
    Init init=(Init)save_resolve("TywrFKCoLGY#libSceSaveData#1#libSceSaveData#257");
    Search search=(Search)save_resolve("dyIhnXq-0SM#libSceSaveData#1#libSceSaveData#257");CHECK(init&&search);CHECK(init(NULL)==0);
    int user=1000000000+(int)getpid();char userdir[1300],base[1400],path[1500],file[1600];
    snprintf(userdir,sizeof(userdir),"%s/%d",runtime_save_root(),user);CHECK(!mkdir(userdir,0777));
    snprintf(base,sizeof(base),"%s/CUSA00004",userdir);CHECK(!mkdir(base,0777));
    const char *names[]={"infamousssAuto0","infamousssUser0"};
    for(int i=0;i<2;++i){snprintf(path,sizeof(path),"%s/%s",base,names[i]);CHECK(!mkdir(path,0777));
        snprintf(path,sizeof(path),"%s/%s.sce_sys",base,names[i]);CHECK(!mkdir(path,0777));snprintf(file,sizeof(file),"%s/param.bin",path);
        unsigned char param[1328]={0};uint32_t value=(uint32_t)(10+i*10);memcpy(param+1280,&value,4);FILE *f=fopen(file,"wb");CHECK(f);CHECK(fwrite(param,1,sizeof(param),f)==sizeof(param));CHECK(fclose(f)==0);}
    Name pattern={"infamousss%"},found[2];memset(found,0x5a,sizeof(found));unsigned char metadata[1328];
    Query q={0};q.user=user;q.pattern=&pattern;q.key=1;q.order=1;Result r={0};r.names=found;r.capacity=1;r.params=metadata;
    CHECK(search(&q,&r)==0&&r.hits==2&&r.count==1);CHECK(!strcmp(found[0].name,"infamousssUser0")&&found[1].name[0]==0x5a);
    uint32_t value;memcpy(&value,metadata+1280,4);CHECK(value==20);int64_t mtime;memcpy(&mtime,metadata+1288,8);CHECK(mtime>0);
    r.names=NULL;r.params=NULL;r.capacity=0;CHECK(search(&q,&r)==0&&r.hits==2&&r.count==0);
    pattern.name[0]=0;CHECK(search(&q,&r)==0&&r.hits==2);
    strcpy(pattern.name,"INFAMOUSSSUSER_");CHECK(search(&q,&r)==0&&r.hits==1);
    strcpy(pattern.name,"infamousss*");CHECK(search(&q,&r)==0&&r.hits==0);
    for(int i=0;i<2;++i){snprintf(path,sizeof(path),"%s/%s.sce_sys",base,names[i]);snprintf(file,sizeof(file),"%s/param.bin",path);CHECK(!unlink(file));CHECK(!rmdir(path));snprintf(path,sizeof(path),"%s/%s",base,names[i]);CHECK(!rmdir(path));}
    CHECK(!rmdir(base));CHECK(!rmdir(userdir));
    puts("PASS: save wildcard semantics, empty/count-only queries, case folding, sort before truncation, metadata and bounds.");return 0;
}
