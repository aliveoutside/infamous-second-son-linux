// Adapted from bloodborne_pc@4d4d20812c0af23a11c8b8d40f5e06a9054925ef, GPL-2.0-or-later.
// bbport: shadPS4 settings used by the video core, read once from BB_* environment
// variables (defaults match shadPS4 except the pipeline cache, which is on).
#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>
#include "common/types.h"

enum GpuReadbacksMode : int { Disabled, Relaxed, Precise };

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance() { static EmulatorSettingsImpl s; return &s; }
    static long Number(const char* name, long fallback) {
        const char* v = std::getenv(name);
        return v ? std::strtol(v, nullptr, 10) : fallback;
    }
    s32 GetGpuId() { return -1; }
    u32 GetInternalScreenWidth() { return 1920; }
    u32 GetInternalScreenHeight() { return 1080; }
    std::string GetPresentMode() {
        static const std::string value = [] {
            const char* v = std::getenv("BB_PRESENT_MODE");
            return std::string{v ? v : "Mailbox"};
        }();
        return value;
    }
    int GetRcasAttenuation() { return 250; }
    // Precise rather than Relaxed: without readbacks the game reads stale GPU-written vertices
    // (vertex explosions).
    u32 GetReadbacksMode() { return GpuReadbacksMode::Precise; }
    // The guest flips on the emulated vblank at 240 Hz: a finished frame is presented as soon as it
    // arrives (IsUncappedVblank) instead of on the next of 60 ticks (at 60 Hz a 20 ms frame waited
    // 33.3 ms, and frames alternated 10/20 ms at 100 Hz). BB_FPS_LIMIT paces the game itself.
    u32 GetVblankFrequency() { return 240; }
    bool IsUncappedVblank() { return true; }
    /// Frames per second the present thread lets through; 0 = no limit (BB_FPS_LIMIT).
    u32 GetFrameLimit() {
        static const u32 value = u32(std::max(0L, Number("BB_FPS_LIMIT", 0)));
        return value;
    }
    bool IsCopyGpuBuffers() { return false; }
    bool IsDirectMemoryAccessEnabled() { return false; }
    bool IsDumpShaders() { return false; }
    bool IsFsrEnabled() { return false; }
    bool IsHdrAllowed() { return false; }
    bool IsInlineFetchShader() { return false; }
    bool IsNullGPU() { return false; }
    bool IsPatchShaders() { return false; }
    bool IsPipelineCacheArchived() { return false; }
    bool IsPipelineCacheEnabled() { return true; }
    bool IsRcasEnabled() { return true; }
    bool IsReadbackLinearImagesEnabled() { return false; }
    bool IsReadbacksWriteDiscard() { return false; }
    bool IsRenderdocEnabled() { return false; }
    bool IsShaderCollect() { return false; }
    bool IsUserfaultfdTracking() { return false; }
    bool IsVkCrashDiagnosticEnabled() { return false; }
    bool IsVkGuestMarkersEnabled() { return false; }
    bool IsVkHostMarkersEnabled() { return false; }
    bool IsVkValidationCoreEnabled() { return true; }
    bool IsVkValidationEnabled() { return false; }
    bool IsVkValidationGpuEnabled() { return false; }
    bool IsVkValidationSyncEnabled() { return false; }
};
#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())
