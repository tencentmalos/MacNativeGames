// SPDX-License-Identifier: MIT
#version 450
layout(location=0) in vec4 packed_value;
layout(location=0) out vec4 value;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1,-1), vec2(3,-1), vec2(-1,3));
    gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
    value = packed_value * 0.5 + 0.5;
}
