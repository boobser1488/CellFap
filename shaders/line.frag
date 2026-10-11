#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 vUV;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in vec3 vLenW;  // half length (in widths), width in px, pinch
layout(location = 0) out vec4 outColor;

// Capsule whose radius narrows toward the middle ("pinch"): adhesin slime stretched between membranes.
void main() {
    float L = vLenW.x;
    if (vLenW.z < -1.5) {  // adhesin fibre across a seam: a thin strand that tapers and fades toward both ends
        float u = L > 1e-3 ? clamp(vUV.x / L, -1.0, 1.0) : 0.0;
        float radius = 1.0 - 0.7 * u * u;
        float df = abs(vUV.y) - radius;
        float aaf = 1.5 / max(vLenW.y, 1.0);
        float af = (1.0 - smoothstep(-aaf, aaf, df)) * vColor.a * (1.0 - u * u) * step(abs(vUV.x), L);
        outColor = vec4(vColor.rgb * nightTint() * af, af);
        return;
    }
    if (vLenW.z < 0.0) {  // flat vector stroke (map walls): crisp capsule, one colour, no sheen
        float xs = max(abs(vUV.x) - L, 0.0);
        float ds = length(vec2(xs, vUV.y)) - 1.0;
        float aas = 1.0 / max(vLenW.y, 1.0);
        float as = (1.0 - smoothstep(-aas, aas, ds)) * vColor.a;
        outColor = vec4(vColor.rgb * nightTint() * as, as);
        return;
    }
    float u = L > 1e-3 ? clamp(vUV.x / L, -1.0, 1.0) : 0.0;
    float radius = 1.0 - vLenW.z * (1.0 - u * u);
    float x = max(abs(vUV.x) - L, 0.0);
    float dist = length(vec2(x, vUV.y)) - radius;
    float aa = 1.5 / max(vLenW.y, 1.0);
    float a = (1.0 - smoothstep(-aa, aa, dist)) * vColor.a;
    // Slight sheen along the strand's axis so the slime reads as a glossy bridge.
    float sheen = 1.0 + 0.25 * (1.0 - smoothstep(0.0, 0.6, abs(vUV.y) / max(radius, 0.05)));
    outColor = vec4(min(vColor.rgb * sheen, vec3(1.0)) * nightTint() * a, a);
}
