#version 450
// The plain screen copy: one colour attachment of the source target, sampled 1:1 through the
// canonical full-screen triangle (see builtin_fullscreen.vert). This is the text behind what a bare
// ScreenPass used to get implicitly — now it is a PROGRAM the host names, so a copy that wants
// different filtering, a colour transform or a tonemap is a different program, not a backend
// setting.
//
// The BINDING is the attachment: binding N reads the source's colour attachment N, which is the
// full-screen program ABI (see ScreenPass::setProgram). screenCopyProgram(N) emits this same text
// with N substituted, which is why the line below is spelled exactly as the factory looks for it.
layout(location = 0) in vec2 vine_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D screen_tex;
void main()
{
    // vsg projects world-up to the top image row (reverse-Y perspective), and the fullscreen
    // triangle's vine_uv.y == 0 sits at the top of the screen, so sampling vine_uv directly keeps the
    // source upright (no Y flip) — the same orientation readColorBuffer() hands back.
    out_color = texture(screen_tex, vine_uv);
}
