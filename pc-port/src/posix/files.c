// SPDX-License-Identifier: GPL-2.0-or-later
// PS4 read-only app mounts and workspace-only writable runtime data.
#include "../runtime.h"
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int64_t sec, nsec; } Timestamp;
typedef struct {
    uint32_t dev,ino; uint16_t mode,nlink; uint32_t uid,gid,rdev;
    Timestamp atime,mtime,ctime; int64_t size,blocks;
    uint32_t blksize,flags,gen; int32_t spare; Timestamp birthtime;
} GuestStat;
_Static_assert(sizeof(GuestStat)==120,"PS4 stat layout");
typedef struct { int host; HostLock lock; int active; char path[1024]; } File;
static File files[4096];
static HostLock registry = HOST_LOCK_INIT;
static pthread_once_t initialize_once = PTHREAD_ONCE_INIT;
static char app_root[1024],user_root[1024],save_root[1200];
static struct { char guest[32],host[1200]; int active,readonly; } mounts[16];
static int32_t error(int n) { return (int32_t)(UINT32_C(0x80020000)|(unsigned)n); }
// FreeBSD (PS4) errno values; the common ones match Linux.
static int host_error(void) {
    switch(errno) {
    case ENOENT: case ENOTDIR: return 2;
    case EACCES: case EPERM: case EROFS: return 13;
    case EBADF: return 9;
    case EEXIST: return 17;
    case ENOSPC: return 28;
    case EINVAL: return 22;
    case EISDIR: return 21;
    default: return 5;
    }
}
static void initialize(void) {
    const char *game=getenv("ISS_GAME_DIR");
    snprintf(app_root,sizeof(app_root),"%s",game?game:".");
    const char *user=getenv("ISS_USER_DIR");
    if(user&&*user)snprintf(user_root,sizeof(user_root),"%s",user);
    else{ssize_t n=readlink("/proc/self/exe",user_root,sizeof(user_root)-8);if(n<0)n=0;user_root[n]=0;
        char *slash=strrchr(user_root,'/');if(slash)*slash=0;strcat(user_root,"/user");}
    mkdir(user_root,0777);
    const char *save_override=getenv("ISS_SAVE_ROOT");
    if(save_override&&*save_override)snprintf(save_root,sizeof(save_root),"%s",save_override);
    else snprintf(save_root,sizeof(save_root),"%s/savedata",user_root);
    const char *names[]={"temp0","download0","data","savedata0"};
    for(unsigned i=0;i<4;++i) { char path[1100];snprintf(path,sizeof(path),"%s/%s",user_root,names[i]);mkdir(path,0777); }
}
// The game's file names are not case exact on every dump: when a read path does not exist as
// spelled, each component is matched ignoring case.
static void fold_case(char *path,size_t root_length) {
    struct stat st;
    if(!stat(path,&st))return;
    char *cursor=path+root_length;
    while(*cursor){
        while(*cursor=='/')++cursor;
        char *end=strchr(cursor,'/');size_t length=end?(size_t)(end-cursor):strlen(cursor);
        if(!length)break;
        char saved=*cursor;(void)saved;
        char directory[2048];size_t parent=(size_t)(cursor-path);
        memcpy(directory,path,parent);directory[parent?parent:0]=0;
        char component[256];if(length>=sizeof(component))return;
        memcpy(component,cursor,length);component[length]=0;
        char probe[2048];snprintf(probe,sizeof(probe),"%s%s",directory,component);
        if(stat(probe,&st)){
            DIR *d=opendir(directory[0]?directory:".");int found=0;
            if(d){struct dirent *e;while((e=readdir(d))){if(!strcasecmp(e->d_name,component)&&strlen(e->d_name)==length){memcpy(cursor,e->d_name,length);found=1;break;}}closedir(d);}
            if(!found)return;
        }
        cursor+=length;
    }
}
static int translate(const char *path,char *out,size_t size,int write) {
    if(!path||!*path) return 2;
    pthread_once(&initialize_once,initialize);
    char normalized[1024];
    int n=snprintf(normalized,sizeof(normalized),"%s%s",path[0]=='/'?"":"/app0/",path);
    if(n<0||(size_t)n>=sizeof(normalized)) return 63;
    if(strchr(normalized,'\\')||strchr(normalized,':')) return 13;
    for(char *p=normalized;*p;++p)
        if((p==normalized||p[-1]=='/')&&p[0]=='.'&&p[1]=='.'&&(p[2]=='/'||!p[2])) return 13;
    const char *suffix=NULL,*base=NULL; char writable[1100];
    for(unsigned i=0;i<16;++i)if(mounts[i].active){size_t length=strlen(mounts[i].guest);
        if(!strncmp(normalized,mounts[i].guest,length)&&(normalized[length]=='/'||!normalized[length])){
            if(write&&mounts[i].readonly)return 30;suffix=normalized+length;base=mounts[i].host;break;}}
    const char *mount_names[]={"/app0","/hostapp","/temp0","/download0","/data","/savedata0"};
    int app=0;
    for(unsigned i=0;!base&&i<6;++i) {
        size_t len=strlen(mount_names[i]);
        if(!strncmp(normalized,mount_names[i],len)&&(normalized[len]=='/'||!normalized[len])) {
            suffix=normalized+len;
            if(i<2) { if(write) return 30; base=app_root;app=1; }
            else {snprintf(writable,sizeof(writable),"%s/%s",user_root,mount_names[i]+1);base=writable;}
            break;
        }
    }
    if(!base) return 2;
    n=snprintf(out,size,"%s%s",base,suffix);
    if(n<0||(size_t)n>=size) return 63;
    if(app&&!write)fold_case(out,strlen(base));
    return 0;
}
const char *runtime_user_root(void){pthread_once(&initialize_once,initialize);return user_root;}
const char *runtime_save_root(void){pthread_once(&initialize_once,initialize);return save_root;}
int runtime_file_mount(unsigned slot,const char *guest,const char *host,int readonly){
    if(slot>=16||!guest||strlen(guest)>=32||!host||strlen(host)>=1200)return -1;
    host_lock(&registry);strcpy(mounts[slot].guest,guest);strcpy(mounts[slot].host,host);
    mounts[slot].readonly=readonly;mounts[slot].active=1;host_unlock(&registry);return 0;
}
void runtime_file_unmount(unsigned slot){if(slot>=16)return;host_lock(&registry);mounts[slot].active=0;host_unlock(&registry);}
static int32_t GUEST_ABI open_file(const char *path,int32_t flags,uint16_t mode) {
    char translated[2048]; int writing=(flags&3)|| (flags&0x600);
    int e=translate(path,translated,sizeof(translated),writing); if(e) return error(e);
    int native=(flags&3)==0?O_RDONLY:(flags&3)==1?O_WRONLY:O_RDWR;
    if(flags&0x200)native|=O_CREAT;if(flags&0x400)native|=O_TRUNC;if(flags&0x800)native|=O_EXCL;
    if(flags&0x20000)native|=O_DIRECTORY;
    int host=open(translated,native|O_CLOEXEC,mode?mode:0644);
    if(host<0) {e=host_error();printf("File open failed: %s errno=%d\n",path,e);return error(e);}
    host_lock(&registry);
    int fd;
    for(fd=3;fd<4096&&files[fd].active;++fd){}
    if(fd==4096){host_unlock(&registry);close(host);return error(24);}
    files[fd].host=host;files[fd].active=1;host_lock_init(&files[fd].lock);
    snprintf(files[fd].path,sizeof(files[fd].path),"%s",path);
    if(flags&8)lseek(host,0,SEEK_END);
    host_unlock(&registry);printf("Opened %s fd=%d\n",path,fd);
    {size_t n=strlen(path);if(n>5&&!strcmp(path+n-5,".xpps"))gpu_file_opened(path);}
    return fd;
}
static File *acquire_file(int fd) {
    host_lock_shared(&registry);
    if(fd<3||fd>=4096||!files[fd].active){host_unlock_shared(&registry);return NULL;}
    host_lock(&files[fd].lock);return &files[fd];
}
static void release_file(File *f){host_unlock(&f->lock);host_unlock_shared(&registry);}
static int32_t GUEST_ABI close_file(int fd) {
    host_lock(&registry);
    if(fd<3||fd>=4096||!files[fd].active){host_unlock(&registry);return error(9);}
    close(files[fd].host);files[fd].active=0;host_unlock(&registry);return 0;
}
static int64_t transfer(int fd,void *buffer,uint64_t size,int writing,int64_t offset,int positioned) {
    if(writing && (fd==1||fd==2)) {
        uint64_t total=0;
        while(total<size){ssize_t n=(ssize_t)((size-total)>0x40000000?0x40000000:size-total);
            n=(ssize_t)(fd==1?fwrite((unsigned char *)buffer+total,1,(size_t)n,stdout):fwrite((unsigned char *)buffer+total,1,(size_t)n,stderr));
            if(n<=0)return total?(int64_t)total:error(host_error());total+=(uint64_t)n;}
        fflush(fd==1?stdout:stderr);return (int64_t)total;
    }
    File *f=acquire_file(fd);if(!f)return error(9);
    if(positioned&&offset<0){release_file(f);return error(22);}
    const int64_t start=lseek(f->host,0,SEEK_CUR);
    uint64_t total=0;int e=0;uint32_t native_error=0;
    // Kernel I/O into a protected page fails with EFAULT instead of raising the GPU tracking
    // fault. Stage through host memory, then copy in user mode, where the fault is handled.
    size_t capacity=(size_t)(size>1048576?1048576:size);
    unsigned char *staging=capacity?malloc(capacity):NULL;
    if(capacity&&!staging)e=12;
    while(!e&&total<size){size_t amount=(size_t)((size-total)>capacity?capacity:size-total);ssize_t done;
        if(writing){memcpy(staging,(unsigned char *)buffer+total,amount);
            done=positioned?pwrite(f->host,staging,amount,(off_t)(offset+(int64_t)total)):write(f->host,staging,amount);}
        else done=positioned?pread(f->host,staging,amount,(off_t)(offset+(int64_t)total)):read(f->host,staging,amount);
        if(done<0){if(errno==EINTR)continue;native_error=(uint32_t)errno;e=host_error();fprintf(stderr,"File transfer failed: fd=%d write=%d bytes=%zu offset=%lld destination=%p errno=%u\n",fd,writing,amount,(long long)offset,(unsigned char *)buffer+total,native_error);break;}
        if(!writing&&done){gpu_invalidate((uint64_t)(uintptr_t)((unsigned char *)buffer+total),(uint64_t)done);
            memcpy((unsigned char *)buffer+total,staging,(size_t)done);}
        total+=(uint64_t)done;if((size_t)done<amount)break;
    }
    free(staging);
    release_file(f);return total?(int64_t)total:e?error(e):0;
}
static int64_t GUEST_ABI read_file(int fd,void *p,uint64_t n){return transfer(fd,p,n,0,0,0);}
static int64_t GUEST_ABI write_file(int fd,const void *p,uint64_t n){return transfer(fd,(void *)p,n,1,0,0);}
static int64_t GUEST_ABI pread_file(int fd,void *p,uint64_t n,int64_t off){return transfer(fd,p,n,0,off,1);}
static int64_t GUEST_ABI pwrite_file(int fd,const void *p,uint64_t n,int64_t off){return transfer(fd,(void *)p,n,1,off,1);}
static int32_t GUEST_ABI fsync_file(int fd){File *f=acquire_file(fd);if(!f)return error(9);int e=fsync(f->host)==0?0:host_error();release_file(f);return e?error(e):0;}
static int32_t GUEST_ABI ftruncate_file(int fd,int64_t size){
    if(size<0)return error(22);File *f=acquire_file(fd);if(!f)return error(9);
    int e=ftruncate(f->host,(off_t)size)==0?0:host_error();release_file(f);return e?error(e):0;
}
static int32_t GUEST_ABI truncate_file(const char *path,int64_t size){int fd=open_file(path,1,0);if(fd<0)return fd;int r=ftruncate_file(fd,size);close_file(fd);return r;}
static int32_t GUEST_ABI mkdir_file(const char *path,uint16_t mode){(void)mode;char host[2048];int e=translate(path,host,sizeof(host),1);if(e)return error(e);return mkdir(host,0777)==0?0:error(host_error());}
static int32_t GUEST_ABI unlink_file(const char *path){char host[2048];int e=translate(path,host,sizeof(host),1);if(e)return error(e);return unlink(host)==0?0:error(host_error());}
static int32_t GUEST_ABI rename_file(const char *from,const char *to){char a[2048],b[2048];int e=translate(from,a,sizeof(a),1);if(e)return error(e);e=translate(to,b,sizeof(b),1);if(e)return error(e);return rename(a,b)==0?0:error(host_error());}
static int64_t GUEST_ABI posix_write(int fd,const void *p,uint64_t n){int64_t r=write_file(fd,p,n);if(r<0){*runtime_errno()=(int32_t)r&65535;return -1;}return r;}
static int64_t GUEST_ABI posix_read(int fd,void *p,uint64_t n){int64_t r=read_file(fd,p,n);if(r<0){*runtime_errno()=(int32_t)r&65535;return -1;}return r;}
static int64_t GUEST_ABI lseek_file(int fd,int64_t off,int origin){
    if(origin<0||origin>2)return error(22);File *f=acquire_file(fd);if(!f)return error(9);
    off_t r=lseek(f->host,(off_t)off,origin);int e=r<0?host_error():0;release_file(f);return r>=0?(int64_t)r:error(e);
}
static Timestamp timestamp(struct timespec t){return (Timestamp){(int64_t)t.tv_sec,(int64_t)t.tv_nsec};}
static int fill_stat(const struct stat *i,GuestStat *s){
    if(!s)return 14;
    memset(s,0,sizeof(*s));s->mode=(uint16_t)((S_ISDIR(i->st_mode)?0040000:0100000)|0444);
    s->ino=(uint32_t)i->st_ino;s->dev=(uint32_t)i->st_dev;s->nlink=(uint16_t)i->st_nlink;
    s->size=(int64_t)i->st_size;s->blocks=(s->size+511)/512;s->blksize=4096;
    s->atime=timestamp(i->st_atim);s->mtime=timestamp(i->st_mtim);s->ctime=s->birthtime=timestamp(i->st_ctim);return 0;
}
static int32_t GUEST_ABI fstat_file(int fd,GuestStat *s){File *f=acquire_file(fd);if(!f)return error(9);struct stat st;int e=fstat(f->host,&st)==0?fill_stat(&st,s):host_error();release_file(f);return e?error(e):0;}
static int32_t GUEST_ABI stat_file(const char *p,GuestStat *s){int fd=open_file(p,0,0);if(fd<0)return fd;int r=fstat_file(fd,s);close_file(fd);return r;}
uintptr_t file_resolve(const char *identity){
    if(!strstr(identity,"#libkernel#1#libkernel#257"))return 0;
    const char *api=runtime_api_name(identity);
#define API(n,f) if(!strcmp(api,n))return (uintptr_t)f
    API("sceKernelOpen",open_file);API("sceKernelClose",close_file);API("sceKernelRead",read_file);
    API("sceKernelWrite",write_file);API("sceKernelPread",pread_file);API("sceKernelLseek",lseek_file);
    API("sceKernelFstat",fstat_file);API("sceKernelStat",stat_file);
    API("sceKernelPwrite",pwrite_file);API("sceKernelFsync",fsync_file);
    API("sceKernelFtruncate",ftruncate_file);API("sceKernelTruncate",truncate_file);API("sceKernelMkdir",mkdir_file);
    API("sceKernelUnlink",unlink_file);API("sceKernelRename",rename_file);
    API("_write",posix_write);API("write",posix_write);API("_read",posix_read);API("read",posix_read);
#undef API
    return 0;
}
