// SPDX-License-Identifier: GPL-2.0-or-later
// EXPERIMENTAL / opt-in UWP depth copy backend. NOT wired into the runtime.
//
// One fragment program serves both destinations: gl_FragDepth feeds a D32SfloatS8Uint
// target (depth-only view), while o_color.r feeds an R32Sfloat target. The two
// pipelines differ only in dynamic-rendering attachments and depth state.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

// Sampled-image descriptor with no sampler: texelFetch does not consume sampler state.
layout(binding = 0, set = 0) uniform texture2D u_src;

layout(location = 0) out vec4 o_color;

void main() {
    const ivec2 coord = ivec2(gl_FragCoord.xy);
    const float value = texelFetch(u_src, coord, 0).r;
    o_color = vec4(value, 0.0, 0.0, 1.0);

    // Limitation: depth is clamped to [0, 1]; the stencil aspect is never written.
    gl_FragDepth = clamp(value, 0.0, 1.0);
}
