// SPDX-License-Identifier: GPL-2.0-or-later
// Mouse look for the 1.07 eboot (sha256 2d1ca796...): the mouse turns the player camera by an angle.
// The right stick only sets the camera's turning speed, so a mouse fed in as the stick behaves like a
// stick; here its distance is added to the camera's angles instead.
//
// eboot+0x14ddd0 updates the player camera (rdi): it reads the right stick (pad vtable +0x70,
// eboot+0x24d740), shapes it into turn rates and calls eboot+0x14d6f0 at eboot+0x14e95a, which
// integrates them: the new yaw and pitch (radians) go to *angles, the rates to *rates, and the caller
// stores them at camera+0x330 / +0x340. That call goes through a jump in the padding at
// eboot+0x140641 to Integrate below: the original runs, then the mouse's angle is added. Yaw wraps at
// +-pi and the stick to the right lowers it; pitch is kept within the limits of the camera settings
// (rsi+0x1c0 and rdx+0x1c0: min, max; -1.45 to 1.525 on foot). The game's invert options (bytes
// eboot+0xefb3cd x, +0xefb3ce y) apply as they do to the stick.
// camera+0x470 is the right stick's state (0 still, 1 moving, 2 full) and +0x474.. the times it
// changed (eboot+0x149050, clock eboot+0xe162a4): it is set to moving while the mouse moves, so the
// camera's return to its default pitch waits for the mouse to stop as it does for the stick.
// The camera also swings back behind the player by itself (auto-follow). The right stick turns that off
// through the game's ManualCamera event; the mouse does not send it. The only reader of the game's
// "no auto-follow" byte (eboot+0xe16c40) is eboot+0x14936e, in eboot+0x149360, which then takes the
// game's own off path. So the byte is set while the mouse moves and cleared a few seconds after the
// last move (ISS_MOUSE_FOLLOW_HOLD, seconds, default 5; 0 leaves auto-follow alone). Found in
// github.com/axylpup/shadPS4 (src/core/mouse_aim.cpp), checked against this eboot.
#include "mouse_look.h"
#include "ultrawide.h"
#include "../src/runtime.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef ISS_SYSV
#define ISS_SYSV __attribute__((sysv_abi))
#endif
namespace {
constexpr std::uint64_t kIntegrate=0x14d6f0,kCall=0x14e95a,kJump=0x140641,kClock=0xe162a4,kInvertX=0xefb3cd,kInvertY=0xefb3ce,kNoFollow=0xe16c40;
struct Region{std::uint64_t at,size,hash;};
constexpr Region kRegions[]={{kIntegrate,0x5a0,0xacc7d72b517b0c5bULL},{0x14e936,0x29,0xc881e6b33e4db34cULL},{kJump,0xf,0xbf8ae185c163fd17ULL}};
// Radians per mouse count at 100% (mouse_sensitivity): 0.046 degrees, 7854 counts a turn.
constexpr float kRadiansPerCount=0.0008f;

using IntegrateFn=void(ISS_SYSV*)(unsigned char*,unsigned char*,unsigned char*,void*,float*,float*,float,float);
IntegrateFn original;
unsigned char *game;
float scale;
double followHold=5;
std::chrono::steady_clock::time_point lastMove;

// Auto-follow is off from the mouse's last move until followHold seconds later.
void Follow(bool moved){
    if(followHold<=0)return;
    const auto now=std::chrono::steady_clock::now();
    if(moved)lastMove=now;
    game[kNoFollow]=lastMove.time_since_epoch().count()!=0&&std::chrono::duration<double>(now-lastMove).count()<followHold;
}

void ISS_SYSV Integrate(unsigned char *camera,unsigned char *settings,unsigned char *mode,void *target,float *angles,float *rates,float x,float y){
    float dx,dy;
    const bool moved=pad_look_take(&dx,&dy)&&(dx!=0||dy!=0);
    if(moved){
        std::int32_t *state=reinterpret_cast<std::int32_t*>(camera+0x470),now;std::memcpy(&now,game+kClock,4);
        if(state[0]==0)state[1]=now;
        if(state[0]!=1){state[0]=1;state[2]=now;}
    }
    Follow(moved);
    original(camera,settings,mode,target,angles,rates,x,y);
    if(!moved)return;
    if(game[kInvertX])dx=-dx;
    if(game[kInvertY])dy=-dy;
    angles[0]=std::remainder(angles[0]-dx*scale,6.2831853f);
    const float *a=reinterpret_cast<const float*>(settings+0x1c0),*b=reinterpret_cast<const float*>(mode+0x1c0);
    const float low=std::fmax(a[0],b[0]),high=std::fmin(a[1],b[1]),pitch=angles[1]+dy*scale;
    // Within the limits, or no further past them than the game had it.
    angles[1]=dy>0?std::fmax(angles[1],std::fmin(pitch,high)):std::fmin(angles[1],std::fmax(pitch,low));
}
}

namespace iss::native {
int InstallMouseLook(unsigned char *image,std::uint64_t size){
    const char *option=std::getenv("ISS_MOUSE_AIM");
    if(option&&option[0]=='0')return 0;
    for(const Region &r:kRegions)
        if(size<=r.at+r.size||CodeHash(image+r.at,r.size)!=r.hash){std::fputs("MOUSE_LOOK disabled: eboot function identity mismatch\n",stderr);return 0;}
    const char *sens=std::getenv("ISS_MOUSE_SENS");
    const float percent=sens?float(std::atof(sens)):100.f;
    scale=kRadiansPerCount*(percent>0?percent:100.f)/100.f;
    const char *hold=std::getenv("ISS_MOUSE_FOLLOW_HOLD");
    if(hold)followHold=std::atof(hold);
    game=image;original=reinterpret_cast<IntegrateFn>(image+kIntegrate);
    // movabs rax, Integrate; jmp rax (in the padding), and the call to it.
    unsigned char jump[12]={0x48,0xb8};const auto address=reinterpret_cast<std::uintptr_t>(&Integrate);
    std::memcpy(jump+2,&address,8);jump[10]=0xff;jump[11]=0xe0;
    unsigned char call[5]={0xe8};const std::int32_t relative=std::int32_t(kJump-(kCall+5));std::memcpy(call+1,&relative,4);
    if(!WriteCode(image,kJump,jump,sizeof(jump))||!WriteCode(image,kCall,call,sizeof(call)))return -1;
    pad_look_direct=1;
    std::fprintf(stderr,"MOUSE_LOOK installed at eboot+0x14e95a sensitivity=%.0f%%\n",double(percent));
    return 1;
}
}
