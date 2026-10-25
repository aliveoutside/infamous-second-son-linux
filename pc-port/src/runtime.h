// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "host.h"
#include <stddef.h>
#include <stdint.h>
#define GUEST_ABI __attribute__((sysv_abi))
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t base, size, init, tls_address, tls_filesz, tls_memsz, tls_align, tls_id;
} GuestModule;
void runtime_configure(unsigned char *image, uint64_t image_size, const GuestModule *modules,
                       uint64_t count, uint64_t procparam);
void runtime_tls_attach(void);
void runtime_tls_detach(void);
uintptr_t runtime_resolve(const char *identity, int data);
// Linux: guest fs: accesses are rewritten to gs: once, before the guest runs.
int runtime_patch_tcb_accesses(unsigned char *code, uint64_t size);
uint64_t runtime_tls_fault_count(void);
uintptr_t sync_resolve(const char *identity);
uintptr_t thread_resolve(const char *identity);
uintptr_t time_resolve(const char *identity);
uintptr_t file_resolve(const char *identity);
uintptr_t service_resolve(const char *identity);
uintptr_t dialog_resolve(const char *identity);
uintptr_t np_resolve(const char *identity);
uintptr_t network_resolve(const char *identity);
uintptr_t save_resolve(const char *identity);
uintptr_t trophy_resolve(const char *identity);
uintptr_t pad_resolve(const char *identity);
// Mouse look (native/mouse_look.cpp): set when the camera takes the mouse's angle directly; the mouse
// counts gathered since the camera's last update (0: none, or the mouse does not aim).
extern int pad_look_direct;
int pad_look_take(float *dx, float *dy);
void pad_graffiti_seen(void); // a graffiti stencil read the gyro aim (native/graffiti.cpp)
uintptr_t audio_resolve(const char *identity);
uintptr_t kernel_sync_resolve(const char *identity);
uintptr_t http_resolve(const char *identity);
const char *runtime_user_root(void);
const char *runtime_save_root(void);
// The save data dialog of libiss-gpu.so (gpu/iss_dialog.h), when the library has it.
int gpu_dialog_available(void);
int gpu_dialog_open(const void *dialog);
int gpu_dialog_poll(void *result);
void gpu_dialog_close(void);
int gpu_dialog_active(void);
// The in-game menu (F1): 1 while it is up; one setting it changed (settings.toml key, value), 0: none.
int gpu_menu_shown(void);
int gpu_menu_take(char *key,size_t key_size,char *value,size_t value_size);
int runtime_file_mount(unsigned slot,const char *guest,const char *host,int readonly);
void runtime_file_unmount(unsigned slot);
void *memory_allocate_image(uint64_t size);
void memory_register_image(uint64_t offset,uint64_t size,uint32_t flags);
uint8_t *memory_backing(uint64_t address,uint64_t size);
uint64_t memory_clamp(uint64_t address,uint64_t size);
void memory_visit(void (*visit)(uint64_t,uint64_t,uint8_t *));
uint64_t memory_mapping_epoch(void);
uint64_t memory_mapping_lock_shared(void);
void memory_mapping_unlock_shared(void);
int memory_gpu_protect(uint64_t address,uint64_t size,uint32_t permissions);
void memory_set_map_hook(void (*hook)(uint64_t,uint64_t,uint8_t *));
uint64_t runtime_tsc_frequency(void);
uint64_t runtime_process_time_us(void);
int gpu_start(uint32_t sdk_version,uint64_t image_base);
uintptr_t gpu_resolve(const char *identity);
int gpu_handle_fault(void *ucontext, void *address);
int gpu_readback(uint64_t address,uint64_t size);
int gpu_invalidate(uint64_t address,uint64_t size);
int gpu_feedback_identity_available(void);
void gpu_file_opened(const char *guest_path);
void gpu_feedback_identity(uint64_t owner_object,uint64_t shader_object,uint64_t record_address);
int native_install_hooks(unsigned char *image,uint64_t size);
const char *runtime_api_name(const char *identity);
// content.c: the player's game data, loaded by content_load() before the guest starts.
enum { CONTENT_MAX_CHUNKS = 1000, CONTENT_MAX_TROPHIES = 128 };
extern int content_package_complete;
extern unsigned content_chunk_count, content_trophy_count;
extern uint64_t content_chunk_sizes[CONTENT_MAX_CHUNKS];
extern unsigned char content_trophy_ids[CONTENT_MAX_TROPHIES];
int content_load(const char *dir);
void runtime_set_thread_handle(void *handle);
int32_t *runtime_errno(void);
uint64_t GUEST_ABI memory_direct_size(void);
int32_t GUEST_ABI memory_allocate_direct(int64_t start, int64_t end, uint64_t length,
                                        uint64_t alignment, int32_t type, int64_t *result);
int32_t GUEST_ABI memory_map_direct(void **address, uint64_t length, int32_t prot,
                                   int32_t flags, int64_t physical, uint64_t alignment);
#ifdef __cplusplus
}
#endif
