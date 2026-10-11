#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in uint vKind;
layout(location = 3) flat in float vRadiusPx;
layout(location = 0) out vec4 outColor;

float sdHex(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}
float sdSeg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}
float fill(float d, float aa) { return 1.0 - smoothstep(-aa, aa, d); }

void main() {
    uint kind = vKind & 255u;
    float seed = float((vKind >> 8) & 255u) / 255.0;
    float aa = 1.0 / max(vRadiusPx, 0.5);  // one pixel in local units
    float t = pc.params.x;
    vec2 q = vLocal;
    float d = length(q);
    float a;
    vec3 rgb = vColor.rgb;
    if (kind == 1u) {
        float h = sdHex(q, 0.85);
        a = 1.0 - smoothstep(-aa, aa, h);
        float core = 1.0 - smoothstep(-aa, aa, sdHex(q, 0.45));
        rgb = mix(rgb, rgb * 1.5 + 0.15, core);
    } else if (kind == 2u) {
        // Thin ring of constant screen width (~1.5 px) whatever the radius: brush circles, markers.
        a = 1.0 - smoothstep(0.75 * aa, 1.5 * aa, abs(d - 1.0));
    } else if (kind == 3u) {
        a = exp(-d * d * 3.0) * (1.0 - smoothstep(0.9, 1.0, d));
    } else if (kind == 4u) {
        a = fill(d - 1.0, aa * 0.6);
    } else if (kind == 8u) {
        // Dissolved substance: a soft cloud, densest in the middle, with a faint grainy shimmer.
        float g = exp(-d * d * 2.6) * (1.0 - smoothstep(0.85, 1.0, d));
        float shimmer = 0.85 + 0.15 * sin(q.x * 9.0 + t * 1.5 + seed * 6.0) * sin(q.y * 8.0 - t * 1.2);
        a = g * shimmer;
    } else if (kind == 9u) {
        // Electric spark: a bright core and a few jagged branches.
        a = exp(-d * d * 18.0);
        float ang = atan(q.y, q.x);
        float br = pow(abs(sin(ang * 3.0 + seed * 9.0 + floor(t * 30.0))), 24.0) * (1.0 - smoothstep(0.2, 1.0, d));
        a = max(a, br);
    } else {
        a = fill(d - 1.0, aa);
        rgb *= 0.85 + 0.3 * (1.0 - d);
    }
    a *= vColor.a;
    if (kind != 2u && kind != 3u && kind != 9u) rgb *= nightTint();  // markers, glows and sparks keep their light
    outColor = vec4(rgb * a, a);
}
