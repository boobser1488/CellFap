#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 iPos;
layout(location = 1) in float iRadius;
layout(location = 2) in float iAngle;
layout(location = 3) in vec4 iColor;
layout(location = 4) in uint iTypeFlags;  // bits 0..7 type, 8..15 flags, 16..31 seed
layout(location = 5) in float iP0;
layout(location = 6) in float iP1;
layout(location = 7) in uvec4 iShape0;  // spokes 0..15, one byte each
layout(location = 8) in uvec4 iShape1;  // spokes 16..31

layout(location = 0) out vec2 vLocal;
layout(location = 1) flat out vec4 vColor;
layout(location = 2) flat out uint vTypeFlags;
layout(location = 3) flat out vec3 vParams;  // radius, p0, p1
layout(location = 4) out vec2 vWorldDir;     // local offset in world orientation (for fixed lighting)
layout(location = 5) flat out uvec4 vShape0;
layout(location = 6) flat out uvec4 vShape1;
layout(location = 7) out vec2 vWorld;        // world position (clipped at the dish glass)
layout(location = 8) flat out vec2 vCenter;  // centre of the deformed outline (local units): where the nucleus sits

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    uint type = iTypeFlags & 255u;
    uint flags = (iTypeFlags >> 8) & 255u;
    // Deformed outlines bulge up to 1.4 radii.
    float ext = 1.5;
    if (type == 1u) ext = 2.6;        // flagellocyte tail
    else if (type == 17u) ext = 1.7;  // ciliocyte hairs
    else if (type == 11u) ext = 1.65; // senseocyte antennae
    else if (type == 7u) ext = 1.6;   // glueocyte fringe
    if ((flags & 1u) != 0u) ext = max(ext, 1.75);

    vec2 c = kCorners[gl_VertexIndex] * ext;
    float ca = cos(iAngle), sa = sin(iAngle);
    // Never smaller than about a pixel: zoomed far out, cells stay visible as dots instead of vanishing.
    float R = max(iRadius, pc.params.y * 1.2);
    vec2 world = iPos + R * vec2(c.x * ca - c.y * sa, c.x * sa + c.y * ca);
    vLocal = c;
    vWorldDir = vec2(c.x * ca - c.y * sa, c.x * sa + c.y * ca);
    // A cell smaller than the minimum dot is drawn at that size but fainter by its true area: thousands of sub-pixel
    // cells moving by fractions of a pixel no longer twinkle as their dots snap between pixels.
    vColor = vec4(iColor.rgb, clamp(iRadius * iRadius / (R * R), 0.3, 1.0));
    vTypeFlags = iTypeFlags;
    vParams = vec3(R, iP0, iP1);
    vShape0 = iShape0;
    vShape1 = iShape1;
    // Area centroid of the outline r(theta): (2/3) * sum r^3 u / sum r^2. A cell pressed flat on one side bulges on the
    // others, and a nucleus left at the physical centre looked pushed against the flat face.
    vec2 cen = vec2(0.0);
    float area = 0.0;
    for (int i = 0; i < 32; ++i) {
        uvec4 v = i < 16 ? iShape0 : iShape1;
        int j = (i >> 2) & 3;
        uint w = j == 0 ? v.x : (j == 1 ? v.y : (j == 2 ? v.z : v.w));
        float r = 0.4 + float((w >> uint((i & 3) * 8)) & 255u) / 255.0;
        float a = float(i) * (6.2831853 / 32.0);
        cen += r * r * r * vec2(cos(a), sin(a));
        area += r * r;
    }
    vCenter = cen * (2.0 / 3.0) / max(area, 1e-4);
    vWorld = world;
    gl_Position = vec4(worldToClip(world), 0.0, 1.0);
}
