// SPDX-License-Identifier: MIT
#version 450
layout(set=0,binding=4) uniform sampler2DArray source_texture;
layout(location=0) out vec4 color;
// A 2D backing must expose one array layer, so the layer index clamps to zero.
void main() { color = texture(source_texture, vec3(0.5, 0.5, 7.0)); }
