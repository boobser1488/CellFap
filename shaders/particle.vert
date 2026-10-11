#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 iPos;
layout(location = 1) in float iRadius;
layout(location = 2) in vec4 iColor;
layout(location = 3) in uint iKind;  // see ParticleInstance::kind
layout(location = 4) in float iAngle;

layout(location = 0) out vec2 vLocal;   // in the particle's own (rotated) frame
layout(location = 1) flat out vec4 vColor;
layout(location = 2) flat out uint vKind;
layout(location = 3) flat out float vRadiusPx;

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    float px = pc.params.y;
    uint kind = iKind & 255u;
    float r = max(iRadius, px * (kind == 0u ? 0.7 : 1.0));  // food crumbs may be smaller than other markers
    float ext = (kind == 3u || kind == 8u) ? 1.0 : 1.3;
    if (kind == 2u) ext = 1.0 + 3.0 * px / r;  // room for the constant-width ring line
    vec2 c = kCorners[gl_VertexIndex] * ext;
    float ca = cos(iAngle), sa = sin(iAngle);
    vLocal = c;
    vColor = iColor;
    // Food smaller than a pixel is drawn one pixel wide but fainter by its true area: far out, thousands of crumbs
    // read as a light dusting, not as fat dots covering the dish.
    if (kind == 0u) vColor.a *= clamp(iRadius * iRadius / (px * px), 0.15, 1.0);
    vKind = iKind;
    vRadiusPx = r / px;
    vec2 w = vec2(c.x * ca - c.y * sa, c.x * sa + c.y * ca);
    gl_Position = vec4(worldToClip(iPos + w * r), 0.0, 1.0);
}
