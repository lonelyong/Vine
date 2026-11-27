#version 450
// The backend compiles this stage once per variant, and a variant's define is delivered
// ONLY when the source asks for it on this line: a name that is missing here is silently dropped (the
// branch that tests it stays dead, with no error from any layer). Keep the list in sync with the
// backend's variants — scripts/vn_shader_check.sh compiles every combination of these names.
#pragma import_defines (VN_VERTEX_COLOR, VN_TEXTURED, VN_TEXCOORD_UV, VN_TEXCOORD_CUBE)
layout(location = 0) in vec3 vn_view_pos;
layout(location = 1) in vec3 vn_view_normal;
#ifdef VN_VERTEX_COLOR
layout(location = 2) in vec4 vn_color;
#endif
#ifdef VN_TEXTURED
#if defined(VN_TEXCOORD_CUBE)
layout(location = 3) in vec3 vn_uv;
#elif defined(VN_TEXCOORD_UV)
layout(location = 3) in vec2 vn_uv;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
#endif
layout(location = 0) out vec4 vn_out_color;

// The parameter blocks this stage reads, from their ONE declaration (vine_params.glsl): the material, the
// per-drawable block and the view's lights, at the bindings of the content set. What a block MEANS is
// stated next to its bytes there; this file states only what the surface does with them.
#define VN_PARAM_MATERIAL
#define VN_PARAM_DRAW
#define VN_PARAM_LIGHTS
#include "vine_params.glsl"
// The material's texture set (its slots, their packing, and what an absent slot contributes) - the SAME
// fragment the G-buffer writer includes, so the forward and the deferred picture of one material cannot come
// from two readings of one map. It declares the five samplers itself, so this stage states only the coordinate.
#include "vine_material.glsl"
// The one specular model both paths shade with (GGX / Smith / Schlick).
#include "vine_brdf.glsl"

// The shadow this pass declared, if any: the map it named as an INPUT and the block that places a
// fragment in it (ShaderAbi.hpp). DECLARED ALWAYS, unlike the deferred lighting program's two
// variants: a content set is shared per (target, depth policy) and a session picks the content program
// once, so a shadowed variant would double that cache for every target — and a host's own content
// program would still have no shadowed twin to pick. One text, both paths, switched at RUNTIME by
// `vn_shadow.params.x` (0 = no shadow reached this pass), which is what the ABI's switch is for.
// The line below IS the insertion point (BuiltinShaders::forwardProgram).
// VN_SHADOW_BINDINGS

void main()
{
#ifdef VN_FLAT
    // Flat shading: the FACE normal, from the screen-space derivatives of the view position.
    // One normal for the whole triangle — which is what "flat" means — and no per-vertex
    // normals at all, so this preset reuses the same vertex stage as the smooth one (see
    // builtinProgram).
    //
    // The cross order is the one that faces the VIEWER: Vulkan's framebuffer rows grow downward,
    // so dFdy points the other way from the y-up convention these derivatives are usually written
    // in, and dFdx x dFdy comes out pointing away from the camera (measured: the lit side of a
    // surface facing the camera stayed at its ambient term until the operands were swapped).
    vec3 vn_n = normalize(cross(dFdy(vn_view_pos), dFdx(vn_view_pos)));
#else
    // A BACK FACE IS THE SAME SURFACE SEEN FROM BEHIND, so the interpolated normal is turned to face the viewer
    // (see vnFacingNormal in vine_material.glsl). The FLAT branch above needs no turn: its cross product is taken
    // from the screen-space derivatives, so it already faces the camera.
    vec3 vn_n = vnFacingNormal(normalize(vn_view_normal));
#endif
    vec3 vn_view_dir = normalize(-vn_view_pos);
    // The surface: this stage's geometric normal, then the material's slots on top of its factors - the
    // same function the G-buffer writer calls, so one material has ONE surface on both paths.
#ifdef VN_TEXTURED
    VineSurface vn_surface = vnSampleMaterial(vn_n, vn_view_pos, vn_uv);
#else
    VineSurface vn_surface = vnSurfaceFromFactors(vn_n);
#endif
    // THE ALPHA MODE, from its one owner (vine_alpha.glsl): the product of the drawable's opacity and the material's
    // own alpha means a THRESHOLD for Mask (the fragment either survives opaque or is cut away), the OUTPUT alpha for
    // Blend, and nothing at all for Opaque - which keeps the drawable's own opacity (graphics-material.md 2.7/3.5).
    // The same call cuts the same holes in a depth pass, so an object's shadow and its picture state one silhouette.
    //
    // That opacity is a PER-DRAWABLE VALUE, not a per-vertex one: it arrives in the draw block and is rewritten in
    // place when the command's opacity changes, so a translucent drawable costs O(1) per frame instead of a pass over
    // its vertices (and the vertex colour stream stays free to carry authored colours, which its alpha does not
    // compete with). The engine sorts a drawable by this value plus the mode (`DrawItem::isTransparent`), so the
    // picture and the order read the same pair.
    float vn_alpha = vnAlphaCoverage(vn_surface.alpha, vn_draw.params.x);
#ifdef VN_VERTEX_COLOR
    // An AUTHORED vertex colour modulates the albedo only. Its alpha is
    // deliberately NOT an opacity input: opacity belongs to the draw block, so
    // one drawable's alpha cannot leak into another geometry that shares the
    // same vertex stream, and a model's fourth colour component means what the
    // model says it means.
    vn_surface.base_color *= vn_color.rgb;
#endif
#ifdef VN_UNLIT
    // Unlit: the surface colour IS the answer - no light term, no specular, and the drawable's opacity
    // unchanged. It is the mode that separates "is this the shape?" from "is this the shading?" (a viewer
    // that has to answer "why is it flat?" wants to be able to switch the lighting OFF first), and the one
    // a data-driven preview wants: the authored colour, with nothing invented on top of it.
    vn_out_color = vec4(vn_surface.base_color, vn_alpha);
    return;
#endif
#ifdef VN_NORMALS
    // View-space normals as colour, the debugging view every 3D tool carries: it answers "which way does
    // this surface face" with no light involved at all (blue = facing the camera). Alpha is 1: the picture
    // is a readout, not content, so nothing translucent about the content should show through it.
    // It shows the SHADING normal - the one the lit branch below shades with - so a normal map's effect is
    // visible in the readout instead of being a difference between two pictures nobody can compare.
    vn_out_color = vec4(normalize(vn_surface.normal) * 0.5 + 0.5, 1.0);
    return;
#endif
    // Ambient is the engine's analytic fill, not a light: it is what a surface would receive from everywhere,
    // so the OCCLUSION slot scales it (an AO map says which cavities the environment cannot reach) while the
    // direct lights below are not scaled by it - a light that reaches a cavity still reaches it, and
    // screen-space occlusion is out of this design's scope (graphics-material.md 1.3).
    vec3 vn_lit_color = vn_surface.base_color * (1.0 - vn_surface.metallic) * (vn_lights.ambient.rgb * vn_lights.ambient.a)
                        * vn_surface.occlusion;
    // Emission is the surface's own light: it is added once, unscaled by any light and unshadowed - a glowing
    // surface glows in the dark, and a shadow cannot dim it.
    vn_lit_color += vn_surface.emissive;
    // The shadow term below is the SAME text the deferred lighting program inserts, and it reads the
    // fragment's view position by one name in both programs: here it arrives as a varying, there it is
    // read out of the G-buffer.
    vec3 vn_pos = vn_view_pos;
    for (int vn_i = 0; vn_i < 8; ++vn_i)
    {
        // What a light IS (kind, position, range, cone) is the block's business and is evaluated in one place
        // (vnEvaluateLight, vine_params.glsl); what is left here is this surface's term.
        vec3  vn_L;
        float vn_attenuation = vnEvaluateLight(vn_i, vn_pos, vn_L);
        if (vn_attenuation <= 0.0)
        {
            continue;   // an unused slot, or a light that cannot reach this fragment
        }
        vec3 vn_c = vn_lights.color_intensity[vn_i].rgb;
        float vn_a = vn_lights.color_intensity[vn_i].a;
        float vn_ndl = max(dot(vn_surface.normal, vn_L), 0.0);
        // The shadow ABI's term goes here: it scales this light's ndl by whether the map found a
        // caster in front of this fragment. The line below IS that insertion point, and it is the same
        // text the deferred lighting program carries (so the two paths cannot drift).
        // VN_SHADOW_TERM
        // One term for the whole BRDF, gated by ndl: a face turned away from the light receives nothing at
        // all (its BRDF value would still be non-zero, because only the cosine is zero), which is also why
        // the gate sits here rather than around the specular lobe alone.
        if (vn_ndl > 0.0)
        {
            // The SAME function the deferred lighting program calls (vine_brdf.glsl): radiance, the cosine
            // and the light's attenuation are this stage's factors, the rest is the surface's.
            vn_lit_color += vnEvaluateBRDF(vn_surface.base_color, vn_surface.metallic, vn_surface.roughness, vn_surface.normal,
                                           vn_view_dir, vn_L) * vn_c * vn_a * vn_ndl * vn_attenuation;
        }
    }
    vn_out_color = vec4(vn_lit_color, vn_alpha);
}
