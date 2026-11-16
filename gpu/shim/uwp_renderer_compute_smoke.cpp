// Isolated M5 Vulkan compute dispatch preflight; this does NOT render the guest.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <array>
#include <cstdio>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/host_shaders/m5_empty_compute_comp.h"
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int Run() {
    Vulkan::Instance renderer{-1, false};
    const vk::Device device = renderer.GetDevice();
    const auto [shader_result, shader] = device.createShaderModuleUnique(
        {.codeSize=sizeof(M5_EMPTY_COMPUTE_COMP),.pCode=M5_EMPTY_COMPUTE_COMP});
    Check(shader_result==vk::Result::eSuccess && bool(shader), "empty shader module");
    const auto [layout_result, layout] = device.createPipelineLayoutUnique({});
    Check(layout_result==vk::Result::eSuccess && bool(layout), "empty compute layout");
    const vk::ComputePipelineCreateInfo ci{
        .stage={.stage=vk::ShaderStageFlagBits::eCompute,.module=*shader,.pName="main"},
        .layout=*layout,
    };
    const auto [pipeline_result,pipeline] = device.createComputePipelineUnique({},ci);
    Check(pipeline_result==vk::Result::eSuccess && bool(pipeline),"empty compute pipeline");
    const auto [pool_result,pool] = device.createCommandPoolUnique(
        {.queueFamilyIndex=renderer.GetGraphicsQueueFamilyIndex()});
    Check(pool_result==vk::Result::eSuccess && bool(pool),"empty compute command pool");
    const auto [commands_result,commands] = device.allocateCommandBuffersUnique(
        {.commandPool=*pool,.level=vk::CommandBufferLevel::ePrimary,.commandBufferCount=1});
    Check(commands_result==vk::Result::eSuccess && commands.size()==1,"empty compute command buffer");
    const vk::CommandBuffer cmd=*commands.front();
    Check(cmd.begin({.flags=vk::CommandBufferUsageFlagBits::eOneTimeSubmit})==
          vk::Result::eSuccess,"empty compute begin");
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*pipeline);
    std::puts("M5 empty compute dispatch: command begun, pipeline bound, no descriptors");
    std::fflush(stdout);
    cmd.dispatch(1,1,1);
    std::puts("M5 empty compute dispatch: vkCmdDispatch returned");
    std::fflush(stdout);
    Check(cmd.end()==vk::Result::eSuccess,"empty compute end");
    const auto [fence_result,fence] = device.createFenceUnique({});
    Check(fence_result==vk::Result::eSuccess && bool(fence),"empty compute fence");
    const vk::SubmitInfo submit{.commandBufferCount=1,.pCommandBuffers=&cmd};
    Check(renderer.GetGraphicsQueue().submit(std::array{submit},*fence)==vk::Result::eSuccess,
          "empty compute submit");
    Check(device.waitForFences(std::array{*fence},VK_TRUE,5000000000ull)==vk::Result::eSuccess,
          "empty compute fence timeout/failure");
    std::puts("M5 empty compute dispatch PASS: device queue and fence; no descriptors or guest frame");
    return 0;
}
}
extern "C" int bbgpu_uwp_renderer_compute_smoke() {
    try { return Run(); }
    catch(const std::exception& error) {
        std::printf("M5 empty compute dispatch FAILED: %s\n",error.what());
        return 1;
    }
}
#endif
