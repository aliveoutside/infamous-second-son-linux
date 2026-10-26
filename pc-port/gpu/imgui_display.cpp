// SPDX-License-Identifier: GPL-2.0-or-later
// The source presenter draws the game image through ImGui; retain actual rendering.
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>
#include "sdl_window.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderdoc.h"
#include "core/debug_state.h"
#include "overlay.h"

namespace iss::save_ui {
void LoadFonts(ImGuiIO& io);
void Draw(float width, float height, float dt);
bool Active();
}
namespace iss::pad_ui {
void Draw(float width, float height);
bool Shown();
}
namespace iss::menu_ui {
void Draw(float width, float height);
bool Shown();
}
namespace ImGui::Core {
// While the save dialog is up the presenter keeps drawing (the game may stop flipping).
bool MustKeepDrawing(){return iss::save_ui::Active()||iss::pad_ui::Shown()||iss::menu_ui::Shown();}
static const Frontend::WindowSDL *window;
static uint64_t last_tick;
void Initialize(const ::Vulkan::Instance& instance,const Frontend::WindowSDL& w,u32 count,vk::Format format){
    window=&w;ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_DockingEnable;io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize={(float)w.GetWidth(),(float)w.GetHeight()};io.Fonts->AddFontDefault();iss::save_ui::LoadFonts(io);
    ImGui::Vulkan::InitInfo info{
        .instance=instance.GetInstance(),.physical_device=instance.GetPhysicalDevice(),
        .device=instance.GetDevice(),.queue_family=instance.GetPresentQueueFamilyIndex(),
        .queue=instance.GetPresentQueue(),.image_count=count,.min_allocation_size=1024*1024,
        .pipeline_rendering_create_info{.colorAttachmentCount=1,.pColorAttachmentFormats=&format},
        .check_vk_result_fn=[](vk::Result r){if(r!=vk::Result::eSuccess)std::fprintf(stderr,"ImGui Vulkan error: %d\n",int(r));}
    };
    ImGui::Vulkan::Init(info);TextureManager::StartWorker();last_tick=SDL_GetPerformanceCounter();
}
void OnSurfaceFormatChange(vk::Format f){ImGui::Vulkan::OnSurfaceFormatChange(f);}
void Shutdown(vk::Device d){d.waitIdle();TextureManager::StopWorker();ImGui::Vulkan::Shutdown();ImGui::DestroyContext();}
ImGuiID NewFrame(bool reused){
    static unsigned frames=0;++frames;
    static uint64_t capture_tick=0;const uint64_t tick=SDL_GetTicks();
    if(frames==1||frames==30||frames==120||tick-capture_tick>=15000){capture_tick=tick;std::fprintf(stdout,"GPU_PRESENT frame=%u reused=%d tick_ms=%llu\n",frames,int(reused),(unsigned long long)tick);std::fflush(stdout);}
    auto& io=ImGui::GetIO();uint64_t now=SDL_GetPerformanceCounter();
    io.DeltaTime=std::max(0.000001f,float(double(now-last_tick)/double(SDL_GetPerformanceFrequency())));last_tick=now;
    io.DisplaySize={(float)window->GetWidth(),(float)window->GetHeight()};ImGui::NewFrame();
    return ImGui::DockSpaceOverViewport(0,ImGui::GetMainViewport(),int(ImGuiDockNodeFlags_PassthruCentralNode)|int(ImGuiDockNodeFlags_NoTabBar));
}
void Render(const vk::CommandBuffer& command,const vk::ImageView& view,const vk::Extent2D& extent){
    static uint64_t sample_tick=SDL_GetTicksNS(),sample_flips=0;static double fps=0,ms=0;
    const uint64_t now=SDL_GetTicksNS(),flips=DebugState.flip_count.load(std::memory_order_relaxed);
    if(now-sample_tick>=500000000){const auto count=flips-sample_flips;double seconds=double(now-sample_tick)/1e9;fps=double(count)/seconds;ms=count?seconds*1000/double(count):0;sample_tick=now;sample_flips=flips;}
    if(iss_show_fps.load()){char label[128];std::snprintf(label,sizeof(label),"%.1f FPS  |  %.2f ms\nF9: show/hide",fps,ms);
        auto *draw=ImGui::GetForegroundDrawList();ImVec2 pos{16,16},size=ImGui::CalcTextSize(label);
        draw->AddRectFilled(pos,ImVec2{pos.x+size.x+20,pos.y+size.y+16},IM_COL32(0,0,0,190),5);
        draw->AddText(ImVec2{pos.x+10,pos.y+8},IM_COL32(235,245,255,255),label);}
    iss::save_ui::Draw(ImGui::GetIO().DisplaySize.x,ImGui::GetIO().DisplaySize.y,ImGui::GetIO().DeltaTime);
    iss::pad_ui::Draw(ImGui::GetIO().DisplaySize.x,ImGui::GetIO().DisplaySize.y);
    iss::menu_ui::Draw(ImGui::GetIO().DisplaySize.x,ImGui::GetIO().DisplaySize.y);
    ImGui::Render();auto *data=ImGui::GetDrawData();ImGui::Vulkan::UpdateTextures(*data,command);
    if(!data->CmdListsCount)return;
    const vk::RenderingAttachmentInfo color{.imageView=view,.imageLayout=vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore};
    const vk::RenderingInfo info{.renderArea=vk::Rect2D{{0,0},extent},.layerCount=1,.colorAttachmentCount=1,.pColorAttachments=&color};
    command.beginRendering(info);ImGui::Vulkan::RenderDrawData(*data,command);command.endRendering();
}
}
