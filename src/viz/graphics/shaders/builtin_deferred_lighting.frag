#version 450
layout(location = 0) in vec2 vine_uv;
layout(location = 0) out vec4 out_color;
layout(binding = 0) uniform sampler2D albedo_tex;
layout(binding = 1) uniform sampler2D normal_tex;
layout(binding = 2) uniform sampler2D spec_tex;
layout(binding = 3) uniform sampler2D pos_tex;
// The lights: the ONE declaration of the parameter blocks (vine_params.glsl), at the binding the full-screen
// arrangement gives it (after the four G-buffer samplers and the depth slot). The same block the forward
// content stage binds, so one scene lights identically on both paths; the typed evaluation is the fragment's
// `vineEvaluateLight`.
#define VINE_PARAM_LIGHTS
#include "vine_params.glsl"
// The shadowed variant of this program inserts its sampler and its VineShadowBlock here
// (BuiltinShaders::deferredLightProgram); the unshadowed one declares neither, so the pass that
// draws it must provide neither. The line below IS the insertion point.
// VINE_SHADOW_BINDINGS
void main()
{
    // vsg projects world-up to the top G-buffer row (reverse-Y
    // perspective) and vine_uv.y == 0 is the top of the screen,
    // so vine_uv samples the buffers upright (no Y flip).
    vec2 uv = vine_uv;
    vec3 albedo = texture(albedo_tex, uv).rgb;
    vec4 n4 = texture(normal_tex, uv);
    vec3 n = n4.xyz;
    vec4 pos4 = texture(pos_tex, uv);
    vec3 pos = pos4.xyz;
    // Background: nothing was drawn here. The G-buffer's position attachment is cleared to transparent
    // black and every fragment the geometry pass writes carries w = 1 (builtin_gbuffer.frag), so the WRITE
    // MASK - not the distance - separates background from geometry. A distance test cannot: the fragment
    // the camera is touching stores view position ~(0,0,0), and shading it as background paints a fixed
    // 0.06-grey hole over geometry that is right in front of the lens (registered as D4, fixed 2026-09-25).
    if (pos4.a < 0.5) { out_color = vec4(vec3(0.06), 1.0); return; }
    n = normalize(n);
    // Per-pixel material from the G-buffer: the reflectance (the metallic-roughness pair's F0) rides
    // attachment 2, and the Blinn-Phong exponent the M1 bridge derives rides the normal attachment's alpha.
    vec3 spec_col = clamp(texture(spec_tex, uv).rgb, 0.0, 1.0);
    float shininess = max(n4.a * 256.0, 1.0);
    vec3 view_dir = normalize(-pos);
    vec3 color = albedo * (lights.ambient.rgb * lights.ambient.a);
    for (int i = 0; i < 8; ++i)
    {
        // The light itself (kind, position, range, cone) is evaluated in ONE place (vineEvaluateLight, see
        // vine_params.glsl); this program contributes the surface it lands on.
        vec3  L;
        float attenuation = vineEvaluateLight(i, pos, L);
        if (attenuation <= 0.0)
        {
            continue;   // an unused slot, or a light that cannot reach this fragment
        }
        vec3 c = lights.color_intensity[i].rgb;
        float a = lights.color_intensity[i].a;
        float ndl = max(dot(n, L), 0.0);
        // The shadowed variant scales ndl by the map here; the unshadowed program has nothing to
        // scale, and this line is the insertion point either way.
        // VINE_SHADOW_TERM
        color += albedo * c * a * ndl * attenuation;
        // Specular is gated by ndl like the diffuse term:
        // a face turned away from the light must not receive
        // a highlight (without the gate, shadowed faces pick
        // up bright white patches that read as glass / see-
        // through on opaque surfaces).
        if (ndl > 0.0)
        {
            vec3 H = normalize(L + view_dir);
            float spec = pow(max(dot(n, H), 0.0), shininess);
            color += c * a * spec * spec_col * ndl * attenuation;
        }
    }
    out_color = vec4(color, 1.0);
}
