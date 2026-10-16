#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in uint vTypeFlags;
layout(location = 3) flat in vec3 vParams;
layout(location = 4) in vec2 vWorldDir;
layout(location = 5) flat in uvec4 vShape0;
layout(location = 6) flat in uvec4 vShape1;
layout(location = 7) in vec2 vWorld;
layout(location = 8) flat in vec2 vCenter;  // centre of the deformed outline (see cell.vert)
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265;
const float TAU = 6.2831853;

// Outline radius of spoke i (0..31), in units of the cell radius. No dynamic indexing of the vector inputs: some
// mobile GPU compilers get that wrong for flat varyings (outlines flickered on Android).
float spoke(int i) {
    i &= 31;
    uvec4 v = i < 16 ? vShape0 : vShape1;
    int j = (i >> 2) & 3;
    uint w = j == 0 ? v.x : (j == 1 ? v.y : (j == 2 ? v.z : v.w));
    return 0.4 + float((w >> uint((i & 3) * 8)) & 255u) / 255.0;
}
// Smooth closed curve through the spokes (Catmull-Rom), so the membrane reads as one vector outline.
float shapeR(float ang) {
    float f = fract(ang / TAU + 1.0) * 32.0;
    int i = int(floor(f));
    float t = f - float(i);
    float p0 = spoke(i - 1), p1 = spoke(i), p2 = spoke(i + 1), p3 = spoke(i + 2);
    float t2 = t * t, t3 = t2 * t;
    return 0.5 * (2.0 * p1 + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

float fillSd(float d, float aa) { return 1.0 - smoothstep(-aa, aa, d); }
float sdCircle(vec2 p, vec2 c, float r) { return length(p - c) - r; }
float sdSeg(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}
float sdEllipse(vec2 p, vec2 c, float ang, vec2 ab) {
    vec2 d = p - c;
    float cs = cos(ang), sn = sin(ang);
    d = vec2(cs * d.x + sn * d.y, -sn * d.x + cs * d.y);
    return (length(d / ab) - 1.0) * min(ab.x, ab.y);
}
float sdHex(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}
vec2 polar(float a, float r) { return vec2(cos(a), sin(a)) * r; }

// Nothing of a cell (a flagellum, a membrane) shows beyond the dish glass; a one-pixel ramp at the rim.
float glassClip() { return clamp(0.5 - (length(vWorld) - pc.params.z) / pc.params.y, 0.0, 1.0); }

void main() {
    uint type = vTypeFlags & 255u;
    uint flags = (vTypeFlags >> 8) & 255u;
    float seed = float(vTypeFlags >> 16) * 0.61803;
    float R = vParams.x;
    float p0 = vParams.y;
    float p1 = vParams.z;
    float t = pc.params.x;
    float aa = max(pc.params.y / R, 0.0005) * 0.75;  // one-pixel edges at any zoom
    vec2 qRaw = vLocal;
    float dRaw = length(qRaw);
    float ang = atan(qRaw.y, qRaw.x);
    float sr = shapeR(ang);  // deformed outline radius in this direction
    // Everything inside the cell is drawn in "normalised" space where the deformed outline is the unit
    // circle: organelles squash and stretch together with the membrane.
    vec2 q = qRaw / sr;
    float d = dRaw / sr;
    aa /= sr;
    vec3 base = vColor.rgb;
    // Detail fades out as the cell shrinks toward a few pixels (fine features blinked black, see the compose step).
    float lod = smoothstep(0.06, 0.28, aa);
    vec4 col = vec4(0.0);

    // Far zoom: plain antialiased (deformed) disc.
    if (aa > 0.3) {
        float a = fillSd(d - 1.0, aa) * 0.95;
        if ((flags & 1u) != 0u) {
            float ring = fillSd(abs(d - 1.25) - 0.08, aa);
            col = over(col, vec3(1.0), ring);
        }
        col = over(col, base, a);
        col.rgb *= mix(nightTint(), vec3(1.0), type == 21u ? clamp(p0, 0.0, 1.0) : 0.0);  // a lit photophore is not dimmed
        outColor = col * (glassClip() * vColor.a);  // vColor.a: coverage of a cell smaller than its drawn dot
        return;
    }

    // ---------------------------------------------------------------- appendages (behind body)
    if (type == 1u) {  // flagellocyte: undulating tail at the back
        float x = -q.x;
        if (x > 0.8 && x < 2.45) {
            float s = clamp((x - 0.85) / 1.5, 0.0, 1.0);
            float phase = p0 * TAU + seed * 7.0;  // p0: the beat phase in cycles (advanced per cell on the CPU)
            float y = 0.24 * s * sin(x * 5.5 - phase);
            float w = mix(0.13, 0.035, s);  // thick at the base, tapering
            float dist = abs(q.y - y) - w;
            float a = fillSd(dist * 0.9, aa) * (1.0 - smoothstep(2.3, 2.42, x));
            col = over(col, base * 0.45, a * (1.0 - lod));
        }
    } else if (type == 17u) {  // ciliocyte: ring of beating hairs
        if (d > 0.9 && d < 1.38) {
            float n = 30.0;
            float k = (ang + PI) / TAU * n;
            float idx = floor(k);
            float bend = 0.35 * sin(p0 * TAU + idx * 0.7 + seed);  // p0: beat phase in cycles
            float off = (fract(k) - 0.5) - bend * (d - 1.0) * 2.5;
            float hair = abs(off) * (TAU * d / n) - 0.022;
            float a = fillSd(hair, aa) * (1.0 - smoothstep(1.26, 1.34, d));
            // Thinner than a pixel the hairs alias into sparkles: blend them into a soft fringe of their average cover.
            a = mix(a, 0.3 * (1.0 - smoothstep(1.26, 1.34, d)), smoothstep(0.012, 0.045, aa));
            col = over(col, base * 0.75, a * 0.9 * (1.0 - lod));
        }
    } else if (type == 11u) {  // senseocyte: two sensory feelers at the front, swaying
        for (int s = -1; s <= 1; s += 2) {
            float sway = 0.12 * sin(t * 2.6 + seed + float(s));
            vec2 a0 = polar(0.35 * float(s), 0.95);
            vec2 a1 = polar(0.55 * float(s) + sway, 1.26);
            float dist = sdSeg(q, a0, a1) - 0.025;
            col = over(col, base * 0.8, fillSd(dist, aa) * (1.0 - lod));
            col = over(col, base * 1.2 + 0.2, fillSd(sdCircle(q, a1, 0.05), aa) * (1.0 - lod));
        }
    } else if (type == 7u) {  // glueocyte: sticky droplet fringe
        float n = 18.0;
        float k = (ang + PI) / TAU * n;
        vec2 c = polar((floor(k) + 0.5) / n * TAU - PI, 1.07);
        float dd = sdCircle(q, c, 0.055 * (0.85 + 0.15 * sin(t * 3.0 + floor(k) * 1.7)));
        col = over(col, mix(base, vec3(0.95, 0.9, 0.55), 0.4), fillSd(dd, aa) * 0.8 * (1.0 - lod));
    }

    // ---------------------------------------------------------------- body (Cell Lab style: flat, crisp)
    // A flat fill, a darker membrane ring and simple per-type glyphs. Every edge is a signed distance with a
    // one-pixel transition, so the cell stays a sharp vector drawing at any zoom.
    float rr = 1.0;
    if (type == 3u) {  // devorocyte teeth, none where it is bonded (p0 = 16-sector bond mask)
        uint bonds = uint(p0 + 0.5);
        float f = fract(ang / TAU + 1.0) * 16.0;
        uint k0 = uint(floor(f)) & 15u;
        float b0 = float((bonds >> k0) & 1u), b1 = float((bonds >> ((k0 + 1u) & 15u)) & 1u);
        float bonded = mix(b0, b1, smoothstep(0.0, 1.0, fract(f)));
        rr = mix(0.93 + 0.12 * pow(abs(cos(ang * 6.0 + seed + t * 0.5)), 6.0), 1.0, bonded);
    }
    float bodyD = d - rr;
    float bodyA = fillSd(bodyD, aa);
    if (bodyA <= 0.0 && col.a <= 0.0 && (flags & 41u) == 0u) discard;  // keep selection/hover rings

    float membW = (type == 5u) ? 0.2 : (type == 22u ? 0.16 : (type == 10u ? 0.13 : 0.085));
    vec3 membCol = base * 0.62;
    if (type == 5u) membCol = mix(base * 0.55, vec3(0.30, 0.27, 0.22), 0.35);  // keratinocyte: thick shell
    vec3 fill = base;
    vec4 org = vec4(0.0);
    vec3 dark = base * 0.62, darker = base * 0.45, light = mix(base, vec3(1.0), 0.45);

    if (type == 0u) {  // phagocyte: two food vacuoles (rings) drifting slowly
        for (int i = 0; i < 2; ++i) {
            vec2 c = polar(seed * 3.0 + float(i) * 2.6 + t * 0.15, 0.48);
            float dd = sdCircle(q, c, 0.13 - 0.03 * float(i));
            org = over(org, light, fillSd(dd, aa));
            org = over(org, dark, fillSd(abs(dd) - 0.025, aa));
        }
    } else if (type == 2u) {  // photocyte: ring of chloroplast discs
        for (int i = 0; i < 7; ++i) {
            float a = float(i) * TAU / 7.0 + seed + t * 0.06;
            float dd = sdEllipse(q, polar(a, 0.6), a + PI * 0.5, vec2(0.15, 0.085));
            org = over(org, mix(vec3(0.16, 0.45, 0.14), base, 0.35), fillSd(dd, aa));
        }
    } else if (type == 3u) {  // devorocyte: dark gullet
        org = over(org, vec3(0.32, 0.05, 0.08), fillSd(sdCircle(q, vec2(0.56, 0.0), 0.17), aa));  // in front of the nucleus
    } else if (type == 4u) {  // lipocyte: one big oil droplet growing with stored fat
        float lr = 0.25 + 0.5 * clamp(p0, 0.0, 1.0);
        float dd = sdCircle(q, vec2(0.12, 0.0), lr);
        org = over(org, vec3(1.0, 0.95, 0.72), fillSd(dd, aa));
        org = over(org, mix(vec3(1.0, 0.95, 0.72), base, 0.6), fillSd(abs(dd) - 0.025, aa));
    } else if (type == 6u) {  // buoyocyte: gas vacuole
        float vr = 0.28 + 0.25 * clamp(1.0 - p0, 0.0, 1.0);
        float dd = sdCircle(q, vec2(0.15, 0.05), vr);
        org = over(org, mix(base, vec3(1.0), 0.6), fillSd(dd, aa));
        org = over(org, dark, fillSd(abs(dd) - 0.03, aa));
    } else if (type == 8u) {  // virocyte: virions
        for (int i = 0; i < 6; ++i) {
            vec2 c = polar(seed + float(i) * 1.047 + t * 0.2, 0.56);
            org = over(org, vec3(0.50, 0.22, 0.66), fillSd(sdHex(q - c, 0.08), aa));
        }
    } else if (type == 9u) {  // nitrocyte: rod bacteroids
        for (int i = 0; i < 4; ++i) {
            float a = seed + float(i) * 1.5708;
            vec2 c = polar(a, 0.55), dir = polar(a + 1.2, 0.13);
            org = over(org, mix(vec3(0.15, 0.45, 0.5), base, 0.3), fillSd(sdSeg(q, c - dir, c + dir) - 0.06, aa));
        }
    } else if (type == 10u) {  // stereocyte: an irregular star of cytoskeleton struts radiating from the central nucleus
        for (int i = 0; i < 6; ++i) {
            float h1 = hash11(seed * 1.3 + float(i) * 7.17), h2 = hash11(seed * 2.1 + float(i) * 3.31);
            float a = (float(i) + 0.35 * (h1 - 0.5)) * TAU / 6.0 + seed;   // uneven spacing
            float len = 0.62 + 0.26 * h2;                                  // uneven length
            vec2 root = polar(a, 0.2);
            vec2 knee = polar(a + 0.28 * (h1 - 0.5), 0.2 + len * 0.55);    // a kink part-way out
            vec2 tip = polar(a + 0.45 * (h2 - 0.5), 0.2 + len * 0.8);
            float dd = min(sdSeg(q, root, knee) - 0.034, sdSeg(q, knee, tip) - 0.02);  // thinner toward the tip
            org = over(org, dark, fillSd(dd, aa));
            org = over(org, dark, fillSd(sdCircle(q, tip, 0.03), aa));     // small anchor at the membrane
        }
        // sensor spot lights with the stimulus
        org = over(org, mix(darker, vec3(1.0, 0.9, 0.6), clamp(p0, 0.0, 1.0)), fillSd(sdCircle(q, vec2(0.66, 0.0), 0.09), aa));
    } else if (type == 11u) {  // senseocyte: eye (iris + pupil widening with the stimulus)
        float s = clamp(p0, 0.0, 1.0);
        org = over(org, vec3(0.92, 0.38, 0.16), fillSd(sdCircle(q, vec2(0.58, 0.0), 0.2), aa));
        org = over(org, vec3(0.08, 0.04, 0.03), fillSd(sdCircle(q, vec2(0.6, 0.0), 0.07 + 0.08 * s), aa));
        org = over(org, vec3(1.0), fillSd(sdCircle(q, vec2(0.64, 0.06), 0.03), aa));
    } else if (type == 12u) {  // myocyte: fibres as arcs from the front pole to the back one, like Cell Lab's muscle
        // Bipolar coordinates with the poles at the front and back of the cell: sigma is constant along each arc
        // through both poles, so equal steps of sigma give spindle-shaped fibres. Contracting draws the poles in.
        float k = (1.0 - 0.35 * clamp(p0, 0.0, 1.0)) / 0.92;
        vec2 m = q * k;
        float sigma = atan(2.0 * m.y, 1.0 - dot(m, m));
        float tau = 0.5 * log((dot(m + vec2(1.0, 0.0), m + vec2(1.0, 0.0)) + 1e-5) / (dot(m - vec2(1.0, 0.0), m - vec2(1.0, 0.0)) + 1e-5));
        const float perRad = 2.67;  // ~8 fibres around the cell
        float v = fract(clamp(sigma * perRad, -4.0, 4.0));
        float wig = abs(sin(2.0 * tau * perRad)) * 0.1;  // fibres swell a little along their length
        float dv = abs(v - 0.5) - (0.2 + wig);
        org = over(org, base * 0.72, fillSd(dv, max(fwidth(v), 1e-4)) * (1.0 - smoothstep(0.86, 0.95, d)));
    } else if (type == 13u) {  // neurocyte: dendrites; impulses run outward, faster when the cell fires
        float act = floor(p0) / 63.0, ph = fract(p0);  // p0 = activity in 1/63 steps + impulse phase (CPU)
        for (int i = 0; i < 6; ++i) {
            float a = float(i) * TAU / 6.0 + seed;
            float dd = sdSeg(q, vec2(0.0), polar(a, 0.98)) - 0.03;  // reaches under the membrane ring
            vec2 b = polar(a, 0.5);
            dd = min(dd, sdSeg(q, b, b + polar(a + 0.7, 0.28)) - 0.02);
            org = over(org, dark, fillSd(dd, aa));
            float u = fract(ph + hash11(seed + float(i) * 3.7));
            float bd = sdCircle(q, polar(a, 0.15 + 0.8 * u), 0.05);
            org = over(org, vec3(1.0, 0.96, 0.7), fillSd(bd, aa) * (0.35 + 0.65 * act));
        }
    } else if (type == 14u) {  // secrocyte: a ring of secretory vesicles
        for (int i = 0; i < 5; ++i) {
            float dd = sdCircle(q, polar(float(i) * TAU / 5.0 + 0.63, 0.56), 0.12);
            org = over(org, light, fillSd(dd, aa));
            org = over(org, dark, fillSd(abs(dd) - 0.025, aa));
        }
    } else if (type == 15u) {  // stemocyte: two concentric rings
        org = over(org, dark, fillSd(abs(d - 0.62) - 0.03, aa));
        org = over(org, dark, fillSd(abs(d - 0.40) - 0.025, aa));
    } else if (type == 16u) {  // gamete: a rhombus with a bright centre
        float dd = (abs(q.x) / 0.62 + abs(q.y) / 0.42 - 1.0) * 0.33;
        org = over(org, dark, fillSd(dd, aa));
        org = over(org, light, fillSd(sdCircle(q, vec2(0.0), 0.13), aa));
    } else if (type == 17u) {  // ciliocyte: basal bodies under the hairs
        for (int i = 0; i < 12; ++i) org = over(org, dark, fillSd(sdCircle(q, polar(float(i) * TAU / 12.0, 0.78), 0.04), aa));
    } else if (type == 18u) {  // electrocyte: a stack of curved electroplaques, a battery from the - pole to the +
        // The plates charge one after another from the back; a full cell flickers, ready to discharge.
        float ch = clamp(p0, 0.0, 1.0);
        vec3 spark = vec3(0.72, 0.93, 1.0);
        float flick = ch > 0.97 ? 0.85 + 0.15 * sin(t * 23.0 + seed * 5.0) : 1.0;
        for (int i = 0; i < 5; ++i) {
            float x0 = -0.5 + float(i) * 0.25;
            float plate = abs(q.x - x0 + 0.35 * q.y * q.y) - 0.05;  // bowed toward the front, like a lens
            plate = max(plate, abs(q.y) - 0.6);
            float lit = smoothstep(float(i) * 0.2, float(i) * 0.2 + 0.2, ch);
            org = over(org, spark, fillSd(plate - 0.05, aa) * 0.35 * lit * flick);  // halo
            org = over(org, mix(darker, spark, lit * flick), fillSd(plate, aa));
            // the plate's innervated (front) face: a thin pale edge
            float face = max(abs(q.x - x0 - 0.065 + 0.35 * q.y * q.y) - 0.014, abs(q.y) - 0.58);
            org = over(org, mix(light, vec3(1.0), lit), fillSd(face, aa) * 0.8);
        }
        // Poles: + at the front, - at the back.
        vec3 pole = mix(darker, spark, ch);
        float plus = min(sdSeg(q, vec2(0.62, 0.0), vec2(0.82, 0.0)), sdSeg(q, vec2(0.72, -0.1), vec2(0.72, 0.1))) - 0.03;
        org = over(org, pole, fillSd(plus, aa));
        org = over(org, darker, fillSd(sdSeg(q, vec2(-0.82, 0.0), vec2(-0.66, 0.0)) - 0.03, aa));
    } else if (type == 19u) {  // cnidocyte: an oval capsule holding a harpoon (triangle) that fills as it reloads
        float ch = clamp(p0, 0.0, 1.0);
        float cap = sdEllipse(q, vec2(0.12, 0.0), 0.0, vec2(0.5, 0.3));
        org = over(org, mix(base, vec3(1.0), 0.35), fillSd(cap, aa));
        org = over(org, darker, fillSd(abs(cap) - 0.03, aa));
        float tri = max(abs(q.y) - 0.26 * (0.48 - q.x), max(-0.18 - q.x, q.x - 0.48));
        org = over(org, darker, fillSd(tri, aa) * (0.25 + 0.75 * ch));
        org = over(org, darker, fillSd(sdSeg(q, vec2(0.62, 0.0), vec2(0.98, 0.0)) - 0.022, aa));  // trigger bristle
    } else if (type == 20u) {  // chromatocyte: six pigment arms around a hexagon; they reach out as it darkens
        float s = clamp(p0, 0.0, 1.0);
        vec3 pig = base * 0.45;
        float reach = 0.3 + 0.52 * s;
        for (int i = 0; i < 6; ++i) {
            vec2 tip = polar(float(i) * TAU / 6.0 + PI / 6.0, reach);
            org = over(org, pig, fillSd(sdSeg(q, vec2(0.0), tip) - 0.05, aa));
            org = over(org, pig, fillSd(sdCircle(q, tip, 0.08), aa));
        }
        org = over(org, pig, fillSd(sdHex(q, 0.22), aa));
    } else if (type == 21u) {  // luminocyte: a photophore (reflector cup + lens) that lights up with the glow
        float g = clamp(p0, 0.0, 1.0);
        vec2 lc = vec2(0.18, 0.0);
        org = over(org, darker, fillSd(abs(sdCircle(q, lc, 0.46)) - 0.035, aa));  // reflector cup
        vec3 lamp = mix(base * 0.8, vec3(1.0, 1.0, 0.92), 0.25 + 0.75 * g);
        org = over(org, lamp, fillSd(sdCircle(q, lc, 0.42), aa));
        org = over(org, vec3(1.0), fillSd(sdCircle(q, lc + vec2(0.1, 0.1), 0.1), aa) * (0.4 + 0.6 * g));
    } else if (type == 22u) {  // cystocyte: a hexagonal two-layer wall around the resting core
        org = over(org, darker, fillSd(abs(sdHex(q, 0.62)) - 0.03, aa));
        org = over(org, darker, fillSd(abs(sdHex(q, 0.5)) - 0.02, aa) * 0.8);
        org = over(org, base * 0.6, fillSd(sdCircle(q, vec2(0.0), 0.32), aa));
        org = over(org, darker, fillSd(sdCircle(q, vec2(0.0), 0.13), aa));
    } else if (type == 23u) {  // grappler: two hooked claws at the front; they close when the grip is on
        float grip = clamp(p0, 0.0, 1.0);
        for (int s = -1; s <= 1; s += 2) {
            float sg = float(s);
            vec2 a0 = vec2(0.45, 0.42 * sg);
            vec2 a1 = vec2(0.92, mix(0.38, 0.12, grip) * sg);
            vec2 a2 = vec2(0.82, mix(0.18, 0.0, grip) * sg);
            org = over(org, darker, fillSd(sdSeg(q, a0, a1) - 0.05, aa));
            org = over(org, darker, fillSd(sdSeg(q, a1, a2) - 0.04, aa));
        }
        org = over(org, dark, fillSd(abs(sdCircle(q, vec2(0.1, 0.0), 0.26)) - 0.04, aa));  // contractile ring
    } else if (type == 24u) {  // fibroblast: thick collagen fibres running across the cell
        for (int i = 0; i < 5; ++i) {
            float y = -0.6 + float(i) * 0.3;
            vec2 a0 = vec2(-0.9, y * 0.7 + 0.12 * sin(y * 6.0 + seed));
            vec2 a1 = vec2(0.9, y * 0.7 + 0.12 * sin(y * 6.0 + seed + 1.0));
            org = over(org, mix(base, vec3(1.0), 0.25), fillSd(sdSeg(q, a0, a1) - 0.055, aa));
        }
        org = over(org, mix(base, vec3(1.0), 0.5), fillSd(sdCircle(q, vec2(0.0), 0.12), aa));
    } else if (type == 25u) {  // baroreceptor: pressure pads on the rim that light up on contact
        float press = clamp(p0, 0.0, 1.0);
        for (int i = 0; i < 8; ++i) {
            vec2 dir = polar(float(i) * TAU / 8.0 + seed * 0.2, 1.0);
            float dd = sdCircle(q, dir * 0.82, 0.13 + 0.05 * press);
            org = over(org, mix(dark, vec3(1.0, 0.95, 0.7), press), fillSd(dd, aa));
        }
    } else if (type == 26u) {  // erythrocyte: a pale biconcave disc, no nucleus (it has none)
        org = over(org, mix(base, vec3(1.0), 0.35), fillSd(abs(d - 0.55) - 0.16, aa) * 0.55);
        org = over(org, mix(base, vec3(1.0), 0.25), fillSd(abs(d - 0.55) - 0.05, aa) * 0.5);
    } else if (type == 27u) {  // chronocyte: clock hands; the sweep shows the phase, the flash the pulse
        float fire = clamp(p0, 0.0, 1.0);
        for (int i = 0; i < 12; ++i) {
            vec2 c = polar(float(i) * TAU / 12.0, 0.8);
            org = over(org, dark, fillSd(sdCircle(q, c, 0.03), aa));
        }
        float sweep = fract(t * 0.5 + seed);
        org = over(org, dark, fillSd(sdSeg(q, vec2(0.0), polar(sweep * TAU, 0.66)) - 0.025, aa));
        org = over(org, mix(base * 0.8, vec3(1.0), 0.7), fillSd(sdCircle(q, vec2(0.0), 0.11 + 0.06 * fire), aa));
    } else if (type == 28u) {  // cyclosporinocyte: a capsule ring, closed on itself like the drug that names it
        for (int i = 0; i < 3; ++i) {
            float r = 0.34 + 0.16 * float(i);
            org = over(org, mix(base, vec3(1.0), 0.2), fillSd(abs(d - r) - 0.022, aa));
        }
        org = over(org, mix(base, vec3(1.0), 0.4), fillSd(sdCircle(q, vec2(0.0), 0.12), aa));
    } else if (type == 29u) {  // granulocyte: granules that fill up with toxin as it collects them
        float tox = clamp(p0, 0.0, 1.0);
        for (int i = 0; i < 9; ++i) {
            float a = seed * 1.7 + float(i) * 2.399;  // golden-angle spiral: evenly spread, no clumps
            float r = 0.2 + 0.55 * fract(float(i) * 0.3819);
            vec2 c = polar(a, r);
            float rr = 0.055 + 0.05 * step(1.0 - tox * 9.0, 9.0 - float(i));  // filled granules turn green
            org = over(org, mix(base, vec3(0.55, 0.9, 0.45), step(9.0 - float(i), tox * 9.0)),
                       fillSd(sdCircle(q, c, rr), aa));
        }
    } else if (type == 30u) {  // griffocyte: hook claws whose grip tightens with the signal (p0)
        float grip = clamp(p0, 0.0, 1.0);
        for (int s = -1; s <= 1; s += 2) {
            float sg = float(s);
            vec2 a0 = vec2(0.1, 0.75 * sg);
            vec2 a1 = vec2(0.55, mix(0.6, 0.25, grip) * sg);
            org = over(org, darker, fillSd(sdSeg(q, a0, a1) - 0.06, aa));
        }
        // the cup it grips the dish with
        org = over(org, mix(base, vec3(0.8, 0.8, 0.85), 0.4), fillSd(abs(d - (0.86 - 0.1 * grip)) - 0.035, aa) *
            (1.0 - smoothstep(-PI * 0.5, 0.0, ang)));
    } else if (type == 31u) {  // spirocyte: a helix drawn around the axis, turning as it spins
        float spin = p1;  // accumulated spin, in radians
        for (int i = 0; i < 5; ++i) {
            float u = (float(i) + 0.5) / 5.0;
            float a = spin * 2.0 + u * TAU;
            vec2 c = vec2(cos(a) * 0.45 * u - 0.45, sin(a) * 0.45 * u);
            org = over(org, mix(base, vec3(1.0), 0.35), fillSd(sdCircle(q, c, 0.07), aa));
        }
        org = over(org, dark, fillSd(sdSeg(q, vec2(-0.9, 0.0), vec2(0.9, 0.0)) - 0.03, aa));
    } else if (type == 32u) {  // chromocyte: colour gradient from black to the chosen hue, rainbow, or by signal
        // p0 carries the parameter 0..1 (-1..1 in the simulation), p1 the preset (0 gradient, 1 rainbow, 2 by signal).
        float par = clamp(p0, 0.0, 1.0);
        float preset = p1;
        vec3 grad;
        if (preset > 1.5) {
            // by signal strength: black -> hue -> white
            grad = mix(vec3(0.0), base, par);
            grad = mix(grad, vec3(1.0), max(0.0, par - 0.85) / 0.15 * 0.6);
        } else if (preset > 0.5) {
            // rainbow around the perimeter
            float h = fract(ang / TAU + par);
            vec3 rainbow = 0.5 + 0.5 * cos(TAU * (h + vec3(0.0, 0.33, 0.67)));
            grad = mix(rainbow, vec3(1.0), smoothstep(0.8, 1.0, par));
        } else {
            // black -> chosen colour, brightness rising with the parameter
            grad = mix(vec3(0.02), base, par) * (0.55 + 0.45 * par);
        }
        // The gradient runs across the cell along its own axis.
        float axis = clamp(q.x * 0.5 + 0.5, 0.0, 1.0);
        fill = mix(grad * 0.35, grad, axis);
        org = over(org, mix(grad, vec3(1.0), 0.4), fillSd(abs(d - 0.86) - 0.02, aa) * 0.6);
    } else if (type == 33u) {  // signal cell: input and output gates that light with the values
        for (int i = 0; i < 4; ++i) {
            vec2 c = polar(float(i) * TAU / 4.0 + PI * 0.25, 0.62);
            org = over(org, mix(dark, vec3(0.4, 0.85, 1.0), 0.3), fillSd(sdCircle(q, c, 0.13), aa));
        }
        float pulse = 0.5 + 0.5 * sin(t * 3.0 + seed);
        org = over(org, mix(base, vec3(0.5, 0.9, 1.0), pulse), fillSd(abs(d - 0.5) - 0.035, aa));
        org = over(org, mix(base, vec3(1.0), 0.4), fillSd(sdCircle(q, vec2(0.0), 0.14), aa));
    }

    // Nucleus with nucleolus (not in virocytes, full of virions, nor lipocytes, pushed aside by the droplet).
    // The nucleus is round, drawn in the cell's own (undeformed) frame, and sits at the centre of the deformed outline
    // (vCenter): a cell pressed flat on one side bulges on the others, and its nucleus moves with the bulk of it
    // instead of staying stuck against the flat face. Only the cnidocyte and luminocyte, whose organ fills the middle,
    // keep it at the back.
    // An erythrocyte has no nucleus, so none is drawn for it (type 26); the same goes for the signal cell,
    // whose middle is the gate ring, and the electrocyte, all plates.
    if (type != 8u && type != 4u && type != 13u && type != 16u && type != 18u && type != 20u && type != 22u && type != 26u &&
        type != 33u) {
        vec2 nc = vCenter + ((type == 19u || type == 21u) ? vec2(-0.6, 0.0) : vec2(0.0));
        float nr = (type == 15u) ? 0.3 : (type == 10u ? 0.21 : (type == 19u || type == 21u ? 0.17 : 0.24));
        float aRaw = aa * sr;
        org = over(org, dark, fillSd(sdCircle(qRaw, nc, nr), aRaw));
        org = over(org, darker, fillSd(sdCircle(qRaw, nc, nr * 0.38), aRaw));
    } else if (type == 13u) {
        org = over(org, light, fillSd(sdCircle(qRaw, vCenter, 0.17), aa * sr));
        org = over(org, dark, fillSd(abs(sdCircle(qRaw, vCenter, 0.17)) - 0.025, aa * sr));
    }

    if ((flags & 2u) != 0u) {  // infected by a virus: violet specks
        for (int i = 0; i < 4; ++i) {
            vec2 c = polar(seed * 4.0 + float(i) * 1.6 + t * 0.5, 0.45);
            org = over(org, vec3(0.75, 0.35, 0.95), fillSd(sdHex(q - c, 0.06), aa));
        }
    }

    // Compose: fill, glyphs, then the membrane ring on top (crisp both sides).
    vec3 body = mix(fill, org.rgb / max(org.a, 1e-4), org.a);
    float memb = fillSd(rr - membW - d, aa);  // 1 inside the ring band
    body = mix(body, membCol, memb);
    // Zooming out, the membrane ring and the organelles shrink below a pixel: they dissolve into the cell's colour
    // (approaching the plain disc of the far zoom) instead of a dark sub-pixel ring that blinked black as the cell
    // moved by fractions of a pixel.
    body = mix(body, base, lod);
    float alpha = 0.95;
    if ((flags & 16u) != 0u) {  // editor phantom: translucent, washed-out
        body = mix(body, vec3(dot(body, vec3(0.33))), 0.3);
        alpha = 0.42;
    }
    if ((flags & 4u) != 0u) {  // starving: fades in as the mass nears death (0.25 -> 0.20 of the maximum), no on/off blink
        float s = clamp((0.25 - p1) / 0.05, 0.0, 1.0);
        float g = dot(body, vec3(0.33));
        body = mix(body, vec3(g), 0.5 * s);
        alpha = mix(alpha, 0.8, s);
    }
    col = over(col, body, bodyA * alpha);

    // Selection ring.
    if ((flags & 1u) != 0u) {
        float pulse = 0.75 + 0.25 * sin(t * 5.0);
        float ring = fillSd(abs(d - 1.2) - 0.035, aa);
        col = over(col, vec3(0.95, 0.98, 1.0), ring * pulse);
    }
    // Editor: dashed ring around every cell of the mode being edited.
    if ((flags & 32u) != 0u) {
        float dash = step(0.5, fract((ang + PI) / TAU * 16.0));
        float ring = fillSd(abs(d - 1.1) - 0.035, aa) * dash;
        col = over(col, vec3(1.0, 0.62, 0.18), ring * 0.95);
    }
    // Hover ring.
    if ((flags & 8u) != 0u) {
        float ring = fillSd(abs(d - 1.14) - 0.02, aa);
        col = over(col, vec3(0.8, 0.9, 1.0), ring * 0.55);
    }
    col.rgb *= mix(nightTint(), vec3(1.0), type == 21u ? clamp(p0, 0.0, 1.0) : 0.0);  // night; a lit photophore keeps its light
    outColor = col * (glassClip() * vColor.a);
}
