// SPDX-License-Identifier: GPL-2.0-or-later
// Preserve the driver's cache during long sessions and diagnostic termination.
#include <windows.h>
#include <cstdio>
#include <mutex>
#include <xxhash.h>
#include "common/perf_stats.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/driver_cache_data.h"

namespace Vulkan {
bool PipelineCache::CheckpointDriverCache(){
    // Vulkan internally synchronizes this cache (no EXTERNALLY_SYNCHRONIZED flag).
    // This mutex serializes our snapshot writers; no compiler or draw is skipped.
    static std::mutex mutex;
    static u64 previous_hash=0;
    std::scoped_lock guard{mutex};
    if(!EmulatorSettings.IsPipelineCacheEnabled()||!pipeline_cache)return false;
    const auto started=Common::Perf::NowNs();
    const auto snapshot=instance.GetDevice().getPipelineCacheData(*pipeline_cache);
    if(snapshot.result!=vk::Result::eSuccess||snapshot.value.size()<32||snapshot.value.size()>256ULL*1024*1024-24){
        std::fprintf(stderr,"PIPELINE_CHECKPOINT unavailable result=%d\n",int(snapshot.result));return false;}
    const u64 hash=XXH3_64bits(snapshot.value.data(),snapshot.value.size());
    if(hash==previous_hash)return true;
    const auto bytes=PackDriverCache(snapshot.value);
    auto temporary=driver_cache_path;temporary+=L".pending-"+std::to_wstring(GetCurrentProcessId());
    bool ok=false;DWORD write_error=0;
    if(FILE *file=std::fopen(temporary.c_str(),"wb")){
        ok=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size()&&std::fflush(file)==0&&fsync(fileno(file))==0;
        write_error=ok?0:DWORD(errno);std::fclose(file);
    }else{std::fprintf(stderr,"PIPELINE_CHECKPOINT open error=%d\n",errno);return false;}
    if(ok)ok=std::rename(temporary.c_str(),driver_cache_path.c_str())==0;
    if(ok)previous_hash=hash;
    std::fprintf(stderr,"PIPELINE_CHECKPOINT ok=%d bytes=%zu elapsed_ms=%.3f error=%lu\n",ok,snapshot.value.size(),double(Common::Perf::NowNs()-started)/1e6,ok?0:write_error?write_error:GetLastError());
    return ok;
}
}
