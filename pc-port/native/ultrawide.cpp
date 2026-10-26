// SPDX-License-Identifier: GPL-2.0-or-later
// Custom resolution, ultrawide field of view and HUD edges for the 1.07 eboot (sha256 2d1ca796...).
// Keys of settings.toml (src/settings.c). Installed before the guest starts (src/posix/native_hooks.cpp); every patched function is identity-checked.
//
// resolution = WxH: the renderer's output and render size. eboot+0x1eff00 (renderer start) and
//   eboot+0x1f0be0 (video mode change) write 1920x1080 outside PS4 Pro mode into the renderer
//   (eboot+0xf1bcd0: output +0/+4, render +0xc/+0x10); every buffer size is derived from them.
//   The cameras take their aspect from the UI screen size, eboot+0x1d0ccc0/c4 (1920x1080, written
//   at eboot+0x1f03f8; camera constructor eboot+0x580140: aspect = w/h, tan(hfov/2) =
//   tan(vfov/2)*aspect). It becomes (1080*W/H)x1080: the vertical field of view is kept and the
//   view widens (Hor+). The UI is laid out in units of 1/1080 of the height, from the centre of the
//   screen (projection eboot+0x4a5000, widget anchors eboot+0x564cf0), so it keeps its size.
// hud_width = a (0 or empty: the full width): the widgets anchored left or right sit at the safe
//   area margin, eboot+0x1d0ccc8 (x) / +0x1d0cccc (y), computed from the system safe area ratio at
//   eboot+0x1f045e, +0x1f2add and +0x1f2bdd. SafeArea() computes the margins of a centred a:1
//   region instead, so the HUD edges stay where a 16:9 (or 21:9) screen would have them. Subtitles
//   wrap in that region too (eboot+0x501c52: 0.6 of the width between the margins).
// fov = p (percent, 100 = the game's): the cameras' tan(fov/2) times p/100. The camera computes
//   it with tanf at eboot+0x5805b9 (SetFov), +0x580f81 (update) and +0x5810e7; the constructor's
//   default tan(0.5) is at eboot+0x5804e7 and eboot+0xae4464. The calls go through a jump in the
//   alignment padding after SetFov (eboot+0x580641, 15 NOP bytes) to ScaledTan.
#include "../src/runtime.h"
#include "../src/settings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <sys/mman.h>
#include <unistd.h>
#ifndef ISS_SYSV
#define ISS_SYSV __attribute__((sysv_abi))
#endif
namespace {
unsigned g_output_w,g_output_h; // the game's output size when the render size is smaller, else 0
struct Region{std::uint64_t at,size,hash;};
// Whole functions (and the padding used for the jump), FNV-1a 64 of the loaded bytes.
constexpr Region kResolution[]={{0x1eff00,0x5d5,0xb20ecde85db657ccULL},{0x1f0be0,0x5ab,0xa0d7e80835318d45ULL},
    {0x1f2a60,0xfb,0xbfa2ea8eb02ed213ULL},{0x1f2b60,0xfb,0x909ce4f53a94a351ULL}};
constexpr Region kFov[]={{0x580140,0x434,0x2a343ca226fd5af6ULL},{0x580580,0xc1,0xef658398f5aad2a4ULL},
    {0x580e80,0x236,0xd5995e3112cf4d59ULL},{0x5810c0,0xaf,0xbc9bd1c2d897fa6bULL},
    {0x580641,0xf,0xbf8ae185c163fd17ULL},{0xae4464,0x4,0xe086055916557c2aULL}};
constexpr std::uint64_t kUiWidth=0x1d0ccc0,kUiHeight=0x1d0ccc4,kSafeX=0x1d0ccc8,kSafeY=0x1d0cccc;
// The safe area code: from the ratio in xmm0 to the stores of both margins (dead after the patch).
constexpr std::uint64_t kSafeAreaSites[][2]={{0x1f045e,0x1f04b6},{0x1f2add,0x1f2b40},{0x1f2bdd,0x1f2c40}};
constexpr std::uint64_t kTanCalls[]={0x5805b9,0x580f81,0x5810e7},kTanJump=0x580641;

std::int32_t *ui_width,*ui_height,*safe_x,*safe_y;
float hud_aspect=0,fov_scale=1;

std::uint64_t Hash(const unsigned char *p,std::size_t size){std::uint64_t h=14695981039346656037ULL;while(size--)h=(h^*p++)*1099511628211ULL;return h;}
bool Matches(const unsigned char *image,std::uint64_t size,const Region *regions,std::size_t count){
    for(std::size_t i=0;i<count;++i)
        if(size<=regions[i].at+regions[i].size||Hash(image+regions[i].at,regions[i].size)!=regions[i].hash)return false;
    return true;
}
// The protection of the mapping that holds address, from /proc/self/maps (-1: not mapped).
int Protection(std::uintptr_t address){
    std::FILE *maps=std::fopen("/proc/self/maps","r");
    if(!maps)return -1;
    char line[512];int protection=-1;
    while(protection<0&&std::fgets(line,sizeof(line),maps)){
        unsigned long long first,end;char mode[5];
        if(std::sscanf(line,"%llx-%llx %4s",&first,&end,mode)!=3||address<first||address>=end)continue;
        protection=(mode[0]=='r'?PROT_READ:0)|(mode[1]=='w'?PROT_WRITE:0)|(mode[2]=='x'?PROT_EXEC:0);
    }
    std::fclose(maps);
    return protection;
}
// Page by page: made writable for the copy, then given back its protection.
bool Write(unsigned char *image,std::uint64_t at,const void *bytes,std::size_t size){
    const std::uintptr_t page=std::uintptr_t(sysconf(_SC_PAGESIZE));
    const std::uintptr_t first=reinterpret_cast<std::uintptr_t>(image+at),end=first+size;
    for(std::uintptr_t p=first&~(page-1);p<end;p+=page){
        const int protection=Protection(p);
        if(protection<0)return false;
        const std::uintptr_t from=std::max(p,first),to=std::min(p+page,end);
        if(!(protection&PROT_WRITE)&&mprotect(reinterpret_cast<void*>(p),page,protection|PROT_WRITE))return false;
        std::memcpy(reinterpret_cast<void*>(from),static_cast<const unsigned char*>(bytes)+(from-first),to-from);
        if(!(protection&PROT_WRITE)&&mprotect(reinterpret_cast<void*>(p),page,protection))return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(first),reinterpret_cast<char*>(end));
    return true;
}
bool Write32(unsigned char *image,std::uint64_t at,std::uint32_t value){return Write(image,at,&value,4);}
std::uint32_t Bits(float value){std::uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
float Float(std::uint32_t bits){float value;std::memcpy(&value,&bits,4);return value;}

// The game's rule (eboot+0x1f045e): margin = (size - int(size*ratio))/2, at least 16 units. The x
// margin is taken in the HUD region and moved in by the space left on each side of it.
void ISS_SYSV SafeArea(float ratio){
    const std::int32_t width=*ui_width,height=*ui_height;
    std::int32_t region=width;
    if(hud_aspect>0){
        const std::int32_t hud=std::int32_t(std::lround(height*hud_aspect))&~1;
        if(hud>0&&hud<width)region=hud;
    }
    const std::int32_t x=(region-std::int32_t(float(region)*ratio))/2,y=(height-std::int32_t(float(height)*ratio))/2;
    *safe_x=(x<16?16:x)+(width-region)/2;
    *safe_y=y<16?16:y;
}
float ISS_SYSV ScaledTan(float half_fov){return std::tan(half_fov)*fov_scale;}

// movabs rax, target; call rax; jmp short to the end of the replaced code. The rest is int3.
bool CallAndSkip(unsigned char *image,std::uint64_t from,std::uint64_t to,const void *target){
    unsigned char code[0x80];
    const std::size_t size=std::size_t(to-from);
    if(size<14||size-14>127||size>sizeof(code))return false;
    std::memset(code,0xcc,size);
    code[0]=0x48;code[1]=0xb8;const auto address=reinterpret_cast<std::uintptr_t>(target);std::memcpy(code+2,&address,8);
    code[10]=0xff;code[11]=0xd0;code[12]=0xeb;code[13]=static_cast<unsigned char>(size-14);
    return Write(image,from,code,size);
}

bool ParseResolution(const char *text,std::uint32_t &width,std::uint32_t &height){
    char *end=nullptr;
    const unsigned long w=std::strtoul(text,&end,10);
    if(end==text||(*end!='x'&&*end!='X'))return false;
    const char *h_text=end+1;
    const unsigned long h=std::strtoul(h_text,&end,10);
    if(end==h_text||*end)return false;
    // Multiples of 8: the renderer halves and quarters these sizes for its buffers.
    width=std::uint32_t(w)&~7u;height=std::uint32_t(h)&~7u;
    return width>=640&&width<=7680&&height>=360&&height<=4320;
}

int InstallResolution(unsigned char *image,std::uint64_t size){
    const char *option=settings_get("resolution");
    if(!option||!*option)return 0;
    std::uint32_t width=1920,height=1080;
    if(option&&*option&&!ParseResolution(option,width,height)){std::fprintf(stderr,"ULTRAWIDE rejected: ISS_RESOLUTION=%s (expected WxH)\n",option);return -1;}
    if(width==1920&&height==1080)return 0;
    if(!Matches(image,size,kResolution,std::size(kResolution))){std::fputs("ULTRAWIDE rejected: eboot function identity mismatch\n",stderr);return -1;}
    if(const char *hud=settings_get("hud_width"))hud_aspect=float(std::atof(hud));
    if(!(hud_aspect>=0.5f&&hud_aspect<=8.f))hud_aspect=0;
    // The UI screen: 1080 high, as wide as the picture's aspect (even, the game halves it).
    const std::uint32_t ui_w=std::uint32_t(std::lround(1080.0*width/height))&~1u,ui_h=1080;
    // The forward shaders (ps 10f721b6, 6f3cd2c9) walk per-16x16-tile light lists, written by
    // the light culling shader (cs ce1675e0) into a fixed buffer, until a 0xffff terminator. Past
    // the buffer's end no list is written and the walk never ends: the GPU hung with too many
    // tiles (title screen at 5120x1440 and 5120x1424, a cutscene at 4984x1400; 3840x1080 played
    // through). The picture is rendered with at most 256 tiles, the game's own
    // PS4 Pro size (3200x1800: 200x113 = 22600), in the same aspect; the window scales it up.
    const std::uint32_t requested_w=width,requested_h=height;
    constexpr std::uint64_t max_tiles=200*113;
    const auto tile_count=[](std::uint32_t w,std::uint32_t h){return std::uint64_t((w+15)/16)*((h+15)/16);};
    if(tile_count(width,height)>max_tiles){
        const double scale=std::sqrt(double(max_tiles)/double(tile_count(width,height)));
        std::uint32_t w=std::uint32_t(width*scale)&~7u;
        while(w>=640){
            const std::uint32_t h=std::uint32_t(double(w)*requested_h/requested_w)&~7u;
            if(tile_count(w,h)<=max_tiles){width=w;height=h;break;}
            w-=8;
        }
    }
    ui_width=reinterpret_cast<std::int32_t*>(image+kUiWidth);ui_height=reinterpret_cast<std::int32_t*>(image+kUiHeight);
    safe_x=reinterpret_cast<std::int32_t*>(image+kSafeX);safe_y=reinterpret_cast<std::int32_t*>(image+kSafeY);
    // mov dword [rbx+0xc], render width; [rbx+0x10], render height; [rbx], output width; [rbx+4], output height.
    bool ok=Write32(image,0x1eff47 + 3,width)&&Write32(image,0x1eff4e + 3,height)&&
            Write32(image,0x1eff55 + 2,width)&&Write32(image,0x1eff5b + 3,height)&&
            // The video mode change: mov esi, 1920 / mov edi, 1080 (the non-Pro output and render size).
            Write32(image,0x1f0d3d + 1,width)&&Write32(image,0x1f0d4c + 1,height);
    if(width!=1920||height!=1080){
        // mov dword [r15], UI height; mov dword [r12], UI width.
        ok=ok&&Write32(image,0x1f03f8 + 3,ui_h)&&Write32(image,0x1f03ff + 4,ui_w);
        for(const auto &site:kSafeAreaSites)ok=ok&&CallAndSkip(image,site[0],site[1],reinterpret_cast<const void*>(&SafeArea));
    }
    if(!ok)return -1;
    std::fprintf(stderr,"ULTRAWIDE resolution=%ux%u output=%ux%u ui=%ux%u hud_aspect=%.4f tiles=%llu\n",
                 requested_w,requested_h,width,height,ui_w,ui_h,double(hud_aspect),
                 (unsigned long long)tile_count(width,height));
    return 1;
}

int InstallFov(unsigned char *image,std::uint64_t size){
    const char *option=settings_get("fov");
    if(!option||!*option)return 0;
    const double percent=std::atof(option);
    if(!(percent>=50&&percent<=200)){std::fprintf(stderr,"ULTRAWIDE rejected: fov=%s (50 to 200)\n",option);return -1;}
    if(percent==100)return 0;
    if(!Matches(image,size,kFov,std::size(kFov))){std::fputs("ULTRAWIDE rejected: camera function identity mismatch\n",stderr);return -1;}
    fov_scale=float(percent/100);
    // movabs rax, ScaledTan; jmp rax: a jump within reach of the rel32 calls.
    unsigned char jump[12]={0x48,0xb8};const auto target=reinterpret_cast<std::uintptr_t>(&ScaledTan);
    std::memcpy(jump+2,&target,8);jump[10]=0xff;jump[11]=0xe0;
    bool ok=Write(image,kTanJump,jump,sizeof(jump));
    for(const std::uint64_t call:kTanCalls){
        unsigned char code[5]={0xe8};const std::int32_t relative=std::int32_t(kTanJump-(call+5));
        std::memcpy(code+1,&relative,4);ok=ok&&Write(image,call,code,sizeof(code));
    }
    // The constructor's default, tan(0.5): stored at eboot+0x5804e7, times the aspect via eboot+0xae4464.
    std::uint32_t original;std::memcpy(&original,image+0xae4464,4);
    ok=ok&&Write32(image,0x5804e7 + 6,Bits(Float(original)*fov_scale))&&Write32(image,0xae4464,Bits(Float(original)*fov_scale));
    if(!ok)return -1;
    std::fprintf(stderr,"ULTRAWIDE fov=%.0f%%\n",percent);
    return 1;
}
}

namespace iss::native {
std::uint64_t CodeHash(const unsigned char *code,std::size_t size){return Hash(code,size);}
bool WriteCode(unsigned char *image,std::uint64_t at,const void *bytes,std::size_t size){return Write(image,at,bytes,size);}
int InstallUltrawide(unsigned char *image,std::uint64_t size){
    const int resolution=InstallResolution(image,size);
    if(resolution<0)return resolution;
    const int fov=InstallFov(image,size);
    if(fov<0)return fov;
    return resolution+fov;
}
}

// Exported from iss-probe (CMakeLists.txt) and found by libiss-gpu.so with dlsym: the output size FSR
// scales the render size up to, or 0 when the game renders at its output size.
extern "C" int iss_output_size(unsigned *width,unsigned *height){
    *width=g_output_w;*height=g_output_h;
    return g_output_w!=0;
}
