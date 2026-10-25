#include "fsr_upscale.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include <vk_mem_alloc.h>

#include "ffx_vk_portable.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace iss::fsr {

namespace {

// The images owned here hold the game's bytes (BGRA, sRGB encoded) as RGBA8 UNORM: FFX writes its
// outputs as storage images, which sRGB formats cannot be; the views read by the presenter decode
// them.
constexpr vk::Format kOwnedFormat = vk::Format::eR8G8B8A8Unorm;

struct Owned {
    vk::Image image{};
    VmaAllocation allocation{};
    vk::ImageUsageFlags usage{};
    u32 width{}, height{};
};

bool CreateOwned(const Vulkan::Instance& instance, Owned& owned, u32 width, u32 height,
                 vk::ImageUsageFlags usage, vk::ImageCreateFlags flags = {},
                 vk::Format format = kOwnedFormat) {
    const vk::ImageCreateInfo info{
        .flags = flags,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    };
    const VmaAllocationCreateInfo alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    const VkImageCreateInfo c_info = static_cast<VkImageCreateInfo>(info);
    if (vmaCreateImage(instance.GetAllocator(), &c_info, &alloc, &image, &owned.allocation,
                       nullptr) != VK_SUCCESS) {
        return false;
    }
    owned.image = vk::Image{image};
    owned.usage = usage;
    owned.width = width;
    owned.height = height;
    return true;
}

Image AsImage(const Owned& owned) {
    return Image{.image = owned.image, .format = kOwnedFormat, .usage = owned.usage,
                 .aspect = vk::ImageAspectFlagBits::eColor, .width = owned.width,
                 .height = owned.height};
}

vk::ImageMemoryBarrier2 Barrier(vk::Image image, vk::PipelineStageFlags2 src_stage,
                                vk::AccessFlags2 src_access, vk::PipelineStageFlags2 dst_stage,
                                vk::AccessFlags2 dst_access,
                                vk::ImageLayout old_layout = vk::ImageLayout::eGeneral,
                                vk::ImageLayout new_layout = vk::ImageLayout::eGeneral) {
    return vk::ImageMemoryBarrier2{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1,
                          .layerCount = 1},
    };
}

void Copy(vk::CommandBuffer cmdbuf, vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
          vk::ImageLayout dst_layout, u32 width, u32 height) {
    const vk::ImageCopy region{
        .srcSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .dstSubresource{.aspectMask = vk::ImageAspectFlagBits::eColor, .layerCount = 1},
        .extent = {width, height, 1},
    };
    cmdbuf.copyImage(src, src_layout, dst, dst_layout, region);
}

struct State {
    FfxVkPortableUpscaleContext* context{};
    FfxVkPortableUpscaleCreateInfo create{};
    Owned output; // every scene render's FSR output, copied to the game's target at the resolve
    Owned down;   // upscaling: the output scaled back to the render size, for the resolve's target
    Owned swapped; // upscaling: the output with red and blue swapped (B8G8R8A8), for the upscale pass
    u32 width{}, height{};         // the render size
    u32 out_width{}, out_height{}; // the output size: the game's (ISS_FSR_OUTPUT_SIZE) or the render size
    bool failed{};
    u64 frame{};
    std::chrono::steady_clock::time_point last{};
};
State g;

int EnvInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return v && v[0] ? std::atoi(v) : fallback;
}

float EnvFloat(const char* name, float fallback) {
    const char* v = std::getenv(name);
    return v && v[0] ? static_cast<float>(std::atof(v)) : fallback;
}

FfxVkPortableDeviceInfo DeviceInfo(const Vulkan::Instance& instance) {
    FfxVkPortableDeviceInfo device{};
    device.structSize = sizeof(device);
    device.instance = static_cast<VkInstance>(instance.GetInstance());
    device.physicalDevice = static_cast<VkPhysicalDevice>(instance.GetPhysicalDevice());
    device.device = static_cast<VkDevice>(instance.GetDevice());
    device.getDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;
    device.queue = static_cast<VkQueue>(instance.GetGraphicsQueue());
    device.queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex();
    // The device is created with these (vk_instance.cpp), when the GPU has them.
    device.shaderFloat16Enabled = VK_TRUE;
    device.synchronization2Enabled = VK_TRUE;
    device.shaderStorageImageWriteWithoutFormatEnabled = VK_TRUE;
    return device;
}

FfxVkPortableImage Convert(const Image& image, FfxVkPortableResourceState state) {
    FfxVkPortableImage out{};
    out.structSize = sizeof(out);
    out.image = static_cast<VkImage>(image.image);
    out.format = static_cast<VkFormat>(image.format);
    out.extent = {image.width, image.height};
    out.mipCount = 1;
    out.arrayLayers = 1;
    out.usage = static_cast<VkImageUsageFlags>(image.usage);
    out.aspect = static_cast<VkImageAspectFlags>(image.aspect);
    out.state = state;
    return out;
}

} // namespace

namespace {
std::atomic<bool> g_enabled{EnvInt("ISS_FSR", 0) == 1};
std::atomic<bool> g_fg_enabled{EnvInt("ISS_FG", 0) == 1};
std::atomic<int> g_sharpness{std::clamp(EnvInt("ISS_FSR_SHARPNESS", 0), 0, 100)};
std::atomic<bool> g_restart{}; // switched on again: the history is stale
std::atomic<bool> g_fg_restart{}; // FG or FSR switched: what FG prepared before is stale
Stats g_stats;
} // namespace

int Sharpness() {
    return g_sharpness.load(std::memory_order_relaxed);
}

void SetSharpness(int percent) {
    g_sharpness = std::clamp(percent, 0, 100);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void SetEnabled(bool on) {
    if (on && !Enabled()) {
        g_restart = true;
    }
    g_fg_restart = true;
    g_enabled = on;
}

Stats& GetStats() {
    return g_stats;
}

bool Prepare(const Vulkan::Instance& instance, u32 width, u32 height, Frame& frame) {
    if (g.failed) {
        return false;
    }
    const bool debug = EnvInt("ISS_FSR_DEBUG", 0) != 0;
    if (!g.context) {
        const auto device = DeviceInfo(instance);
        FfxVkPortableUpscaleCreateInfo create{};
        create.structSize = sizeof(create);
        create.flags = FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED | FFX_VK_PORTABLE_CONTEXT_DEPTH_INFINITE;
        if (debug) {
            create.flags |= FFX_VK_PORTABLE_CONTEXT_DEBUG_CHECKING;
        }
        // fsr_mode below native: the picture is drawn smaller than the game's output
        // (native/ultrawide.cpp) and FSR scales it up to that.
        u32 out_w = width, out_h = height;
        if (const char* v = std::getenv("ISS_FSR_OUTPUT_SIZE")) {
            u32 w = 0, h = 0;
            if (std::sscanf(v, "%ux%u", &w, &h) == 2 && w >= width && h >= height) {
                out_w = w;
                out_h = h;
            }
        }
        create.maxRenderSize = {width, height};
        create.maxOutputSize = {out_w, out_h};
        const auto result = ffxVkPortableUpscaleContextCreate(&device, &create, &g.context);
        if (result != FFX_VK_PORTABLE_OK) {
            std::fprintf(stderr, "FSR: context create failed (%d), the game's own resolve stays\n",
                         int(result));
            g.failed = true;
            return false;
        }
        using U = vk::ImageUsageFlagBits;
        if (!CreateOwned(instance, g.output, out_w, out_h,
                         U::eStorage | U::eSampled | U::eTransferSrc) ||
            ((out_w != width || out_h != height) &&
             (!CreateOwned(instance, g.down, width, height, U::eTransferDst | U::eTransferSrc) ||
              !CreateOwned(instance, g.swapped, out_w, out_h, U::eTransferDst | U::eTransferSrc, {},
                           vk::Format::eB8G8R8A8Unorm)))) {
            std::fprintf(stderr, "FSR: output allocation failed, the game's own resolve stays\n");
            g.failed = true;
            return false;
        }
        g.create = create;
        g.width = width;
        g.height = height;
        g.out_width = out_w;
        g.out_height = out_h;
        std::fprintf(stderr, "FSR: context created at %ux%u, output %ux%u\n", width, height, out_w,
                     out_h);
    }
    if (width != g.width || height != g.height) {
        return false;
    }
    frame.output = AsImage(g.output);

    // The game's own jitter: +-0.25 render px diagonally, by the SMAA T2x subsample index of the
    // frame. SMAA's reference pairs index 1 with (0.25, -0.25) in clip space (y up), +0.25 +0.25
    // in FSR's pixels (y down). ISS_FSR_JITTER bit 0 flips x, bit 1 flips y.
    const int combo = EnvInt("ISS_FSR_JITTER", 0);
    const float amp = EnvFloat("ISS_FSR_JITTER_AMP", 0.25f);
    const float j = frame.phase == 1 ? amp : -amp;
    frame.jitter[0] = (combo & 1) ? -j : j;
    frame.jitter[1] = (combo & 2) ? -j : j;
    // The game's vectors are UV, current minus previous; FSR wants pixels towards the previous
    // position. ISS_FSR_MV bit 0 flips x, bit 1 flips y.
    const int mv = EnvInt("ISS_FSR_MV", 0);
    frame.mv_scale[0] = ((mv & 1) ? 1.0f : -1.0f) * float(width);
    frame.mv_scale[1] = ((mv & 2) ? 1.0f : -1.0f) * float(height);

    const auto now = std::chrono::steady_clock::now();
    frame.frame_ms = g.frame == 0 ? 16.667f
                                  : std::chrono::duration<float, std::milli>(now - g.last).count();
    frame.reset = frame.reset || g.frame == 0 || g_restart.exchange(false);
    frame.frame_id = g.frame;
    g.last = now;
    if (frame.frame_ms < 1.0f || frame.frame_ms > 200.0f) {
        frame.frame_ms = 16.667f;
    }
    if (g.frame < 3 || (g.frame & 1023) == 0) {
        std::fprintf(stderr, "FSR: frame %llu phase %d jitter %+.3f %+.3f mv scale %.0f %.0f dt %.2f ms\n",
                     (unsigned long long)g.frame, frame.phase, frame.jitter[0], frame.jitter[1], frame.mv_scale[0],
                     frame.mv_scale[1], frame.frame_ms);
    }
    ++g.frame;
    return true;
}

namespace {

float CameraNear() {
    static const float v = EnvFloat("ISS_FSR_NEAR", 0.1f);
    return v;
}
float CameraFar() {
    static const float v = EnvFloat("ISS_FSR_FAR", 5000.0f);
    return v;
}
float CameraFov() {
    static const float v = EnvFloat("ISS_FSR_FOV", 1.0f);
    return v;
}

FfxVkPortableUpscaleDispatchInfo Fill(VkCommandBuffer cmdbuf, const Frame& frame) {
    FfxVkPortableUpscaleDispatchInfo d{};
    d.structSize = sizeof(d);
    d.commandBuffer = cmdbuf;
    // Optional inputs left out: a null image, but its struct size set, or the dispatch is refused.
    d.exposure.structSize = sizeof(d.exposure);
    d.reactiveMask.structSize = sizeof(d.reactiveMask);
    d.transparencyAndCompositionMask.structSize = sizeof(d.transparencyAndCompositionMask);
    d.color = Convert(frame.color, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    d.depth = Convert(frame.depth, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = Convert(frame.motion, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    d.output = Convert(frame.output, FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {frame.jitter[0], frame.jitter[1]};
    d.motionVectorScale = {frame.mv_scale[0], frame.mv_scale[1]};
    d.renderSize = {frame.color.width, frame.color.height};
    d.outputSize = {frame.output.width, frame.output.height};
    d.frameTimeMilliseconds = frame.frame_ms;
    d.preExposure = 1.0f;
    d.cameraNear = CameraNear();
    d.cameraFar = CameraFar();
    d.cameraVerticalFovRadians = CameraFov();
    d.viewSpaceToMeters = 1.0f;
    const int sharpness = g_sharpness.load(std::memory_order_relaxed);
    d.sharpness = sharpness / 100.0f;
    d.enableSharpening = sharpness > 0 ? VK_TRUE : VK_FALSE;
    d.reset = frame.reset ? VK_TRUE : VK_FALSE;
    d.frameId = frame.frame_id;
    return d;
}

} // namespace

bool Check(const Frame& frame) {
    // Any non-null handle: the command buffer is the recorder's, known only when it runs.
    const auto d = Fill(reinterpret_cast<VkCommandBuffer>(uintptr_t{1}), frame);
    const u64 issues = ffxVkPortableValidateUpscaleDispatchInfo(&g.create, &d);
    if (issues) {
        static int reported = 0;
        if (reported++ < 8) {
            std::fprintf(stderr, "FSR: dispatch refused (issues %#llx), the game's resolve stays\n",
                         (unsigned long long)issues);
        }
        return false;
    }
    return true;
}

void Record(vk::CommandBuffer cmdbuf, const Frame& frame) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    // The last output was copied out (RecordCopyOut) or not wanted: written whole again.
    const auto ready = Barrier(frame.output.image, Stage::eTransfer | Stage::eComputeShader,
                               Access::eTransferRead | Access::eShaderRead, Stage::eComputeShader,
                               Access::eShaderWrite, vk::ImageLayout::eUndefined);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ready});
    const auto d = Fill(static_cast<VkCommandBuffer>(cmdbuf), frame);
    if (const auto result = ffxVkPortableUpscaleContextRecordDispatch(g.context, &d);
        result != FFX_VK_PORTABLE_OK) {
        static int reported = 0;
        if (reported++ < 8) {
            std::fprintf(stderr, "FSR: dispatch failed (%d)\n", int(result));
        }
    }
}

bool Upscaling() {
    return g.context && (g.out_width != g.width || g.out_height != g.height);
}

void RecordCopyOut(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Image target, u32 width,
                   u32 height, bool swap_rb) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto written = Barrier(frame.output.image, Stage::eComputeShader | Stage::eTransfer,
                                 Access::eShaderWrite, Stage::eTransfer, Access::eTransferRead);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &written});
    const vk::ImageSubresourceLayers layers{.aspectMask = vk::ImageAspectFlagBits::eColor,
                                            .layerCount = 1};
    if (swap_rb && g.swapped.image && width == g.swapped.width && height == g.swapped.height) {
        // A blit from RGBA to BGRA keeps the channels, so the bytes trade places; then copied.
        const auto to_dst = Barrier(g.swapped.image, Stage::eTransfer, Access::eTransferRead,
                                    Stage::eTransfer, Access::eTransferWrite,
                                    vk::ImageLayout::eUndefined);
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
        const vk::ImageBlit blit{
            .srcSubresource = layers,
            .srcOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{int(width), int(height), 1}},
            .dstSubresource = layers,
            .dstOffsets = std::array{vk::Offset3D{0, 0, 0}, vk::Offset3D{int(width), int(height), 1}},
        };
        cmdbuf.blitImage(frame.output.image, vk::ImageLayout::eGeneral, g.swapped.image,
                         vk::ImageLayout::eGeneral, blit, vk::Filter::eNearest);
        const auto to_src = Barrier(g.swapped.image, Stage::eTransfer, Access::eTransferWrite,
                                    Stage::eTransfer, Access::eTransferRead);
        cmdbuf.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src});
        Copy(cmdbuf, g.swapped.image, vk::ImageLayout::eGeneral, target,
             vk::ImageLayout::eTransferDstOptimal, width, height);
        return;
    }
    if (width == frame.output.width && height == frame.output.height) {
        Copy(cmdbuf, frame.output.image, vk::ImageLayout::eGeneral, target,
             vk::ImageLayout::eTransferDstOptimal, width, height);
        return;
    }
    // The render-size target (upscaling): scaled down in the owned format, then copied as bytes (a
    // blit straight to the game's sRGB image would encode the sRGB bytes again).
    const auto to_dst = Barrier(g.down.image, Stage::eTransfer, Access::eTransferRead,
                                Stage::eTransfer, Access::eTransferWrite,
                                vk::ImageLayout::eUndefined);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_dst});
    const vk::ImageBlit blit{
        .srcSubresource = layers,
        .srcOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{int(frame.output.width), int(frame.output.height), 1}},
        .dstSubresource = layers,
        .dstOffsets = std::array{vk::Offset3D{0, 0, 0},
                                 vk::Offset3D{int(g.down.width), int(g.down.height), 1}},
    };
    cmdbuf.blitImage(frame.output.image, vk::ImageLayout::eGeneral, g.down.image,
                     vk::ImageLayout::eGeneral, blit, vk::Filter::eLinear);
    const auto to_src = Barrier(g.down.image, Stage::eTransfer, Access::eTransferWrite,
                                Stage::eTransfer, Access::eTransferRead);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_src});
    Copy(cmdbuf, g.down.image, vk::ImageLayout::eGeneral, target,
         vk::ImageLayout::eTransferDstOptimal, std::min(width, g.down.width),
         std::min(height, g.down.height));
}

// Frame generation.

namespace {

struct FgState {
    FfxVkPortableFrameGenerationContext* context{};
    FfxVkPortableFrameGenerationCreateInfo create{};
    Owned hudless, current, output;
    vk::ImageView view{}; // the output as the presenter samples the flip image
    u32 width{}, height{};
    bool failed{};
    bool prepared{};   // a Prepare is queued since the last dispatch
    bool reset{true};  // the next Prepare drops the history
    bool prepared_reset{}; // the prepared frame dropped it: no frame to generate from
    u64 frame_id{};    // the prepared frame
    float frame_ms{16.667f};
    std::chrono::steady_clock::time_point last{};
    bool last_valid{}; // last is a frame of this run of FG
    float average_ms{16.667f}; // between real frames, hitches left out
    u64 dispatches{}, shown{};
};
FgState fg;
Vulkan::Frame* g_generated{};

bool CreateFg(const Vulkan::Instance& instance, u32 width, u32 height) {
    fg.width = width;
    fg.height = height;
    using U = vk::ImageUsageFlagBits;
    const auto source = U::eSampled | U::eTransferSrc | U::eTransferDst;
    if (!CreateOwned(instance, fg.hudless, width, height, source) ||
        !CreateOwned(instance, fg.current, width, height, source) ||
        !CreateOwned(instance, fg.output, width, height, U::eSampled | U::eStorage,
                     vk::ImageCreateFlagBits::eMutableFormat |
                         vk::ImageCreateFlagBits::eExtendedUsage)) {
        std::fprintf(stderr, "FG: image allocation failed, no frame generation\n");
        return false;
    }
    // The flip image's own view (vk_presenter.cpp GetFrameViewFormat): its bytes are BGRA.
    const vk::ImageViewUsageCreateInfo view_usage{.usage = U::eSampled};
    const vk::ImageViewCreateInfo view_info{
        .pNext = &view_usage,
        .image = fg.output.image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eB8G8R8A8Srgb,
        .components = {.a = vk::ComponentSwizzle::eOne},
        .subresourceRange{.aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1,
                          .layerCount = 1},
    };
    auto [result, view] = instance.GetDevice().createImageView(view_info);
    if (result != vk::Result::eSuccess) {
        std::fprintf(stderr, "FG: output view failed, no frame generation\n");
        return false;
    }
    fg.view = view;

    const auto device = DeviceInfo(instance);
    FfxVkPortableFrameGenerationCreateInfo create{};
    create.structSize = sizeof(create);
    create.flags = g.create.flags;
    create.maxRenderSize = {width, height};
    create.displaySize = {width, height};
    create.interpolationSourceFormat = static_cast<VkFormat>(kOwnedFormat);
    create.outputFormat = static_cast<VkFormat>(kOwnedFormat);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationCreateInfo(&create)) {
        std::fprintf(stderr, "FG: create info refused (issues %#llx)\n",
                     (unsigned long long)issues);
        return false;
    }
    const auto r = ffxVkPortableFrameGenerationContextCreate(&device, &create, &fg.context);
    if (r != FFX_VK_PORTABLE_OK) {
        std::fprintf(stderr, "FG: context create failed (%d), no frame generation\n", int(r));
        return false;
    }
    fg.create = create;
    std::fprintf(stderr, "FG: context created at %ux%u\n", width, height);
    return true;
}

void Report(const char* what, u64 issues) {
    static int reported = 0;
    if (reported++ >= 8) {
        return;
    }
    std::fprintf(stderr, "FG: %s refused (issues %#llx):", what, (unsigned long long)issues);
    for (u64 bit = 1; bit; bit <<= 1) {
        if (issues & bit) {
            std::fprintf(stderr, " %s", ffxVkPortableValidationIssueName(bit));
        }
    }
    std::fprintf(stderr, "\n");
}

FfxVkPortableFrameGenerationPrepareInfo FillPrepare(VkCommandBuffer cmdbuf, const Frame& frame,
                                                    const FgPrepared& prepared) {
    FfxVkPortableFrameGenerationPrepareInfo p{};
    p.structSize = sizeof(p);
    p.commandBuffer = cmdbuf;
    p.depth = Convert(frame.depth, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    p.motionVectors = Convert(frame.motion, FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
    p.renderSize = {frame.color.width, frame.color.height};
    p.jitterOffset = {frame.jitter[0], frame.jitter[1]};
    p.motionVectorScale = {frame.mv_scale[0], frame.mv_scale[1]};
    p.frameTimeMilliseconds = prepared.frame_ms;
    p.cameraNear = CameraNear();
    p.cameraFar = CameraFar();
    p.cameraVerticalFovRadians = CameraFov();
    p.viewSpaceToMeters = 1.0f;
    p.minLuminance = 0.00001f;
    p.maxLuminance = 1000.0f;
    p.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    p.cameraUp = {0.0f, 1.0f, 0.0f};
    p.cameraRight = {1.0f, 0.0f, 0.0f};
    p.cameraForward = {0.0f, 0.0f, 1.0f};
    p.reset = prepared.reset ? VK_TRUE : VK_FALSE;
    p.frameId = frame.frame_id;
    return p;
}

FfxVkPortableFrameGenerationDispatchInfo FillDispatch(VkCommandBuffer cmdbuf) {
    FfxVkPortableFrameGenerationDispatchInfo d{};
    d.structSize = sizeof(d);
    d.commandBuffer = cmdbuf;
    d.currentColor = Convert(AsImage(fg.current), FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    d.hudlessColor = Convert(AsImage(fg.hudless), FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    d.distortionField.structSize = sizeof(d.distortionField);
    d.output = Convert(AsImage(fg.output), FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
    d.displaySize = {fg.width, fg.height};
    d.interpolationRect = {0, 0, fg.width, fg.height}; // zero is refused, not the whole frame
    d.frameTimeMilliseconds = fg.frame_ms;
    d.cameraNear = CameraNear();
    d.cameraFar = CameraFar();
    d.cameraVerticalFovRadians = CameraFov();
    d.viewSpaceToMeters = 1.0f;
    d.minLuminance = 0.00001f;
    d.maxLuminance = 1000.0f;
    d.transferFunction = FFX_VK_PORTABLE_TRANSFER_FUNCTION_SRGB;
    d.frameId = fg.frame_id;
    return d;
}


} // namespace

bool FgEnabled() {
    return Enabled() && g_fg_enabled.load(std::memory_order_relaxed);
}

void SetFgEnabled(bool on) {
    g_fg_restart = true;
    g_fg_enabled = on;
}

bool FgPrepare(const Vulkan::Instance& instance, const Frame& frame, FgPrepared& prepared) {
    if (!FgEnabled() || fg.failed) {
        return false;
    }
    const u32 width = frame.output.width, height = frame.output.height;
    if (!fg.context && !CreateFg(instance, width, height)) {
        fg.failed = true;
        return false;
    }
    if (width != fg.width || height != fg.height) {
        fg.reset = true;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - fg.last).count();
    fg.last = now;
    // A hitch: the frame between would show the jump slowed down, not motion. Over two and a half
    // times the usual frame (the average of the others), at least 50 ms: at 25 FPS the frames
    // vary more than 50 ms - 40 ms apart.
    const bool hitch = fg.last_valid && ms > std::max(50.0f, 2.5f * fg.average_ms);
    if (hitch) {
        g_stats.last_hitch_ms = static_cast<u32>(ms);
        ++g_stats.hitches;
    } else if (fg.last_valid) {
        fg.average_ms += (std::clamp(ms, 1.0f, 200.0f) - fg.average_ms) / 16.0f;
    }
    fg.last_valid = true;
    prepared.reset = fg.reset || frame.reset || hitch;
    prepared.frame_ms = fg.frame_ms = ms >= 1.0f && ms <= 200.0f ? ms : fg.average_ms;
    const auto p = FillPrepare(reinterpret_cast<VkCommandBuffer>(uintptr_t{1}), frame, prepared);
    if (const u64 issues = ffxVkPortableValidateFrameGenerationPrepareInfo(&fg.create, &p)) {
        Report("prepare", issues);
        fg.reset = true;
        return false;
    }
    fg.reset = false;
    fg.prepared = true;
    fg.prepared_reset = prepared.reset;
    fg.frame_id = frame.frame_id;
    return true;
}

void FgRecordPrepare(vk::CommandBuffer cmdbuf, const Frame& frame, const FgPrepared& prepared) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const std::array before{
        Barrier(frame.output.image, Stage::eComputeShader | Stage::eTransfer,
                Access::eShaderWrite, Stage::eTransfer, Access::eTransferRead),
        Barrier(fg.hudless.image, Stage::eComputeShader, Access::eShaderRead, Stage::eTransfer,
                Access::eTransferWrite, vk::ImageLayout::eUndefined),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                               .pImageMemoryBarriers = before.data()});
    Copy(cmdbuf, frame.output.image, vk::ImageLayout::eGeneral, fg.hudless.image,
         vk::ImageLayout::eGeneral, fg.width, fg.height);
    // The next render's FSR writes the output again: after the copy has read it.
    const std::array after{
        Barrier(frame.output.image, Stage::eTransfer, Access::eTransferRead, Stage::eAllCommands,
                Access::eNone),
        Barrier(fg.hudless.image, Stage::eTransfer, Access::eTransferWrite, Stage::eComputeShader,
                Access::eShaderRead),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                               .pImageMemoryBarriers = after.data()});
    const auto p = FillPrepare(static_cast<VkCommandBuffer>(cmdbuf), frame, prepared);
    const auto source = Convert(AsImage(fg.hudless), FFX_VK_PORTABLE_RESOURCE_STATE_GENERIC_READ);
    if (const auto r = ffxVkPortableFrameGenerationContextPrepare(fg.context, &p, &source);
        r != FFX_VK_PORTABLE_OK) {
        static int reported = 0;
        if (reported++ < 8) {
            std::fprintf(stderr, "FG: prepare failed (%d)\n", int(r));
        }
    }
}

bool FgBeginDispatch(const Flip& flip, bool& show) {
    show = false;
    if (g_fg_restart.exchange(false)) {
        fg.prepared = false;
        fg.last_valid = false;
    }
    if (!fg.prepared) {
        // A flip without a resolve (menus, loading): the next frame starts over.
        fg.reset = true;
        ++g_stats.fg_skipped;
        return false;
    }
    fg.prepared = false;
    if (flip.width != fg.width || flip.height != fg.height) {
        fg.reset = true;
        ++g_stats.fg_skipped;
        return false;
    }
    const auto d = FillDispatch(reinterpret_cast<VkCommandBuffer>(uintptr_t{1}));
    if (const u64 issues = ffxVkPortableValidateFrameGenerationDispatchInfo(&fg.create, &d)) {
        Report("dispatch", issues);
        fg.reset = true;
        ++g_stats.fg_skipped;
        return false;
    }
    // A reset dispatch has no previous frame to start from: it only builds the history.
    show = fg.dispatches++ > 0 && !fg.prepared_reset;
    if (show) {
        ++fg.shown;
    } else {
        ++g_stats.fg_skipped;
    }
    if (fg.dispatches < 3 || (fg.dispatches & 1023) == 0) {
        std::fprintf(stderr, "FG: dispatch %llu shown %llu dt %.2f ms\n",
                     (unsigned long long)fg.dispatches, (unsigned long long)fg.shown,
                     fg.frame_ms);
    }
    return true;
}

void FgRecordDispatch(vk::CommandBuffer cmdbuf, const Flip& flip) {
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const std::array before{
        Barrier(fg.current.image, Stage::eComputeShader, Access::eShaderRead, Stage::eTransfer,
                Access::eTransferWrite, vk::ImageLayout::eUndefined),
        Barrier(fg.output.image, Stage::eFragmentShader, Access::eShaderRead,
                Stage::eComputeShader, Access::eShaderWrite, vk::ImageLayout::eUndefined),
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 2,
                                               .pImageMemoryBarriers = before.data()});
    Copy(cmdbuf, flip.image, vk::ImageLayout::eTransferSrcOptimal, fg.current.image,
         vk::ImageLayout::eGeneral, fg.width, fg.height);
    const auto copied = Barrier(fg.current.image, Stage::eTransfer, Access::eTransferWrite,
                                Stage::eComputeShader, Access::eShaderRead);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &copied});
    const auto d = FillDispatch(static_cast<VkCommandBuffer>(cmdbuf));
    if (const auto r = ffxVkPortableFrameGenerationContextRecordDispatch(fg.context, &d);
        r != FFX_VK_PORTABLE_OK) {
        static int reported = 0;
        if (reported++ < 8) {
            std::fprintf(stderr, "FG: dispatch failed (%d)\n", int(r));
        }
    }
    const auto done = Barrier(fg.output.image, Stage::eComputeShader, Access::eShaderWrite,
                              Stage::eFragmentShader, Access::eShaderRead,
                              vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal);
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &done});
}

vk::ImageView FgView() {
    return fg.view;
}

void SetGeneratedFrame(Vulkan::Frame* frame) {
    g_generated = frame;
}

Vulkan::Frame* TakeGeneratedFrame() {
    return std::exchange(g_generated, nullptr);
}

} // namespace iss::fsr
