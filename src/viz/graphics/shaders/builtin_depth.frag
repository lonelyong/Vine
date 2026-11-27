#version 450
// THE FRAGMENT STAGE OF A DEPTH-ONLY PASS: it declares no colour output, and it CUTS THE SAME HOLES the colour pass
// cuts.
//
// WHY IT DECLARES NOTHING. A pipeline that rasterises needs a fragment stage (rasterizerDiscardEnable is the only way
// to go without one, and it also stops depth writes), while a shadow map has no colour attachment for that stage to
// write. A stage that still writes location 0 there is a validation warning that says exactly this: the write is
// unused and the text is wrong about what the pass does. Declaring nothing is the truthful version of "this pass
// writes depth".
//
// WHY IT READS THE MATERIAL ANYWAY. A depth map holds a DISTANCE per texel, so it can only describe a surface whose
// coverage is all-or-nothing. A MASK material's coverage is not: its alpha cuts a silhouette out of a quad, and a depth
// pass that ignored it would record the whole quad - the leaf's shadow would be a rectangle on the ground while the
// picture showed a leaf (review 18 Y7). So this stage asks the same rule the colour pass asks (`vine_alpha.glsl`),
// which makes the silhouette one fact instead of two.
//
// WHAT IT DOES NOT DO. It does not shade, and it does not project a surface: `vine_alpha.glsl` is included INSTEAD of
// `vine_material.glsl`, so no normal, roughness, occlusion or tangent basis is declared or derived here. The modes
// that cannot reach it are the pass' own statement: a BLENDED drawable is not recorded by a depth pass at all
// (PassSelection says a map records what OCCLUDES), and OPAQUE content settles the coverage before the fetch - an
// opaque caster therefore costs no texture read per texel, and only a Mask caster pays one.
//
// THE VERTEX STAGE IS THE CONTENT'S OWN (see BuiltinShaders::depthOnlyProgram): same push constants, same instancing,
// same per-drawable position expression and the same variant defines as the colour passes, so a triangle's depth here
// and its colour pass land in the same place by construction rather than by a second copy of the transform - and the
// texcoord this stage reads is the one that vertex stage interpolated for the same material.
#pragma import_defines (VN_TEXTURED, VN_TEXCOORD_UV, VN_TEXCOORD_CUBE)
#ifdef VN_TEXTURED
#if defined(VN_TEXCOORD_CUBE)
layout(location = 3) in vec3 vn_uv;
#elif defined(VN_TEXCOORD_UV)
layout(location = 3) in vec2 vn_uv;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
#endif
// The two blocks the coverage rule reads, from their one declaration (vine_params.glsl): the material (its mode, its
// cutoff, its base factor) and the per-drawable values (the drawable's opacity, which is the threshold's other input).
#define VN_PARAM_MATERIAL
#define VN_PARAM_DRAW
#include "vine_params.glsl"
// The coverage itself - the base slot, the three modes and the threshold - shared with the forward and the G-buffer
// stages (see the file note).
#include "vine_alpha.glsl"
void main()
{
    // Discards the fragment when the material's alpha mode cuts it away; the returned alpha has no attachment to go
    // to here, which is the whole point of a depth-only pass.
#ifdef VN_TEXTURED
    vnSurfaceCoverage(vn_draw.params.x, vn_uv);
#else
    vnAlphaCoverage(vnAlphaFactor(), vn_draw.params.x);
#endif
}
