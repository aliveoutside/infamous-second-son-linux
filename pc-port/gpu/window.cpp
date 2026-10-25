// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "overlay.h"
#include "runtime_timeline.h"
#include "sdl_window.h"
#include "visual_capture.h"
namespace iss::save_ui { bool OnEvent(const SDL_Event& e); }
namespace iss::pad_ui { bool OnEvent(const SDL_Event& e); }
namespace iss::menu_ui { bool OnEvent(const SDL_Event& e); }
namespace Frontend {
WindowSDL::WindowSDL(s32 w,s32 h,const char *title):width{w},height{h},base_title{title}{
    // ISS_WINDOW_SIZE=1920x1080: the window's size (the game's own picture is 1080p either way).
    if(const char* size=std::getenv("ISS_WINDOW_SIZE")){int sw=0,sh=0;if(std::sscanf(size,"%dx%d",&sw,&sh)==2&&sw>=320&&sh>=180){w=sw;h=sh;width=w;height=h;}}
    if(!SDL_InitSubSystem(SDL_INIT_VIDEO))throw std::runtime_error(SDL_GetError());
    const char* display_mode=std::getenv("ISS_FULLSCREEN");
    const bool fullscreen=display_mode&&display_mode[0]=='1';
    const char* hidden_env=std::getenv("ISS_HYBRID_BENCH_HIDDEN");
    const bool hidden=hidden_env && hidden_env[0]=='1';
    window=SDL_CreateWindow(title,w,h,SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE|
        (hidden?SDL_WINDOW_HIDDEN:(fullscreen?SDL_WINDOW_FULLSCREEN:0)));
    if(!window)throw std::runtime_error(SDL_GetError());
    int pixel_width=w,pixel_height=h;
    SDL_GetWindowSizeInPixels(window,&pixel_width,&pixel_height);
    width=pixel_width;height=pixel_height;
    if(const char* icon_path=std::getenv("ISS_WINDOW_ICON")){
        if(auto* icon=SDL_LoadBMP(icon_path)){
            SDL_SetWindowIcon(window,icon);SDL_DestroySurface(icon);
        }else std::fprintf(stderr,"WINDOW_ICON failed: %s\n",SDL_GetError());
    }
    std::fprintf(stderr,"WINDOW_MODE fullscreen=%d size=%dx%d\n",fullscreen,pixel_width,pixel_height);
    const auto properties=SDL_GetWindowProperties(window);
    if(!SDL_strcmp(SDL_GetCurrentVideoDriver(),"x11")){
        window_info.type=WindowSystemType::X11;
        window_info.display_connection=SDL_GetPointerProperty(properties,SDL_PROP_WINDOW_X11_DISPLAY_POINTER,nullptr);
        window_info.render_surface=reinterpret_cast<void*>(SDL_GetNumberProperty(properties,SDL_PROP_WINDOW_X11_WINDOW_NUMBER,0));
    }else{
        window_info.type=WindowSystemType::Wayland;
        window_info.display_connection=SDL_GetPointerProperty(properties,SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER,nullptr);
        window_info.render_surface=SDL_GetPointerProperty(properties,SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,nullptr);
    }
    window_info.render_surface_scale=SDL_GetWindowDisplayScale(window);
}
WindowSDL::~WindowSDL(){if(window)SDL_DestroyWindow(window);}
bool WindowSDL::PollEvents(){SDL_Event e;while(SDL_PollEvent(&e)){
    if(iss::save_ui::OnEvent(e))continue; // the save dialog takes the input while it is up
    if(iss::menu_ui::OnEvent(e))continue; // the in-game menu (F1) for the mouse on it
    if(iss::pad_ui::OnEvent(e))continue;  // so does the touchpad panel (F5) for the mouse on it
    if(e.type==SDL_EVENT_KEY_DOWN&&!e.key.repeat){
        if(e.key.key==SDLK_RETURN&&(e.key.mod&SDL_KMOD_ALT)){
            const bool fullscreen=(SDL_GetWindowFlags(window)&SDL_WINDOW_FULLSCREEN)!=0;
            if(!SDL_SetWindowFullscreen(window,!fullscreen))
                std::fprintf(stderr,"WINDOW_MODE toggle failed: %s\n",SDL_GetError());
        }
        if(e.key.key==SDLK_F9){bool show=!iss_show_fps.load();iss_show_fps.store(show);std::fprintf(stderr,"FPS_OVERLAY visible=%d tick_ms=%llu\n",show,(unsigned long long)SDL_GetTicks());}
        if(e.key.key==SDLK_F10){iss::timeline::Emit("user_stutter_mark",0,0,SDL_GetTicks());std::fprintf(stderr,"PERF_MARK tick_ms=%llu\n",(unsigned long long)SDL_GetTicks());std::fflush(stderr);if(iss::visual_capture::Enabled())iss::visual_capture::Start(SDL_GetTicksNS());}}
    if(e.type==SDL_EVENT_QUIT||e.type==SDL_EVENT_WINDOW_CLOSE_REQUESTED)is_open=false;
    if(e.type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED){width=e.window.data1;height=e.window.data2;}}
    return is_open;
}

// The loading screen while the renderer starts and the shaders preload (RPCS3 style): the game's
// own art (sce_sys/pic1.png, the crow emblem; pic0.png if missing, from ISS_GAME_DIR, as shadPS4's launcher
// shows it), a shade at the bottom, the status and a progress bar. Drawn with GDI+ straight on the
// window until the game's first frame replaces it (ISS_LOADING_SCREEN=0 off).
void WindowSDL::DrawLoading(double,const std::string&){}
void WindowSDL::BeginTextInput(const std::string& initial,const std::string& prompt){std::scoped_lock guard{text_mutex};text=initial;text_prompt=prompt;text_state=0;text_requested=true;}
int WindowSDL::PollTextInput(std::string& value){std::scoped_lock guard{text_mutex};value=text;return text_state;}
void WindowSDL::UpdateTextTitle(){}
void WindowSDL::UpdateCursor(){}
}
