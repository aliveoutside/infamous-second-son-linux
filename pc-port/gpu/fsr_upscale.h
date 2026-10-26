// ISS_FSR: the game's T2x temporal resolve replaced by the FSR 3 upscaler of FSR-Vulkan, at the
// game's own resolution (FSR as anti-aliasing). The rasterizer finds the images and records the
// dispatch (Rasterizer::FsrResolve); this file holds the context and the per-frame constants.
#pragma once

#include <atomic>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
struct Frame;
}

namespace iss::fsr {

struct Image {
    vk::Image image{};
    vk::Format format{};
    vk::ImageUsageFlags usage{};
    vk::ImageAspectFlags aspect{};
    u32 width{};
    u32 height{};
};

/// ISS_FSR=1 at start, then the in-game menu (F1).
bool Enabled();
void SetEnabled(bool on);
/// RCAS sharpening in percent, 0 (off) to 100.
int Sharpness();
void SetSharpness(int percent);

/// One dispatch, for each render of the scene (a hitch can render two before one resolve): the
/// images in the layouts the rasterizer has put them in (colour, motion and depth shader read).
struct Frame {
    Image color, depth, motion;
    Image output; // FSR's own, set by Prepare: copied to the game's target by RecordCopyOut
    float jitter[2]{};
    float mv_scale[2]{};
    float frame_ms{};
    u64 frame_id{};
    int phase{}; // the game's SMAA subsample index: 1 or 2 (T2x), 0 (1x, no jitter) (set by the caller)
    bool reset{}; // the history is dropped (set by the caller, or the first frame)
};

/// Creates the context on the first call. False: FSR cannot run on this device or at this size.
bool Prepare(const Vulkan::Instance& instance, u32 width, u32 height, Frame& frame);

/// The library's checks of the dispatch, before the game's resolve is dropped. False: keep it.
bool Check(const Frame& frame);

/// Records the dispatch into cmdbuf (the recording thread).
void Record(vk::CommandBuffer cmdbuf, const Frame& frame);

/// The output is larger than the render size (fsr_mode below native).
bool Upscaling();

/// The last dispatch's output copied to a game target (transfer dst layout): at the resolve, or
/// when upscaling the pass after it that takes the picture to the output size. A target of the
/// render size gets the output scaled down; swap_rb: red and blue trade places (the upscale pass).
void RecordCopyOut(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Image target, u32 width,
                   u32 height, bool swap_rb = false);

// ISS_FG=1 (with ISS_FSR=1): FSR 3 frame generation, one frame shown between two real ones. At the
// resolve the FSR output (the scene before the HUD) is copied and the optical flow prepared; at the
// flip the game's whole picture is copied and the frame between it and the last one generated.
// Every call runs on the command processor thread, the Record* ones record in its stream order.

/// ISS_FG=1 (at start, then the menu) and FSR on.
bool FgEnabled();
void SetFgEnabled(bool on);

/// For the menu's debug view (counted since the start).
struct Stats {
    std::atomic<u64> fsr_resolves{};  // resolves FSR did
    std::atomic<u64> game_resolves{}; // resolves left to the game while FSR is on
    std::atomic<u64> fg_generated{};  // generated frames presented
    std::atomic<u64> fg_skipped{};    // real flips without one (reset, hitch, menu, refused)
    std::atomic<u64> presents{};      // every frame presented, generated ones too
    std::atomic<u64> double_renders{}; // resolves after two scene renders (FG skips them)
    std::atomic<u64> hitches{};       // real frames far slower than the others (FG skips them)
    std::atomic<u32> last_hitch_ms{}; // the last of those
    std::atomic<u64> flips{};         // flips the game submitted (on the GPU thread)
};
Stats& GetStats();

/// What FgPrepare decided for the frame, recorded later by FgRecordPrepare.
struct FgPrepared {
    float frame_ms{};
    bool reset{};
};

/// At the resolve, after Prepare: creates the context and images on the first call. False: no
/// frame generation for this frame.
bool FgPrepare(const Vulkan::Instance& instance, const Frame& frame, FgPrepared& prepared);

/// After Record: the copy of the output (general layout) and the optical flow.
void FgRecordPrepare(vk::CommandBuffer cmdbuf, const Frame& frame, const FgPrepared& prepared);

/// At the flip: the image the game shows, in transfer source layout.
struct Flip {
    vk::Image image{};
    u32 width{}, height{};
};

/// True when a frame is generated for this flip (a frame was prepared since the last one); show:
/// it is one to present (false on a reset, which only builds the history).
bool FgBeginDispatch(const Flip& flip, bool& show);

/// Copies the flip image and records the generated frame. Leaves the output in shader read
/// layout; its view (FgView) shows the game's BGRA bytes as RGBA, still sRGB encoded.
void FgRecordDispatch(vk::CommandBuffer cmdbuf, const Flip& flip);
vk::ImageView FgView();

/// The generated frame of the last PrepareFrame, taken by the flip that follows it.
void SetGeneratedFrame(Vulkan::Frame* frame);
Vulkan::Frame* TakeGeneratedFrame();

} // namespace iss::fsr
