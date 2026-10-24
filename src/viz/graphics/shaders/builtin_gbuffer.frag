#version 450
// The backend sets these per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests it
// stays dead. Keep the list in sync with the backend's variants - scripts/vine_shader_check.sh compiles
// every combination of these names.
#pragma import_defines (VINE_TEXTURED, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)
layout(location = 0) in vec3 vine_view_pos;
layout(location = 1) in vec3 vine_view_normal;
#ifdef VINE_TEXTURED
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 2) in vec3 vine_texcoord;
#elif defined(VINE_TEXCOORD_UV)
layout(location = 2) in vec2 vine_texcoord;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
#endif
layout(location = 0) out vec4 out_base_color;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_emissive;
layout(location = 3) out vec4 out_position;
// The material this stage PROJECTS into the G-buffer: the ONE declaration of the parameter blocks (see
// vine_params.glsl), so a deferred drawable and a forward one of the same material shade the same bytes.
// `base_color.a` is read for the ALPHA MODE's sake (Mask's threshold and Blend's exclusion), never written into
// an attachment - the attachments carry the surface, and the composite's alpha is the lighting pass's.
#define VINE_PARAM_MATERIAL
// The per-drawable block, read for `params.x`: the drawable's opacity is the Mask threshold's second input, and
// reading the SAME value the forward stage reads is what makes the two paths cut the same holes.
#define VINE_PARAM_DRAW
#include "vine_params.glsl"
// The SAME surface the forward stage shades: the material's five slots, their packing and what an absent slot
// contributes are stated once (vine_material.glsl), which is what keeps a deferred picture from disagreeing
// with the forward one about a material.
#include "vine_material.glsl"
// The attachment packing this stage writes and the lighting pass reads (vine_gbuffer.glsl): the octahedral
// normal encoding, and the note that says what rides each channel.
#include "vine_gbuffer.glsl"
void main()
{
    // The geometric normal, then the material's own surface on top of it: the shading normal a normal map
    // produces is what the G-buffer must carry, because the lighting pass cannot rebuild it - it has no UVs, no
    // derivatives of a model surface and no tangent basis, which is the whole reason this pass exists.
    vec3 n = normalize(vine_view_normal);
#ifdef VINE_TEXTURED
    VineSurface surface = vineSampleMaterial(n, vine_view_pos, vine_texcoord);
#else
    VineSurface surface = vineSurfaceFromFactors(n);
#endif
    // WHAT IS NOT RECORDED (graphics-material.md N4): a BLENDED surface has no single value to store - its
    // colour is a mix with whatever is behind it - and baking one would shade it as if it were opaque. So the
    // geometry pass records opaque and masked content only; a host that wants blended content on this path puts
    // it in a forward overlay pass, which is what the Deferred preset does. A masked fragment that survives the
    // threshold is fully opaque, exactly as in the forward stage.
    const uint alpha_mode = (material.flags.x >> VINE_MATERIAL_ALPHA_MODE_SHIFT) & 3u;
    if (alpha_mode == 2u)
    {
        discard;
    }
    if (alpha_mode == 1u && draw.params.x * surface.alpha < material.params.w)
    {
        discard;
    }
    // Most of this design's work happens in ONE line: the surface is stored, not shaded. The albedo is the
    // material's BASE colour (not its diffuse lobe): the lighting pass derives the diffuse part and the
    // reflectance from it and the metallic it reads beside the normal, so f0 is never stored - a derived value
    // in an attachment is how two paths come to disagree about it (vine_gbuffer.glsl).
    out_base_color = vec4(surface.base_color, 1.0);
    // rg = the shading normal (octahedral), b = roughness, a = metallic: one row of the surface the deferred
    // pass rebuilds, which is the design's point that the G-buffer IS the material's projection.
    out_normal = vec4(vineOctahedralEncode(surface.normal), surface.roughness, surface.metallic);
    // Emission is HDR and the occlusion factor is a per-texel scalar: the emissive attachment is a half-float
    // one for that reason (graphics-material.md N3), and its alpha is the occlusion the lighting pass scales
    // the ambient term by.
    out_emissive = vec4(surface.emissive, surface.occlusion);
    // w = 1 marks "this pixel was written", and the deferred lighting program's background test reads it
    // (a cleared pixel keeps the transparent black it was cleared with, w = 0) - the two programs are the
    // two halves of one contract, so this channel is not spare.
    out_position = vec4(vine_view_pos, 1.0);
}
