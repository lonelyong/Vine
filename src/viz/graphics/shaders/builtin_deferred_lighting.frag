#version 450
layout(location = 0) in vec2 vn_uv;
layout(location = 0) out vec4 vn_out_color;
// The canonical G-buffer's four colour attachments, bound by INDEX (the fullscreen ABI: attachment N sits at
// binding N - see ContentImages / BuiltinShaders::deferredLightProgram). What each one CARRIES is the packing
// contract in vine_gbuffer.glsl, not this file's business.
layout(binding = 0) uniform sampler2D vn_albedo_tex;
layout(binding = 1) uniform sampler2D vn_normal_tex;
layout(binding = 2) uniform sampler2D vn_emissive_tex;
layout(binding = 3) uniform sampler2D vn_pos_tex;
// The lights: the ONE declaration of the parameter blocks (vine_params.glsl), at the binding the full-screen
// arrangement gives it (after the four G-buffer samplers and the depth slot). The same block the forward
// content stage binds, so one scene lights identically on both paths; the typed evaluation is the fragment's
// `vnEvaluateLight`.
#define VN_PARAM_LIGHTS
#include "vine_params.glsl"
// The BRDF the forward stage shades with, called with the surface this pass REBUILDS from texels - which is
// what makes "one material, one answer on either path" a structural fact rather than a promise (R3/T3).
#include "vine_brdf.glsl"
// The attachment packing, from its one home: the octahedral normal decode this pass needs.
#include "vine_gbuffer.glsl"
// The shadowed variant of this program inserts its sampler and its VineShadowBlock here
// (BuiltinShaders::deferredLightProgram); the unshadowed one declares neither, so the pass that
// draws it must provide neither. The line below IS the insertion point.
// VN_SHADOW_BINDINGS
void main()
{
    // vsg projects world-up to the top G-buffer row (reverse-Y
    // perspective) and vn_uv.y == 0 is the top of the screen,
    // so vn_uv samples the buffers upright (no Y flip).
    vec4 vn_pos4 = texture(vn_pos_tex, vn_uv);
    // Background: nothing was drawn here. The G-buffer's position attachment is cleared to transparent
    // black and every fragment the geometry pass writes carries w = 1 (builtin_gbuffer.frag), so the WRITE
    // MASK - not the distance - separates background from geometry. A distance test cannot: the fragment
    // the camera is touching stores view position ~(0,0,0), and shading it as background paints a fixed
    // 0.06-grey hole over geometry that is right in front of the lens (registered as D4, fixed 2026-09-25).
    if (vn_pos4.a < 0.5) { vn_out_color = vec4(vec3(0.06), 1.0); return; }
    vec3 vn_pos = vn_pos4.xyz;
    // The surface, rebuilt exactly as the forward stage built it: base colour from attachment 0, the shading
    // normal and the two factors from attachment 1, emission and occlusion from attachment 2. The reflectance
    // is NOT read - it is derived from base and metallic by the BRDF's first line, the same way on both paths.
    vec3  vn_base_color = texture(vn_albedo_tex, vn_uv).rgb;
    vec4  vn_n4         = texture(vn_normal_tex, vn_uv);
    vec3  vn_n          = vnOctahedralDecode(vn_n4.rg);
    float vn_roughness  = clamp(vn_n4.b, 0.0, 1.0);
    float vn_metallic   = clamp(vn_n4.a, 0.0, 1.0);
    vec4  vn_emissive4  = texture(vn_emissive_tex, vn_uv);
    vec3  vn_view_dir   = normalize(-vn_pos);
    // Ambient is the analytic fill and is the only term the occlusion slot scales; emission is the surface's
    // own light and is added once, unshadowed - the same two rules the forward stage follows.
    vec3 vn_lit_color = vn_base_color * (1.0 - vn_metallic) * (vn_lights.ambient.rgb * vn_lights.ambient.a) * vn_emissive4.a;
    vn_lit_color += vn_emissive4.rgb;
    for (int vn_i = 0; vn_i < 8; ++vn_i)
    {
        // The light itself (kind, position, range, cone) is evaluated in ONE place (vnEvaluateLight, see
        // vine_params.glsl); this program contributes the surface it lands on.
        vec3  vn_L;
        float vn_attenuation = vnEvaluateLight(vn_i, vn_pos, vn_L);
        if (vn_attenuation <= 0.0)
        {
            continue;   // an unused slot, or a light that cannot reach this fragment
        }
        vec3 vn_c = vn_lights.color_intensity[vn_i].rgb;
        float vn_a = vn_lights.color_intensity[vn_i].a;
        float vn_ndl = max(dot(vn_n, vn_L), 0.0);
        // The shadowed variant scales ndl by the map here; the unshadowed program has nothing to
        // scale, and this line is the insertion point either way.
        // VN_SHADOW_TERM
        // One term for the whole BRDF, gated by ndl like the forward stage's: the cosine is the only part of
        // the product that can go negative, so the gate belongs around the term rather than inside it.
        if (vn_ndl > 0.0)
        {
            vn_lit_color += vnEvaluateBRDF(vn_base_color, vn_metallic, vn_roughness, vn_n, vn_view_dir, vn_L)
                                           * vn_c * vn_a * vn_ndl * vn_attenuation;
        }
    }
    vn_out_color = vec4(vn_lit_color, 1.0);
}
