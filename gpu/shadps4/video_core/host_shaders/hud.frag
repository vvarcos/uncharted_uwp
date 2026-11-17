// SPDX-License-Identifier: GPL-2.0-or-later
// UWP HUD: R8 glyph atlas coverage times the per-vertex color.

#version 450

layout(binding = 0) uniform sampler2D u_atlas;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 o_color;

void main() {
    const float coverage = texture(u_atlas, v_uv).r;
    o_color = vec4(v_color.rgb, v_color.a * coverage);
}
