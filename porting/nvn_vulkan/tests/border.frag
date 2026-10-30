// SPDX-License-Identifier: MIT
#version 450
layout(set=0,binding=4) uniform sampler2D source_texture;
layout(location=0) out vec4 color;
void main() { color = texture(source_texture, vec2(2.0)); }
