#version 450
layout(location = 0) in vec2 vine_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D albedo_tex;
layout(binding = 1) uniform sampler2D normal_tex;
layout(binding = 2) uniform sampler2D spec_tex;
layout(binding = 3) uniform sampler2D pos_tex;
// The shadowed variant of this program inserts its sampler and its VineShadowBlock here
// (BuiltinShaders::deferredLightProgram); the unshadowed one declares neither, so the pass that
// draws it must provide neither. The line below IS the insertion point.
// VINE_SHADOW_BINDINGS
layout(push_constant) uniform PushConstants
{
    vec4 ambient;
    // Reserved for a program that reconstructs the view position from the depth buffer (near, far,
    // proj[0][0], proj[1][1]); this program does not read it - it samples `pos_tex`.
    vec4 projparms;
    vec4 sun_dir[3];
    vec4 sun_color[3];
} pc;
void main()
{
    // vsg projects world-up to the top G-buffer row (reverse-Y
    // perspective) and vine_uv.y == 0 is the top of the screen,
    // so vine_uv samples the buffers upright (no Y flip).
    vec2 uv = vine_uv;
    vec3 albedo = texture(albedo_tex, uv).rgb;
    vec4 n4 = texture(normal_tex, uv);
    vec3 n = n4.xyz;
    vec3 pos = texture(pos_tex, uv).xyz;
    // Background (stored position ~0 where nothing was drawn).
    if (dot(pos, pos) < 1e-6) { out_color = vec4(vec3(0.06), 1.0); return; }
    n = normalize(n);
    // Per-pixel material from the G-buffer: specular colour rides
    // attachment 2, shininess rides the normal attachment's alpha.
    vec3 spec_col = clamp(texture(spec_tex, uv).rgb, 0.0, 1.0);
    float shininess = max(n4.a * 256.0, 1.0);
    vec3 view_dir = normalize(-pos);
    vec3 color = albedo * (pc.ambient.rgb * pc.ambient.a);
    for (int i = 0; i < 3; ++i)
    {
        vec3 d = pc.sun_dir[i].xyz;
        vec3 c = pc.sun_color[i].rgb;
        float a = pc.sun_color[i].a;
        if (dot(d, d) < 1e-6) continue;
        vec3 L = normalize(-d);
        float ndl = max(dot(n, L), 0.0);
        // The shadowed variant scales ndl by the map here; the unshadowed program has nothing to
        // scale, and this line is the insertion point either way.
        // VINE_SHADOW_TERM
        color += albedo * c * a * ndl;
        // Specular is gated by ndl like the diffuse term:
        // a face turned away from the light must not receive
        // a highlight (without the gate, shadowed faces pick
        // up bright white patches that read as glass / see-
        // through on opaque surfaces).
        if (ndl > 0.0)
        {
            vec3 H = normalize(L + view_dir);
            float spec = pow(max(dot(n, H), 0.0), shininess);
            color += c * a * spec * spec_col * ndl;
        }
    }
    out_color = vec4(color, 1.0);
}
