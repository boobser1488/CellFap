// Shared push constants for all scene pipelines.
//  cam    : xy = camera centre (world), zw = world->clip scale
//  params : x = time, y = world units per pixel, z = dish radius, w = light level (0..1)
//  extra  : x = light direction (radians), y = grid alpha, z = light range (0..1), w = theme (1 classic, 0 dark)
//  extra2 : x = night (0 day .. 1 deep night)
layout(push_constant) uniform PC {
    vec4 cam;
    vec4 params;
    vec4 extra;
    vec4 extra2;
} pc;

vec2 worldToClip(vec2 w) { return (w - pc.cam.xy) * pc.cam.zw; }

float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash21(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Premultiplied "over" compositing.
vec4 over(vec4 dst, vec3 rgb, float a) { return vec4(rgb * a + dst.rgb * (1.0 - a), a + dst.a * (1.0 - a)); }

// Night (day cycle): the whole scene dims to a cool blue, light emitters excepted. 1 = no change.
vec3 nightTint() {
    float n = clamp(pc.extra2.x, 0.0, 1.0);
    vec3 t = mix(vec3(0.42, 0.47, 0.66), vec3(0.50, 0.52, 0.63), pc.extra.w);  // dark theme, classic theme
    return mix(vec3(1.0), t, n);
}
