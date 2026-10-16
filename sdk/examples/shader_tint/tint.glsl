
// ---- «Сиреневые клетки» (sdk/examples/shader_tint): appended to the game's cell.frag, whose main() was renamed
// cellfapBaseMain(). The cell is drawn as usual, then tinted: everything above (inputs, push constants, outColor)
// is the game's interface, described in docs/mods.md («Шейдеры»).
void main() {
    cellfapBaseMain();
    // outColor is premultiplied by alpha: scaling rgb keeps the edges clean.
    float pulse = 0.5 + 0.5 * sin(pc.params.x * 1.5);  // params.x: time, s
    vec3 lilac = vec3(0.95, 0.62, 1.15);
    outColor.rgb = mix(outColor.rgb, min(outColor.rgb * lilac, vec3(outColor.a)), 0.45 + 0.35 * pulse);
}
