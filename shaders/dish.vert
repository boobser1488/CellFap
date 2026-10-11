#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) out vec2 vWorld;

void main() {
    vec2 ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    vWorld = pc.cam.xy + ndc / pc.cam.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
