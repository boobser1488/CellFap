
// ---- «Сиреневые клетки» (sdk/examples/shader_tint): appended to the game's cell.frag, whose main() was renamed
// cellfapBaseMain(). The cell is drawn as usual, then tinted: everything above (inputs, push constants, outColor)
// is the game's interface, described in docs/mods.md («Шейдеры»).
void main() {
    cellfapBaseMain();
    // outColor is premultiplied by alpha: a colour built from its brightness stays premultiplied too.
    float pulse = 0.5 + 0.5 * sin(pc.params.x * 1.5);  // params.x: time, s
    float light = dot(outColor.rgb, vec3(0.30, 0.59, 0.11));
    vec3 lilac = min(vec3(0.85, 0.55, 1.0) * (0.25 * outColor.a + 1.2 * light), vec3(outColor.a));
    outColor.rgb = mix(outColor.rgb, lilac, 0.6 + 0.3 * pulse);
}
