// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime.h"
#include "pad_panel.h"
#include <dlfcn.h>
#include "../gpu/iss_gpu.h"
#include "../gpu/iss_dialog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uintptr_t (*resolve_import)(const char *);
static int (*handle_fault)(void *,void *);
static int (*readback_memory)(uint64_t,uint64_t);
static int (*invalidate_memory)(uint64_t,uint64_t);
// A pack of the game opened: its shaders go first in the ahead-of-time compiler.
static void (*file_opened)(const char *);
// The save data dialog drawn by libiss-gpu.so over the game (gpu/save_dialog_ui.cpp).
static int (*dialog_open)(const IssDialog *);
static int (*dialog_poll)(IssDialogResult *);
static void (*dialog_close)(void);
static int (*dialog_active)(void);
// The in-game menu (F1, gpu/ingame_menu.cpp): up or not, and the settings it changed.
static int (*menu_shown)(void);
static int (*menu_take)(char *,size_t,char *,size_t);
// The window's first size: 1280x720, or 720 high in the aspect of ISS_RESOLUTION (native/ultrawide.cpp).
// The picture is fitted to whatever size the window gets.
static void window_size(uint32_t *width,uint32_t *height){
    *width=1280;*height=720;
    const char *resolution=getenv("ISS_RESOLUTION");unsigned w=0,h=0;
    if(!resolution||sscanf(resolution,"%ux%u",&w,&h)!=2||!w||!h)return;
    const double aspect=(double)w/h;
    double window_w=720*aspect,window_h=720;
    *width=(uint32_t)window_w&~1u;*height=(uint32_t)window_h&~1u;
}
int gpu_start(uint32_t sdk_version,uint64_t image_base){
    char path[2048];ssize_t length=readlink("/proc/self/exe",path,sizeof(path)-16);if(length<0)return -1;path[length]=0;
    char *slash=strrchr(path,'/');if(!slash)return -1;strcpy(slash,"/libiss-gpu.so");
    void *library=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!library){fprintf(stderr,"GPU library load failed: %s\n",dlerror());return -1;}
    int (*initialize)(const IssGpuCallbacks *)=(void *)dlsym(library,"iss_gpu_initialize");
    resolve_import=(void *)dlsym(library,"iss_gpu_resolve");handle_fault=(void *)dlsym(library,"iss_gpu_fault");
    readback_memory=(void *)dlsym(library,"iss_gpu_readback");
    invalidate_memory=(void *)dlsym(library,"iss_gpu_invalidate");
    file_opened=(void *)dlsym(library,"iss_gpu_file_opened");
    void (*mapped)(uint64_t,uint64_t,uint8_t *)=(void *)dlsym(library,"iss_gpu_map");
    dialog_open=(void *)dlsym(library,"iss_gpu_dialog_open");dialog_poll=(void *)dlsym(library,"iss_gpu_dialog_poll");
    dialog_close=(void *)dlsym(library,"iss_gpu_dialog_close");dialog_active=(void *)dlsym(library,"iss_gpu_dialog_active");
    menu_shown=(void *)dlsym(library,"iss_gpu_menu_shown");menu_take=(void *)dlsym(library,"iss_gpu_menu_take");
    if(!initialize||!resolve_import||!handle_fault||!mapped)return -1;
    IssGpuCallbacks callbacks={0};callbacks.struct_size=sizeof(callbacks);callbacks.sdk_version=sdk_version;
    callbacks.psf_attributes=545259522;window_size(&callbacks.width,&callbacks.height);
    callbacks.serial="CUSA00004";callbacks.title="inFAMOUS Second Son";callbacks.user_dir=runtime_user_root();
    callbacks.tsc_frequency=runtime_tsc_frequency;callbacks.process_time_us=runtime_process_time_us;
    callbacks.errno_location=runtime_errno;callbacks.backing=memory_backing;callbacks.clamp=memory_clamp;
    callbacks.visit_mappings=memory_visit;callbacks.protect=memory_gpu_protect;
    callbacks.image_base=image_base;
    callbacks.mapping_epoch=memory_mapping_epoch;
    callbacks.mapping_lock_shared=memory_mapping_lock_shared;
    callbacks.mapping_unlock_shared=memory_mapping_unlock_shared;
    pad_panel_bind(dlsym(library,"iss_gpu_pad_panel_toggle"),dlsym(library,"iss_gpu_pad_panel_read"),
                   dlsym(library,"iss_gpu_pad_panel_mode"),dlsym(library,"iss_gpu_pad_panel_covers"),
                   dlsym(library,"iss_gpu_pad_panel_report"));
    if(initialize(&callbacks))return -1;memory_set_map_hook(mapped);
    puts("Vulkan renderer initialized; waiting for guest graphics submissions.");return 0;
}
uintptr_t gpu_resolve(const char *name){return resolve_import?resolve_import(name):0;}
int gpu_dialog_available(void){return dialog_open&&dialog_poll&&dialog_close;}
int gpu_dialog_open(const void *dialog){return dialog_open?dialog_open((const IssDialog *)dialog):-1;}
int gpu_dialog_poll(void *result){return dialog_poll?dialog_poll((IssDialogResult *)result):-1;}
void gpu_dialog_close(void){if(dialog_close)dialog_close();}
int gpu_dialog_active(void){return dialog_active?dialog_active():0;}
int gpu_menu_shown(void){return menu_shown?menu_shown():0;}
int gpu_menu_take(char *key,size_t key_size,char *value,size_t value_size){return menu_take?menu_take(key,key_size,value,value_size):0;}
int gpu_readback(uint64_t address,uint64_t size){return readback_memory?readback_memory(address,size):0;}
int gpu_invalidate(uint64_t address,uint64_t size){return invalidate_memory?invalidate_memory(address,size):0;}
void gpu_file_opened(const char *guest_path){if(file_opened)file_opened(guest_path);}
int gpu_handle_fault(void *ucontext,void *address){return handle_fault?handle_fault(ucontext,address):0;}
