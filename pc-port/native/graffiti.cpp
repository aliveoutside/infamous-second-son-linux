// SPDX-License-Identifier: GPL-2.0-or-later
// Graffiti without a gyro, for the 1.07 eboot: the stencil scripts first wait for the controller to be
// held sideways (GYRO_Y_AXIS, eboot+0x24e060, the pad's vtable +0xe0), then aim the spray can with
// GYRO_RETICLE_POS (eboot+0x24e020, vtable +0xd0, the aim that eboot+0x24b790 makes of the
// controller's orientation). No other script calls either. Their entries in the script function table
// (function pointer, then the name 0x20 further) are pointed at Hook below, which tells src/pad.c that
// a stencil is on screen (pad_graffiti_seen) and runs the original. Between the two the stencil asks
// for a shake: the pad sets the SprayShake button (0x1000000) when it is shaken held sideways
// (eboot+0x24b340), and the minigame's own code tests it with the pad's button tests (its vtable at
// eboot+0xcddf70, +0 pressed and +8 down, the mask in esi), hooked by Button below. pad.c then turns
// the mouse or the right stick into the can (see pad_spray_step), held sideways.
#include "graffiti.h"
#include "ultrawide.h"
#include "../src/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <xmmintrin.h>
#ifndef ISS_SYSV
#define ISS_SYSV __attribute__((sysv_abi))
#endif
namespace {
using ScriptFn=__m128(ISS_SYSV*)(void*);
struct Entry{std::uint64_t function,slot,hash;const char *name;ScriptFn original;};
Entry entries[]={{0x24e020,0xd3a5d0,0xe7b1d682a0871f35ULL,"GYRO_RETICLE_POS"},{0x24e060,0xd3a690,0x4c2db9d55a1bb865ULL,"GYRO_Y_AXIS"}};
template<int i> __m128 ISS_SYSV Hook(void *self){pad_graffiti_seen();return entries[i].original(self);}
const ScriptFn hooks[]={Hook<0>,Hook<1>};

constexpr std::uint32_t kSprayShake=0x1000000;
// The pad's button tests, vtable eboot+0xcddf70: +0 pressed (mask, time in xmm0), +8 down (mask).
using ButtonFn=std::uint64_t(ISS_SYSV*)(void*,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,float);
struct Query{std::uint64_t function,slot,hash;const char *name;ButtonFn original;};
Query queries[]={{0x248b40,0xcddf70,0x47b85551a15a0d8aULL,"pressed"},{0x248ad0,0xcddf78,0x098b4a0eee67c3d1ULL,"down"}};
template<int i> std::uint64_t ISS_SYSV Button(void *pad,std::uint64_t mask,std::uint64_t a,std::uint64_t b,std::uint64_t c,float time){
    if(mask&kSprayShake){
        static bool logged;if(!logged){logged=true;std::fprintf(stderr,"GRAFFITI shake asked (%s)\n",queries[i].name);}
        pad_graffiti_seen();
    }
    return queries[i].original(pad,mask,a,b,c,time);
}
const ButtonFn buttons[]={Button<0>,Button<1>};
// Checks a script table slot (function pointer, its name 0x20 further) so a different layout is left
// alone. The table holds relocated pointers (image+offset).
bool Matches(unsigned char *image,std::uint64_t size,std::uint64_t function,std::uint64_t slot,std::uint64_t name_at,std::uint64_t hash,const char *expected){
    const std::uint64_t base=reinterpret_cast<std::uintptr_t>(image);
    if(size<=slot+0x28||size<=name_at+8||iss::native::CodeHash(image+function,0x18)!=hash){std::fputs("GRAFFITI disabled: eboot function identity mismatch\n",stderr);return false;}
    std::uint64_t at,name;std::memcpy(&at,image+slot,8);std::memcpy(&name,image+name_at,8);
    if(at!=base+function||name<base||name>=base+size||std::strcmp(reinterpret_cast<const char*>(name),expected)){
        std::fputs("GRAFFITI disabled: script table mismatch\n",stderr);return false;}
    return true;
}
}

namespace iss::native {
int InstallGraffiti(unsigned char *image,std::uint64_t size){
    for(const Entry &e:entries)if(!Matches(image,size,e.function,e.slot,e.slot+0x20,e.hash,e.name))return 0;
    const std::uint64_t base=reinterpret_cast<std::uintptr_t>(image);
    for(const Query &q:queries){
        std::uint64_t at;std::memcpy(&at,image+q.slot,8);
        if(size<=q.slot+8||at!=base+q.function||iss::native::CodeHash(image+q.function,0x18)!=q.hash){
            std::fputs("GRAFFITI disabled: pad button test mismatch\n",stderr);return 0;}
    }
    for(int i=0;i<2;i++){
        entries[i].original=reinterpret_cast<ScriptFn>(image+entries[i].function);
        const std::uint64_t hook=reinterpret_cast<std::uintptr_t>(hooks[i]);
        if(!WriteCode(image,entries[i].slot,&hook,8))return -1;
    }
    for(int i=0;i<2;i++){
        queries[i].original=reinterpret_cast<ButtonFn>(image+queries[i].function);
        const std::uint64_t hook=reinterpret_cast<std::uintptr_t>(buttons[i]);
        if(!WriteCode(image,queries[i].slot,&hook,8))return -1;
    }
    std::fputs("GRAFFITI stencil detection installed\n",stderr);
    return 1;
}
}
