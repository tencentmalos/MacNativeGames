// SPDX-License-Identifier: MIT
#version 450
layout(set=0,binding=4) uniform ArrayColor { vec4 value; } colors[2];
layout(set=0,binding=16) uniform OtherColor { vec4 value; } other;
layout(location=0) out vec4 color;
void main() { color = vec4(colors[0].value.r, other.value.g, colors[1].value.b, 1.0); }
