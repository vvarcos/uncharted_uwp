// UWP has no FSR/DLSS/XeSS/frame generation. Keep the bbgpu ABI link-complete while native 720p
// is used; these methods deliberately remain inert.
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_frame_generation.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"

namespace Vulkan {
struct Fsr4Upscaler::Impl {};
Fsr4Upscaler::~Fsr4Upscaler() = default;
TemporalUpscaler::TemporalUpscaler(const Instance& i, Scheduler& s, VideoCore::TextureCache& t,
                                   Runtime& r, CameraMotion& c, SceneTargets& st)
    : instance(i), scheduler(s), texture_cache(t), runtime(r), camera_motion(c), scene_targets(st) {}
TemporalUpscaler::~TemporalUpscaler() = default;
void TemporalUpscaler::OnSceneColor(VideoCore::ImageId) {}
void TemporalUpscaler::OnBlendedSceneDraw() {}
void TemporalUpscaler::OnSceneComposite() {}
void TemporalUpscaler::OnDispatch(u64) {}
void TemporalUpscaler::OnDrawRecorded(u64, VideoCore::ImageId) {}
bool TemporalUpscaler::OnFrameStart() { return false; }
bool TemporalUpscaler::RasterScaling() const { return false; }
float TemporalUpscaler::SceneMipBias() const { return 0.0f; }
void TemporalUpscaler::OnDraw(u64, VideoCore::ImageId, VideoCore::ImageId, bool) {}
void TemporalUpscaler::OnColorTarget(VideoCore::ImageId) {}
bool TemporalUpscaler::RedirectColor(VideoCore::ImageId, const VideoCore::ImageViewInfo&, Target&) { return false; }
bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId, Target&) { return false; }
bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId, const VideoCore::ImageViewInfo&, vk::ImageView&, vk::ImageLayout&) { return false; }
bool TemporalUpscaler::DisplayOverride(VAddr, Display&) { return false; }

FrameGenerator::FrameGenerator(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
FrameGenerator::~FrameGenerator() = default;
bool FrameGenerator::Wanted() const { return false; }
vk::ImageView FrameGenerator::BeginCapture(vk::CommandBuffer, vk::ImageView, u32, u32) { return {}; }
void FrameGenerator::EndCapture(vk::CommandBuffer, std::array<float,2>, bool) {}
void FrameGenerator::CaptureHudless(vk::CommandBuffer, vk::Image, vk::Format, u32, u32) {}
vk::ImageView FrameGenerator::Hudless(vk::Format, vk::Extent2D& size) { size={}; return {}; }
u32 FrameGenerator::Generate(vk::CommandBuffer, vk::Image, vk::ImageView, vk::ImageUsageFlags,
                             vk::Image, vk::ImageView, u32, u32, std::span<const Output>) { return 0; }
void FrameGenerator::DumpIfRequested(vk::CommandBuffer, vk::Image, u32, u32, const char*) {}
} // namespace Vulkan
