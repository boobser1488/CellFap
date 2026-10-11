#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 iA;
layout(location = 1) in vec2 iB;
layout(location = 2) in float iWidth;
layout(location = 3) in vec4 iColor;
layout(location = 4) in float iPinch;

layout(location = 0) out vec2 vUV;  // x along (in widths), y across (-1..1)
layout(location = 1) flat out vec4 vColor;
layout(location = 2) flat out vec3 vLenW;  // half length in widths, width in px, pinch

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    float px = pc.params.y;
    float w = max(iWidth, px * 1.2);  // never thinner than ~1px
    vec2 d = iB - iA;
    float len = max(length(d), 1e-5);
    vec2 dir = d / len;
    vec2 nrm = vec2(-dir.y, dir.x);
    vec2 c = kCorners[gl_VertexIndex];
    float halfLen = len * 0.5 + w;
    vec2 world = (iA + iB) * 0.5 + dir * c.x * halfLen + nrm * c.y * w;
    vUV = vec2(c.x * halfLen / w, c.y);
    vLenW = vec3(len * 0.5 / w, w / px, clamp(iPinch, -2.0, 0.9));  // pinch -1: flat vector stroke, -2: fibre
    vColor = iColor;
    gl_Position = vec4(worldToClip(world), 0.0, 1.0);
}
