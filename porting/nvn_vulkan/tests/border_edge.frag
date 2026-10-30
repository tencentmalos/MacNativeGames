// SPDX-License-Identifier: MIT
#version 450
layout(set=0,binding=4) uniform sampler2D source_texture;
layout(location=0) out vec4 color;
void main() {
    float u = gl_FragCoord.x < 16.0 ? -1.0 : gl_FragCoord.x < 32.0 ? 0.0 : gl_FragCoord.x < 48.0 ? 0.5 : 1.0;
    color = texture(source_texture, vec2(u, 0.5));
}
