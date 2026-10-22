// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/visual_capture.h"
#include "video_core/renderdoc.h"
#include <cstdio>
#include <cstdlib>
static unsigned requests{};
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"VISUAL CAPTURE FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
namespace VideoCore {void RequestScreenshot(ScreenshotRequest r){CHECK(r==ScreenshotRequest::GameOnly);++requests;}}
int main(){
    setenv("ISS_VISUAL_SEQUENCE","1",1);
    CHECK(iss::visual_capture::Enabled());CHECK(iss::visual_capture::Start(1000));
    CHECK(!iss::visual_capture::Start(1001));
    iss::visual_capture::OnFrame(999,1,false);CHECK(requests==0);
    iss::visual_capture::OnFrame(1000,2,false);CHECK(requests==1);
    iss::visual_capture::OnFrame(1001,3,true);CHECK(requests==1);
    for(unsigned i=1;i<5;++i)iss::visual_capture::OnFrame(1000+375000000ull*i,4+i,false);
    CHECK(requests==5);
    iss::visual_capture::OnFrame(3000000000ull,10,false);CHECK(requests==5);
    CHECK(iss::visual_capture::Start(4000000000ull));
    iss::visual_capture::OnFrame(4000000000ull,11,false);CHECK(requests==6);
    std::puts("Visual capture: spacing, five requests, busy rejection and sequence reuse passed");
}
