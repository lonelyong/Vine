#version 450
layout(location = 0) in vec2 vine_uv;
layout(location = 0) out vec4 out_color;
// The canonical G-buffer's four colour attachments, bound by INDEX (the fullscreen ABI: attachment N sits at
// binding N - see ContentImages / BuiltinShaders::deferredLightProgram). What each one CARRIES is the packing
// contract in vine_gbuffer.glsl, not this file's business.
layout(binding = 0) uniform sampler2D albedo_tex;
layout(binding = 1) uniform sampler2D normal_tex;
layout(binding = 2) uniform sampler2D emissive_tex;
layout(binding = 3) uniform sampler2D pos_tex;
// The lights: the ONE declaration of the parameter blocks (vine_params.glsl), at the binding the full-screen
// arrangement gives it (after the four G-buffer samplers and the depth slot). The same block the forward
// content stage binds, so one scene lights identically on both paths; the typed evaluation is the fragment's
// `vineEvaluateLight`.
#define VINE_PARAM_LIGHTS
#include "vine_params.glsl"
// The BRDF the forward stage shades with, called with the surface this pass REBUILDS from texels - which is
// what makes "one material, one answer on either path" a structural fact rather than a promise (R3/T3).
#include "vine_brdf.glsl"
// The attachment packing, from its one home: the octahedral normal decode this pass needs.
#include "vine_gbuffer.glsl"
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
    vec4 pos4 = texture(pos_tex, uv);
    // Background: nothing was drawn here. The G-buffer's position attachment is cleared to transparent
    // black and every fragment the geometry pass writes carries w = 1 (builtin_gbuffer.frag), so the WRITE
    // MASK - not the distance - separates background from geometry. A distance test cannot: the fragment
    // the camera is touching stores view position ~(0,0,0), and shading it as background paints a fixed
    // 0.06-grey hole over geometry that is right in front of the lens (registered as D4, fixed 2026-09-25).
    if (pos4.a < 0.5) { out_color = vec4(vec3(0.06), 1.0); return; }
    vec3 pos = pos4.xyz;
    // The surface, rebuilt exactly as the forward stage built it: base colour from attachment 0, the shading
    // normal and the two factors from attachment 1, emission and occlusion from attachment 2. The reflectance
    // is NOT read - it is derived from base and metallic by the BRDF's first line, the same way on both paths.
    vec3  base_color = texture(albedo_tex, uv).rgb;
    vec4  n4         = texture(normal_tex, uv);
    vec3  n          = vineOctahedralDecode(n4.rg);
    float roughness  = clamp(n4.b, 0.0, 1.0);
    float metallic   = clamp(n4.a, 0.0, 1.0);
    vec4  emissive4  = texture(emissive_tex, uv);
    vec3  view_dir   = normalize(-pos);
    // Ambient is the analytic fill and is the only term the occlusion slot scales; emission is the surface's
    // own light and is added once, unshadowed - the same two rules the forward stage follows.
    vec3 color = base_color * (1.0 - metallic) * (lights.ambient.rgb * lights.ambient.a) * emissive4.a;
    color += emissive4.rgb;
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
        // One term for the whole BRDF, gated by ndl like the forward stage's: the cosine is the only part of
        // the product that can go negative, so the gate belongs around the term rather than inside it.
        if (ndl > 0.0)
        {
            color += vineEvaluateBRDF(base_color, metallic, roughness, n, view_dir, L) * c * a * ndl * attenuation;
        }
    }
    out_color = vec4(color, 1.0);
}
