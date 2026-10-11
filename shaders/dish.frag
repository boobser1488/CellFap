#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vWorld;
layout(location = 0) out vec4 outColor;

// Clean vector look: only smooth gradients and analytically antialiased lines (no noise textures).
// Theme 1 reproduces the original game's microscope: pale lavender field, thin dark dish outline.
void main() {
    float R = pc.params.z;
    float px = pc.params.y;
    float light = pc.params.w;
    float classic = pc.extra.w;
    float r = length(vWorld);

    // Lit side of the dish (same falloff as World::lightAt).
    vec2 lightDir = vec2(cos(pc.extra.x), sin(pc.extra.x));
    float s = dot(vWorld / R, lightDir);
    float edge = 1.0 - 2.5 * pc.extra.z;
    float lit = light * smoothstep(edge - 0.25, edge + 0.25, s);

    vec3 bgDark = vec3(0.045, 0.052, 0.066);
    vec3 fluidDark = mix(vec3(0.060, 0.080, 0.098), vec3(0.110, 0.155, 0.168), lit);
    vec3 bgClassic = vec3(0.735, 0.735, 0.865);
    vec3 fluidClassic = mix(vec3(0.780, 0.780, 0.925), vec3(0.880, 0.880, 1.000), lit);
    vec3 bg = mix(bgDark, bgClassic, classic);
    vec3 fluid = mix(fluidDark, fluidClassic, classic);
    fluid *= 1.0 - mix(0.16, 0.05, classic) * pow(r / R, 4.0);  // soft lens falloff toward the rim

    // Microscope reticle: 1px lines every 20 µm plus 100 µm major lines; fade out when too dense.
    vec3 gridCol = mix(vec3(0.030, 0.045, 0.050), vec3(-0.06, -0.06, -0.04), classic);
    for (int k = 0; k < 2; ++k) {
        float stepW = k == 0 ? 20.0 : 100.0;
        vec2 g = abs(fract(vWorld / stepW + 0.5) - 0.5) * stepW;
        float line = 1.0 - smoothstep(0.0, px * 1.1, min(g.x, g.y));
        float fade = clamp(stepW / (px * 14.0) - 0.4, 0.0, 1.0);
        fluid += gridCol * line * fade * pc.extra.y * (k == 0 ? 0.8 : 1.4);
    }

    // One-pixel ramps written relative to the pixel size: smoothstep(R - px/2, R + px/2, r) has equal edges in
    // 32-bit floats once R is huge (the editor uses a 1e7 µm dish) and mobile GPUs return NaN for it (black screen).
    float inside = clamp(0.5 - (r - R) / px, 0.0, 1.0);
    vec3 col = mix(bg, fluid, inside);

    // Dish rim: a solid glass wall of constant thickness with one-pixel antialiased edges, sharp at any zoom.
    float halfW = max(px * 0.9, R * 0.0016);
    float rim = clamp(0.5 - (abs(r - R) - halfW) / px, 0.0, 1.0);
    vec3 darkRim = mix(col, vec3(0.40, 0.55, 0.60), rim * 0.85);
    vec3 classicRim = mix(col, vec3(0.12, 0.12, 0.18), rim * 0.9);
    col = mix(darkRim, classicRim, classic);

    outColor = vec4(col * nightTint(), 1.0);  // night dims the whole view, inside and outside the dish
}
