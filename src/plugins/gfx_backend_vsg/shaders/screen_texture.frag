#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D screen_tex;
void main()
{
    // vsg projects world-up to the top image row (reverse-Y perspective), and
    // the fullscreen triangle's v_uv.y == 0 sits at the top of the screen, so
    // sampling v_uv directly keeps the source upright (no Y flip).
    out_color = texture(screen_tex, v_uv);
}
