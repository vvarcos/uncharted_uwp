// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#ifdef UNCHARTED_UWP
// Raw marker channel: the shadPS4 log sink is not connected in the AppContainer, and the assert
// message alone would arrive as a bare file:line.
extern "C" void bb_log_raw(const char* text);
#endif

namespace Vulkan {

/* bbport: the console ICD answers an unsupported graphics pipeline with VK_ERROR_OUT_OF_HOST_MEMORY
 * (measured), so the error code says nothing about which part it refused. BB_FORCE_STATIC_PSO=1 forces
 * every optional device feature to "absent", which selects the conservative path this code already
 * has (baked vertex input, baked depth/stencil, static color write mask, no provoking vertex, no
 * depth clip control, no subgroup size, no dynamic states beyond viewport/scissor). One switch
 * separates "a feature the ICD cannot honour" from everything else. */
namespace {
bool ForceStaticPso() {
    static const bool forced = [] {
        const char* env = std::getenv("BB_FORCE_STATIC_PSO");
        return env != nullptr && env[0] == '1';
    }();
    return forced;
}

bool PsoDigest() {
    static const bool digest = [] {
        const char* env = std::getenv("BB_LOG_PSO_DIGEST");
        return env == nullptr || env[0] != '0'; // on by default: one line per new pipeline
    }();
    return digest;
}

/* bbport: RECT_LIST/QUAD_LIST are emulated with a synthesised tessellation control + evaluation pair,
 * which makes the pipeline 4 stages (vs+tcs+tes+fs). If the ICD refuses to create a tessellation
 * pipeline, this switch drops the aux stages and asks for a plain triangle list instead: the geometry
 * is then wrong, but it says whether the refusal is about tessellation or about something else. */
bool NoAuxTess() {
    static const bool off = [] {
        const char* env = std::getenv("BB_NO_AUX_TESS");
        return env != nullptr && env[0] == '1';
    }();
    return off;
}

struct FeatureGate {
    const Instance& device;
    const bool forced{ForceStaticPso()};

    bool IsVertexInputDynamicState() const {
        return !forced && device.IsVertexInputDynamicState();
    }
    bool IsDepthClipEnableSupported() const { return !forced && device.IsDepthClipEnableSupported(); }
    bool IsProvokingVertexSupported() const { return !forced && device.IsProvokingVertexSupported(); }
    bool IsDepthClipControlSupported() const {
        return !forced && device.IsDepthClipControlSupported();
    }
    bool IsDepthBoundsSupported() const { return !forced && device.IsDepthBoundsSupported(); }
    bool IsDynamicColorWriteMaskSupported() const {
        return !forced && device.IsDynamicColorWriteMaskSupported();
    }
    bool IsSubgroupSize64Supported() const { return !forced && device.IsSubgroupSize64Supported(); }
    bool IsMixedDepthSamplesSupported() const {
        return !forced && device.IsMixedDepthSamplesSupported();
    }
    bool IsLogicOpSupported() const { return !forced && device.IsLogicOpSupported(); }
    bool IsExtendedDynamicState3Supported() const {
        return !forced && device.IsExtendedDynamicState3Supported();
    }
    bool IsFormatSupported(vk::Format format, vk::FormatFeatureFlagBits2 feature) const {
        return !forced && device.IsFormatSupported(format, feature);
    }
};
} // namespace


using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    const vk::Device device = instance.GetDevice();
    const FeatureGate features{instance};
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
#ifdef UNCHARTED_UWP
    {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "M5 pipe: call=layout result=%d (%s) stages=%u dyn=%u", int(layout_result),
                      vk::to_string(layout_result).c_str(), unsigned(0), unsigned(0));
        bb_log_raw(line);
    }
#endif
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!features.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const bool no_aux_tess = NoAuxTess();
    const bool is_rect_list = !no_aux_tess && key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = !no_aux_tess && key.prim_type == AmdGpu::PrimitiveType::QuadList;
    // With the emulation off the topology must be drawable without tessellation stages.
    const auto topology = no_aux_tess ? vk::PrimitiveTopology::eTriangleList
                                      : LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || features.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!features.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!features.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = features.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (features.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (features.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (features.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = 64,
    };
    for (auto& stage : shader_stages) {
        stage.pNext = features.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr;
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!features.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = features.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        // Vulkan ignores blend factors for min/max, but a factor that zeroes one operand
        // makes the operation collapse to a plain selection: min(s, 0) is 0 and max(s, 0)
        // is s for normalized alpha. Rewrite those to the equivalent add so the result is
        // exact instead of leaving the other operand to survive.
        auto eff_src_alpha = src_alpha;
        auto eff_dst_alpha = dst_alpha;
        auto eff_alpha_blend = alpha_blend;
        if (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) {
            const bool takes_max = alpha_blend == vk::BlendOp::eMax;
            if (src_alpha == vk::BlendFactor::eOne && dst_alpha == vk::BlendFactor::eZero) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
                eff_dst_alpha = vk::BlendFactor::eZero;
            } else if (src_alpha == vk::BlendFactor::eZero && dst_alpha == vk::BlendFactor::eOne) {
                eff_alpha_blend = vk::BlendOp::eAdd;
                eff_src_alpha = vk::BlendFactor::eZero;
                eff_dst_alpha = takes_max ? vk::BlendFactor::eOne : vk::BlendFactor::eZero;
            }
        }

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne) &&
            !key.color_buffers[i].blend_self_scale;
        const auto alpha_scaled_min_max =
            (eff_alpha_blend == vk::BlendOp::eMin || eff_alpha_blend == vk::BlendOp::eMax) &&
            (eff_src_alpha != vk::BlendFactor::eOne || eff_dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = eff_src_alpha,
            .dstAlphaBlendFactor = eff_dst_alpha,
            .alphaBlendOp = eff_alpha_blend,
            .colorWriteMask =
                features.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // The shader squares its color output for this attachment (see PsColorBuffer), so the
        // factors must not scale the operands again.
        if (key.color_buffers[i].blend_self_scale) {
            LOG_WARNING(
                Render_Vulkan,
                "Emulating scaled min/max blend with squared shader output on attachment {}", i);
            attachments[i].srcColorBlendFactor = vk::BlendFactor::eOne;
            attachments[i].dstColorBlendFactor = vk::BlendFactor::eOne;
        }

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            features.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = &pipeline_rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !features.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !features.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

#ifdef UNCHARTED_UWP
    /* bbport: what the driver is actually being asked for. The failure code is generic, so this line
     * is the evidence: render target formats (a colour format that is not RT-capable on D3D12 FL 11_0
     * fails here with the same code), sample counts, depth format and which optional features were
     * requested. One line per new pipeline; BB_LOG_PSO_DIGEST=0 turns it off. */
    if (PsoDigest()) {
        char line[704];
        char colors[192]{};
        size_t used = 0;
        /* bbport: which pipeline topologies the device announces at all. The console refused the
         * 4-stage (tessellation) pipeline; whether a 3-stage vs+gs+fs pipeline is even a candidate
         * depends on these two flags, so they belong in the same line as the request. */
        const auto device_features = instance.GetPhysicalDevice().getFeatures();
        for (s32 i = 0; i < key.num_color_attachments && used + 32 < sizeof(colors); ++i) {
            const auto name = vk::to_string(color_formats[i]);
            used += std::snprintf(colors + used, sizeof(colors) - used, "%s%s",
                                  i ? "," : "", name.c_str());
        }
        std::snprintf(line, sizeof(line),
                      "M5 pso: %s colors=%d[%s] samples=%u depth=%s dsamples=%u stages=%u "
                      "dynstates=%u static=%d gs=%d tess=%d dyn{vid=%d clip=%d provoking=%d "
                      "clipctl=%d bounds=%d cwm=%d sg64=%d mixds=%d logicop=%d eds3=%d}",
                      debug_str.c_str(), int(key.num_color_attachments), colors,
                      unsigned(color_samples[0]), vk::to_string(depth_format).c_str(),
                      unsigned(color_samples[0]), unsigned(shader_stages.size()),
                      unsigned(dynamic_states.size()), int(ForceStaticPso()),
                      int(device_features.geometryShader), int(device_features.tessellationShader), int(features.IsVertexInputDynamicState()),
                      int(features.IsDepthClipEnableSupported()),
                      int(features.IsProvokingVertexSupported()),
                      int(features.IsDepthClipControlSupported()),
                      int(features.IsDepthBoundsSupported()),
                      int(features.IsDynamicColorWriteMaskSupported()),
                      int(features.IsSubgroupSize64Supported()),
                      int(features.IsMixedDepthSamplesSupported()),
                      int(features.IsLogicOpSupported()),
                      int(features.IsExtendedDynamicState3Supported()));
        bb_log_raw(line);
    }
#endif
    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    if (pipeline_result != vk::Result::eSuccess) {
#ifdef UNCHARTED_UWP
        {
            char line[384];
            std::snprintf(line, sizeof(line), "M5 pipeline: graphics create failed result=%s (%d) %s",
                          vk::to_string(pipeline_result).c_str(), int(pipeline_result),
                          debug_str.c_str());
            bb_log_raw(line);
        }
#endif
        // bbport (Mevlock, drakolordx7): some drivers refuse a pipeline only because of the
        // pipeline cache (ErrorInitializationFailed on a Radeon RX 9070 XT at start): try once
        // more without it.
        auto [retry_result, retry_pipe] = device.createGraphicsPipelineUnique(nullptr, pipeline_info);
        if (retry_result == vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan,
                        "Graphics pipeline {} was refused with the pipeline cache ({}); created without it",
                        debug_str, vk::to_string(pipeline_result));
            pipeline_result = retry_result;
            pipe = std::move(retry_pipe);
        }
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    /* bbport: a stage the device does not implement must not appear in a descriptor set layout. The
     * console does not announce tessellationShader, and a layout whose bindings carry
     * TESSELLATION_CONTROL/EVALUATION is refused by vkCreatePipelineLayout with the same generic
     * VK_ERROR_OUT_OF_HOST_MEMORY, which then masks the real (pipeline) failure behind an assert
     * here. Geometry is announced on this device and stays. Logged because the mask is the input
     * that names the failure. */
    static const vk::ShaderStageFlags device_stages = [this] {
        vk::ShaderStageFlags flags = vk::ShaderStageFlagBits::eVertex |
                                     vk::ShaderStageFlagBits::eFragment;
        const auto features = instance.GetPhysicalDevice().getFeatures();
        if (features.tessellationShader) {
            flags |= vk::ShaderStageFlagBits::eTessellationControl |
                     vk::ShaderStageFlagBits::eTessellationEvaluation;
        }
        if (features.geometryShader) {
            flags |= vk::ShaderStageFlagBits::eGeometry;
        }
        return flags;
    }();
    vk::ShaderStageFlags requested_stages{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto raw_stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        requested_stages |= raw_stage_bit;
        const auto stage_bit = raw_stage_bit & device_stages;
        for (const auto& buffer : stage->buffers) {
            (void)buffer; // the V# is not needed for the layout (and reading it may touch memory)
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
#ifdef UNCHARTED_UWP
    {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "M5 pipe: call=dslayout result=%d bindings=%u stageflags=0x%x "
                      "supported=0x%x push=%d",
                      int(layout_result), unsigned(bindings.size()),
                      unsigned(requested_stages), unsigned(device_stages),
                      int(uses_push_descriptors));
        bb_log_raw(line);
    }
#endif
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
