// SPDX-License-Identifier: GPL-2.0-or-later
#include "iss_gpu.h"
#include "native_shadow.h"
#include "native_gbuffer.h"
#include "native_passes.h"
#include "pause_guard.h"
#include "sparse_copy.h"
#include "readback_hint.h"
#include "infamous_renderer/feedback_identity_registry.h"
#include <SDL3/SDL.h>
#include <atomic>
// The shader preload's progress (vk_pipeline_serialization.cpp): entries read, phase
// (0 not started, 1 reading, 2 finishing the compiles, 3 done), shown in the window title.
namespace iss { extern std::atomic<unsigned> g_preload_entries, g_preload_total; extern std::atomic<int> g_preload_phase; }
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include "g4_conditional_backing_write.h"
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <xxhash.h>
#include "common/elf_info.h"
#include "common/path_util.h"
#include "common/perf_stats.h"
#include "common/rdtsc.h"
#include "common/signal_context.h"
#include "core/libraries/libs.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/kernel/process.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

Frontend::WindowSDL *g_window=nullptr;
namespace Libraries::GnmDriver {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::VideoOut {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::Ajm {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::Videodec {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::Kernel {void RegisterEventQueue(Core::Loader::SymbolsResolver*);}
namespace {
IssGpuCallbacks host{};
struct HostAliasMapping {u64 guest{}, size{}, physical{};};
thread_local std::vector<HostAliasMapping>* alias_visit_output{};
ReadbackHints readback_hints;
std::atomic<bool> explicit_readback_hints{};
struct Symbol {std::string nid,library,module;uintptr_t pointer;};
std::vector<Symbol> symbols;
std::mutex mappings_mutex;
boost::icl::interval_set<VAddr> mappings;
std::thread window_thread;
std::mutex ready_mutex;
std::condition_variable ready_cv;
int ready=0;
}
namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char *nid,const char *library,const char *module,SymbolType,u64 address){symbols.push_back({nid,library,module,address});}
}
namespace Core {
class Emulator {
public:static void Configure(){auto& e=Common::ElfInfo::Instance();e.initialized=true;e.game_serial=host.serial;e.title=host.title;e.sdk_ver=host.sdk_version;e.psf_attributes.raw=host.psf_attributes;e.app_ver="01.00";}
};
void AddressSpace::Register(VAddr address,u64 size,u8 *backing){
    if(!size)return;
    std::scoped_lock guard{mappings_mutex};mappings.add(boost::icl::interval<VAddr>::right_open(address,address+size));
    for(u64 at=address;at<address+size;at+=16384)backing_pages[at>>14]=backing+(at-address);
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions(){std::scoped_lock guard{mappings_mutex};return mappings;}
void AddressSpace::Protect(VAddr address,u64 size,MemoryPermission perms){
    if(!host.protect(address,size,static_cast<u32>(perms)))throw std::runtime_error("GPU protection failed");
}
void MemoryManager::SetRasterizer(Vulkan::Rasterizer *value){rasterizer=value;
    host.visit_mappings([](uint64_t address,uint64_t size,uint8_t *backing){iss_gpu_map(address,size,backing);});
}
u64 MemoryManager::ClampRangeSize(VAddr address,u64 size){return host.clamp(address,size);}
std::optional<MemoryManager::BackingSpan> MemoryManager::DescribeBacking(VAddr address,u64 size){
    if(size==0)return std::nullopt;
    const u64 contiguous=host.clamp(address,size);
    if(contiguous==0)return std::nullopt;
    u8* backing=host.backing(address,contiguous);
    if(!backing)return std::nullopt;
    return BackingSpan{reinterpret_cast<u64>(backing),contiguous};
}
std::optional<std::vector<MemoryManager::BackingAlias>>
MemoryManager::EnumerateBackingAliases(VAddr address,u64 size){
    const auto requested=DescribeBacking(address,size);
    if(!requested||requested->size!=size||
       requested->physical_start>std::numeric_limits<u64>::max()-size)return std::nullopt;
    std::vector<HostAliasMapping> mappings_snapshot;
    mappings_snapshot.reserve(4096); // native mapping table is bounded to 4096 entries
    auto* previous=alias_visit_output;
    alias_visit_output=&mappings_snapshot;
    host.visit_mappings([](u64 guest,u64 length,u8* physical){
        if(alias_visit_output)alias_visit_output->push_back(
            HostAliasMapping{guest,length,reinterpret_cast<u64>(physical)});
    });
    alias_visit_output=previous;
    const u64 physical_end=requested->physical_start+size;
    std::vector<BackingAlias> aliases;
    for(const auto& mapping:mappings_snapshot){
        if(mapping.size==0||mapping.guest>std::numeric_limits<u64>::max()-mapping.size||
           mapping.physical>std::numeric_limits<u64>::max()-mapping.size)return std::nullopt;
        const u64 lo=std::max(requested->physical_start,mapping.physical);
        const u64 hi=std::min(physical_end,mapping.physical+mapping.size);
        if(lo>=hi)continue;
        const u64 guest= mapping.guest+(lo-mapping.physical);
        // A CPU-only VA alias can still write the same physical bytes. The SRT provider
        // must arm it too, or reject the snapshot if PageManager cannot protect it.
        // The native table is append-only. A replaced VA or stale earlier entry must not
        // silently authorize a snapshot of the wrong physical bytes.
        if(host.backing(guest,hi-lo)!=reinterpret_cast<u8*>(lo))return std::nullopt;
        for(const auto& other:mappings_snapshot){
            if(&other==&mapping||other.size==0)continue;
            if(other.guest>std::numeric_limits<u64>::max()-other.size||
               other.physical>std::numeric_limits<u64>::max()-other.size)
                return std::nullopt;
            const u64 overlap_lo=std::max(guest,other.guest);
            const u64 overlap_hi=std::min(guest+(hi-lo),other.guest+other.size);
            if(overlap_lo<overlap_hi &&
               lo+(overlap_lo-guest)!=other.physical+(overlap_lo-other.guest))
                return std::nullopt;
        }
        aliases.push_back(BackingAlias{guest,lo,hi-lo});
        if(aliases.size()>64)return std::nullopt;
    }
    if(aliases.empty())return std::nullopt;
    std::sort(aliases.begin(),aliases.end(),[](const auto& a,const auto& b){
        return a.guest_address<b.guest_address;
    });
    aliases.erase(std::unique(aliases.begin(),aliases.end(),[](const auto& a,const auto& b){
        return a.guest_address==b.guest_address && a.size==b.size &&
               a.physical_start==b.physical_start;
    }),aliases.end());
    return aliases;
}
u64 MemoryManager::MappingEpoch() const noexcept {
    return host.mapping_epoch ? host.mapping_epoch() : 0;
}
bool MemoryManager::WithMappingReadLock(const std::function<bool(u64)>& publish) const {
    if (!host.mapping_lock_shared || !host.mapping_unlock_shared) return false;
    const u64 epoch=host.mapping_lock_shared();
    try {
        const bool accepted=publish(epoch);
        host.mapping_unlock_shared();
        return accepted;
    } catch (...) {
        host.mapping_unlock_shared();
        throw;
    }
}
u64 MemoryManager::PreferredReadbackWindow(VAddr address,u64 fallback){return explicit_readback_hints.load(std::memory_order_relaxed)?readback_hints.WindowFor(address,fallback):fallback;}
void MemoryManager::CopySparseMemory(VAddr address,u8 *dest,u64 size){
    iss_copy_sparse(address,dest,size,host.backing);
}
u64 MemoryManager::HashSparseMemory(VAddr address,u64 size){std::vector<u8> bytes(size);CopySparseMemory(address,bytes.data(),size);return XXH3_64bits(bytes.data(),bytes.size());}
bool MemoryManager::TryWriteBacking(void *address,const void *data,u64 size){
    const auto guest=reinterpret_cast<uintptr_t>(address);
    auto *dest=host.backing(guest,size);
    if(!dest)return false;
    if(rasterizer)rasterizer->WithSrtDirectWrite([&]{std::memcpy(dest,data,size);});
    else std::memcpy(dest,data,size);
    return true;
}
bool MemoryManager::TryWriteBackingIf(void* address,const void* data,u64 size,
                                      u64 expected_native_epoch,
                                      const std::function<bool()>& predicate){
    if(!data||!size||!expected_native_epoch||!predicate||
       !host.mapping_lock_shared||!host.mapping_unlock_shared)return false;
    const auto guest=reinterpret_cast<uintptr_t>(address);
    // Resolve before taking the outer lock: host.backing takes its own shared
    // SRW lock and SRW locks do not promise recursive shared acquisition.
    auto* dest=host.backing(guest,size);
    if(!dest)return false;
    return iss::g4::ConditionalBackingWrite(
        [&](const auto& action){return WithMappingReadLock(action);},
        expected_native_epoch,predicate,[&]{
        if(rasterizer)rasterizer->WithSrtDirectWrite([&]{std::memcpy(dest,data,size);});
        else std::memcpy(dest,data,size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address,u64 size){if(rasterizer)rasterizer->InvalidateMemory(address,size);}
}
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
static std::thread service_thread;
void KernelSignalRequest(){} // Work guard keeps the timer executor alive.
int *PS4_SYSV_ABI __Error(){return host.errno_location();}
u64 PS4_SYSV_ABI sceKernelGetTscFrequency(){return host.tsc_frequency();}
u64 PS4_SYSV_ABI sceKernelReadTsc(){return Common::FencedRDTSC();}
u64 PS4_SYSV_ABI sceKernelGetProcessTime(){return host.process_time_us();}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter(){return host.process_time_us();}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency(){return 1000000;}
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32 *out){if(!out)return static_cast<s32>(0x80020016);*out=host.sdk_version;return 0;}
s32 PS4_SYSV_ABI sceKernelIsNeoMode(){return 0;}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 us){std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
}
namespace Libraries::SystemService {bool IsSplashVisible(){return false;}}
namespace VideoCore {
static std::atomic<u32> screenshot_requests,overlay_screenshot_requests;
void LoadRenderDoc(){}void StartCapture(){}void EndCapture(){}void TriggerCapture(){}
void SetOutputDir(const std::filesystem::path&,const std::string&){}
bool IsRenderDocLoaded(){return false;}
void RequestScreenshot(ScreenshotRequest request){if(request==ScreenshotRequest::GameOnly)++screenshot_requests;else if(request==ScreenshotRequest::WithOverlays)++overlay_screenshot_requests;}
u32 ConsumeGameOnlyScreenshotRequests(){return screenshot_requests.exchange(0);}
u32 ConsumeWithOverlaysScreenshotRequests(){return overlay_screenshot_requests.exchange(0);}
ScreenshotRequests ConsumeScreenshotRequests(){return {ConsumeGameOnlyScreenshotRequests(),ConsumeWithOverlaysScreenshotRequests()};}
}
namespace Common::FS {
static std::map<PathType,std::filesystem::path> paths;
const std::filesystem::path& GetUserPath(PathType type){return paths.at(type);}
std::string GetUserPathString(PathType type){return GetUserPath(type).string();}
void SetUserPath(PathType type,const std::filesystem::path& path){std::filesystem::create_directories(path);paths[type]=path;}
bool ValidatePath(const std::filesystem::path& path){return !path.empty();}
std::string PathToUTF8String(const std::filesystem::path& path){auto s=path.u8string();return std::string(s.begin(),s.end());}
}
extern "C" void iss_gpu_map(uint64_t address,uint64_t size,uint8_t *backing){
    auto *memory=Core::Memory::Instance();memory->GetAddressSpace().Register(address,size,backing);
    if(auto *r=memory->GetRasterizer()){r->RegisterMemory(address,size);r->MapMemory(address,size);}
}
extern "C" int iss_gpu_readback(uint64_t address,uint64_t size){
    if(!size||!host.backing(address,size))return 0;
    auto *renderer=Core::Memory::Instance()->GetRasterizer();if(!renderer)return 0;
    readback_hints.Remember(address);explicit_readback_hints.store(true,std::memory_order_relaxed);
    renderer->ReadMemory(address,size,false,0);return 1;
}
extern "C" int iss_gpu_invalidate(uint64_t address,uint64_t size){
    // File reads are invalidated here instead of left to their page faults (one per 4 KB page:
    // the game streams ~5 MB of textures every ~2 s, ~1470 exceptions).
    if(size<0x1000||!host.backing(address,size))return 0;
    auto *renderer=Core::Memory::Instance()->GetRasterizer();if(!renderer)return 0;
    return renderer->InvalidateMemory(address,size,false)?1:0;
}
extern "C" int iss_gpu_fault(void *context,void *address){
    return Core::Signals::Instance()->DispatchAccessViolation(context,address);
}
extern "C" uintptr_t iss_gpu_resolve(const char *identity){
    std::string id(identity);std::vector<std::string> parts;size_t start=0;
    for(size_t end;(end=id.find('#',start))!=std::string::npos;start=end+1)parts.push_back(id.substr(start,end-start));parts.push_back(id.substr(start));
    if(parts.size()!=5||parts[2]!="1")return 0;
    for(const auto& s:symbols)if(s.nid==parts[0]&&s.library==parts[1]&&s.module==parts[3])return s.pointer;
    return 0;
}
// A short sleep that lasts what it asks (a high resolution waitable timer: ~0.5 ms, whatever
// the process timer resolution), for the renderer's polling waits.
extern "C" void IssPreciseSleepUs(unsigned us){
    timespec ts{time_t(us/1000000),long(us%1000000)*1000};
    clock_nanosleep(CLOCK_MONOTONIC,0,&ts,nullptr);
}
namespace MemoryPatcher {extern u64 g_eboot_address;}
extern "C" int iss_gpu_initialize(const IssGpuCallbacks *callbacks){
    if(!callbacks||callbacks->struct_size!=sizeof(host)||!callbacks->backing||!callbacks->clamp||!callbacks->visit_mappings||!callbacks->protect||!callbacks->mapping_epoch||!callbacks->mapping_lock_shared||!callbacks->mapping_unlock_shared)return -1;
    host=*callbacks;
    // The renderer reports guest code addresses as eboot offsets (readback_relaxed_sites).
    MemoryPatcher::g_eboot_address=host.image_base;
    // The executing image (as native/engine_draw.cpp patches it), not a backing view.
    if(iss::native_shadow::Enabled())
        iss::native_shadow::Install(host.image_base,reinterpret_cast<const unsigned char*>(host.image_base+0x43D590));
    iss::native_gbuffer::Install(host.image_base);
    iss::pause_guard::Install(host.image_base);
    iss::native_passes::Install(host.image_base);
    Core::Emulator::Configure();
    for(int i=0;i<=static_cast<int>(Common::FS::PathType::LicensesDir);++i){
        auto type=static_cast<Common::FS::PathType>(i);Common::FS::SetUserPath(type,std::filesystem::path(host.user_dir)/("gpu-"+std::to_string(i)));}
    Common::FS::SetUserPath(Common::FS::PathType::UserDir,host.user_dir);
    Common::Perf::GetTuning().async_pipelines=0; // Never skip a draw while compiling its pipeline.
    auto& tuning=Common::Perf::GetTuning();
    tuning.stream_dedup=true;tuning.stream_static=true;
    tuning.sync_memo=true;tuning.spec_memo=true;
    tuning.region_summary=true;tuning.tex_memo=true;
    tuning.skip_redundant_binds=true;tuning.shader_params_memo=true;
    tuning.skip_empty_draws=true;tuning.pipeline_key_memo=true;
    tuning.rt_memo=true;tuning.reuse_buffer_checks=true;
    tuning.large_read_memo=true;
    tuning.real_occlusion=true;
    tuning.readback_window_bytes=512*1024;
    tuning.threaded_recording=true;tuning.recorder_spin_us=20;tuning.readback_queue=true;
    tuning.asc_yield_packets=0;
    tuning.asc_queue=false;
    tuning.async_uploads=true;
    tuning.async_written_uploads=1;tuning.scout=1;tuning.scout_fast=1;tuning.bind_prefetch=1;
    window_thread=std::thread([]{
        try{
            g_window=new Frontend::WindowSDL(host.width,host.height,host.title);
            // The window answers from its first instant: the libraries, the renderer and the
            // shader preload start on their own thread while this one pumps the window's
            // messages (they ran here first: the window did not answer for 8 s, 30 s with the
            // launcher's shader cache, and Windows marked it "not responding"; winprobe1
            // 2026-10-18). The title shows the preload's progress meanwhile.
            static Core::Loader::SymbolsResolver resolver;
            std::atomic<int> init_state{0}; // 0 running, 1 done, -1 failed
            std::string init_error;
            std::thread init([&]{
                try{
                    Libraries::Kernel::RegisterEventQueue(&resolver);
                    Libraries::Kernel::work.emplace(boost::asio::make_work_guard(Libraries::Kernel::io_context));
                    Libraries::Kernel::service_thread=std::thread([]{Libraries::Kernel::io_context.run();});Libraries::Kernel::service_thread.detach();
                    Libraries::GnmDriver::RegisterLib(&resolver);Libraries::VideoOut::RegisterLib(&resolver);
                    Libraries::Ajm::RegisterLib(&resolver);
                    Libraries::Videodec::RegisterLib(&resolver);
                    init_state.store(1,std::memory_order_release);
                }catch(const std::exception& e){init_error=e.what();init_state.store(-1,std::memory_order_release);}
            });
            {
                auto last_title=std::chrono::steady_clock::now()-std::chrono::seconds(1);
                std::string shown;
                while(init_state.load(std::memory_order_acquire)==0){
                    if(!g_window->PollEvents()){
                        std::fputs("PORT_STOP window closed while loading\n",stderr);std::fflush(nullptr);
                        ::TerminateProcess(::GetCurrentProcess(),0);
                    }
                    const auto now=std::chrono::steady_clock::now();
                    if(now-last_title>std::chrono::milliseconds(40)){
                        last_title=now;
                        const unsigned entries=iss::g_preload_entries.load(std::memory_order_relaxed);
                        const unsigned total=iss::g_preload_total.load(std::memory_order_relaxed);
                        const int phase=iss::g_preload_phase.load(std::memory_order_relaxed);
                        // Phase 4: the first run compiles the game's compute shaders.
                        std::string status=phase==0?"Starting...":
                            phase==1?"Loading shaders  "+std::to_string(entries)+(total?" / "+std::to_string(total):""):
                            phase==4?"Preparing shaders (first run only)  "+std::to_string(entries)+" / "+std::to_string(total):
                            phase==5?"Indexing the game's shaders (first run only)  "+std::to_string(entries)+" / "+std::to_string(total):
                            "Finishing shaders...";
                        const double fraction=(phase==1||phase==4||phase==5)&&total?double((std::min)(entries,total))/total:
                                              phase>=2?1.0:-1.0;
                        g_window->DrawLoading(fraction,status);
                        std::string title=std::string(host.title)+" - "+status;
                        if(title!=shown){SDL_SetWindowTitle(g_window->GetSDLWindow(),title.c_str());shown=title;}
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                init.join();
                SDL_SetWindowTitle(g_window->GetSDLWindow(),host.title);
                if(init_state.load()<0)throw std::runtime_error(init_error);
            }
            auto *renderer=Core::Memory::Instance()->GetRasterizer();
            if(renderer)renderer->GetPipelineCache().CheckpointDriverCache();
            {std::scoped_lock guard{ready_mutex};ready=1;}ready_cv.notify_all();
            auto last_checkpoint=std::chrono::steady_clock::now(),last_stop_check=last_checkpoint;
            auto last_priority=last_checkpoint-std::chrono::seconds(8);
            for(;;){
                const bool stop=!g_window->PollEvents();auto now=std::chrono::steady_clock::now();
                if(stop){
                    if(renderer)renderer->GetPipelineCache().CheckpointDriverCache();
                    // Our atomic checkpoint replaces the old destructor-only snapshot.
                    Storage::DataBase::Instance().SetBeforeClose({});Storage::DataBase::Instance().Close();
                    std::fputs("PORT_STOP cache writes drained; closing all guest threads and audio\n",stderr);std::fflush(nullptr);
                    ::TerminateProcess(::GetCurrentProcess(),0);return;
                }
                // Off the window thread: a changed driver cache takes ~120 ms to write.
                if(renderer&&now-last_checkpoint>std::chrono::seconds(30)){
                    std::thread([renderer]{renderer->GetPipelineCache().CheckpointDriverCache();}).detach();last_checkpoint=now;}
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }catch(const std::exception& e){std::fprintf(stderr,"GPU initialization failed: %s\n",e.what());{std::scoped_lock guard{ready_mutex};ready=-1;}ready_cv.notify_all();}
    });window_thread.detach();
    std::unique_lock guard{ready_mutex};ready_cv.wait(guard,[]{return ready!=0;});return ready==1?0:-1;
}

// The command processor thread limits the frame rate while several guest threads
// spin on rdtsc waiting for it. The physical core of logical processor L is taken out of the
// process default CPU sets (no other thread of the process runs there, its SMT sibling stays
// idle) and the command processor is given logical processor L alone.
uint64_t IssImageBase() { return host.image_base; }

#ifdef ISS_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
#endif
void IssPinCommandProcessor() {
#ifdef ISS_PGO_GENERATE
    // PGO=generate builds: write the profile every 20 s (merged into the existing files).
    static std::once_flag pgo_once;
    std::call_once(pgo_once, [] {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                __gcov_dump();
                __gcov_reset();
            }
        }).detach();
    });
#endif
    // The core the firmware ranks fastest (ACPI CPPC highest_perf, the
    // "preferred core" that boosts highest), first logical processor of it.
    const auto read_long = [](const std::string& path, long fallback) {
        long v = fallback;
        if (FILE* f = std::fopen(path.c_str(), "r")) {
            if (std::fscanf(f, "%ld", &v) != 1) v = fallback;
            std::fclose(f);
        }
        return v;
    };
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    long wanted = -1;
    long best = -1;
    for (long c = 0; c < ncpu; ++c) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c);
        const long perf = read_long(base + "/acpi_cppc/highest_perf", -1);
        const long first = read_long(base + "/topology/core_cpus_list", c);
        if (perf > best && first == c) { best = perf; wanted = c; }
    }
    if (wanted < 0) wanted = ncpu > 2 ? 1 : 0;
    if (wanted < 0 || wanted >= ncpu || ncpu < 4) return;
    // The core's logical processors (SMT siblings), e.g. "1,7" or "2-3".
    cpu_set_t core;
    CPU_ZERO(&core);
    if (FILE* f = std::fopen(("/sys/devices/system/cpu/cpu" + std::to_string(wanted) +
                              "/topology/thread_siblings_list").c_str(), "r")) {
        char list[64] = {};
        if (std::fgets(list, sizeof(list), f)) {
            for (char* p = list; *p;) {
                char* end;
                const long a = std::strtol(p, &end, 10);
                if (end == p) break;
                long b = a;
                if (*end == '-') b = std::strtol(end + 1, &end, 10);
                for (long c = a; c <= b; ++c) CPU_SET(int(c), &core);
                p = (*end == ',') ? end + 1 : end;
                if (*p == '\n') break;
            }
        }
        std::fclose(f);
    }
    CPU_SET(int(wanted), &core);
    // Every other thread of the process (and the threads they create later) off that core.
    cpu_set_t others;
    CPU_ZERO(&others);
    for (long c = 0; c < ncpu && c < CPU_SETSIZE; ++c)
        if (!CPU_ISSET(int(c), &core)) CPU_SET(int(c), &others);
    const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
        const pid_t tid = static_cast<pid_t>(std::strtol(entry.path().filename().c_str(), nullptr, 10));
        if (tid > 0 && tid != self) sched_setaffinity(tid, sizeof(others), &others);
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(int(wanted), &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return;
}
