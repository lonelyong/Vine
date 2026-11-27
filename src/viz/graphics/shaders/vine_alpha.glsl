// WHAT A MATERIAL'S ALPHA MODE DOES TO A FRAGMENT: the base colour slot a coverage can come from, the mode's three
// rules, and the threshold that cuts a fragment away - stated once, for every stage that needs one of them.
//
// WHY A FRAGMENT. THREE stages apply this rule and they have to agree: the forward content stage (which outputs the
// alpha), the G-buffer writer (which refuses a blended surface and cuts a masked one) and the DEPTH stage of a
// depth-only pass (which must leave the SAME holes, or an object's shadow has the shape of its quad instead of its
// silhouette - review 18 Y7). Two copies of a threshold is how a picture and its shadow come to disagree about one
// leaf, and nothing in the engine would report it: the frame draws, the shadow lands, and the leaf is a rectangle on
// the ground.
//
// THE THREE MODES are `AlphaMode`'s ordinals (ShaderAbi.hpp kMaterialAlphaModeShift; the value rides the material
// block's `flags`, bits 8-9):
//
//   OPAQUE - alpha is ignored. The drawable's OPACITY is still the stage's output coverage, because that is how a
//            host fades a whole object, and the sort put such a drawable in the transparent queue on that value alone.
//   MASK   - the product of the opacity and the material's own alpha is a THRESHOLD: below the cutoff the fragment is
//            discarded, above it the fragment is fully opaque and the drawable stays in the opaque queue, so a cut-out
//            leaf still occludes itself (graphics-material.md C4). This is the mode a depth pass reproduces, and the
//            only reason a depth stage may sample a base map at all.
//   BLEND  - the product IS the output alpha. The drawable is in the transparent queue by its mode alone, and a DEPTH
//            pass records nothing for it (PassSelection): one distance cannot describe partial coverage, so a shadow
//            of blended content is a policy the pipeline states, not a texel it writes.
//
// WHAT A STAGE DECLARES. `vn_material` (VN_PARAM_MATERIAL) always; the drawable's OPACITY arrives as an ARGUMENT,
// never as a hidden read of `vn_draw` - a coverage is a function of two numbers a caller has, and the deferred path's
// readers do not all hold a draw block.
//
// `vine_material.glsl` includes this file (a shading stage needs the base slot and the alpha to build a surface); a
// depth stage includes it INSTEAD, which is what keeps a depth pass from declaring a normal, a roughness or a tangent
// basis it has no use for.

#ifdef VN_TEXTURED
// The base colour slot, at the content set's binding 1 (graphics-material.md §3.3): the ONE map a coverage can come
// from. Its kind follows the vertex stage's texcoord width, and the variant is BINARY (one VN_TEXTURED bit for the
// whole set, see §2.5), so the kind must be stated by exactly one of these names - a sampled slot whose kind is
// unstated cannot be built at all.
//
// THE NAME IS AN ABI KEY, not this file's local: the content set decides what a declared sampler receives BY NAME
// (an image's origin is answered from this exact spelling), so renaming one side alone silently rebinds the slot to
// the pass' inputs. The other four slots live in vine_material.glsl for the same reason: the set is one vocabulary,
// split by which stage needs it.
#if defined(VN_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube vn_baseColorMap;
#elif defined(VN_TEXCOORD_UV)
layout(set = 0, binding = 1) uniform sampler2D vn_baseColorMap;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
#endif

/**
 * @brief The material's alpha mode: VN_ALPHA_MODE_OPAQUE, _MASK or _BLEND.
 *
 * @return The two mode bits, already shifted down.
 */
uint vnAlphaMode()
{
    return (vn_material.flags.x >> VN_MATERIAL_ALPHA_MODE_SHIFT) & 3u;
}

/**
 * @brief The material's own alpha, FACTOR half: its base colour's a.
 *
 * @details
 * - The slot's own alpha multiplies this where a stage samples one; a material with no base map states its coverage
 *   in this factor alone, which is why the two halves are separate functions' worth of fact even though the surface
 *   keeps one number.
 *
 * @return The factor, clamped to [0, 1].
 */
float vnAlphaFactor()
{
    return clamp(vn_material.base_color.a, 0.0, 1.0);
}

/**
 * @brief Applies the mode's rule to one fragment's coverage, DISCARDING it when the mode cuts it away.
 *
 * @details
 * - THE ONE THRESHOLD (graphics-material.md §2.7): every stage that can cut a fragment away calls this, so the forward
 *   picture, the G-buffer projection and a depth map cut the same holes from the same two numbers.
 * - It reads the material block only; @p opacity is the draw block's `params.x` as an argument (see the file note).
 *
 * @param surface_alpha The material's own alpha: its factor, times its base map's alpha when the stage sampled one.
 * @param opacity       The drawable's effective opacity (scene x node x geometry), in [0, 1].
 * @return The alpha the stage outputs: the product for Blended surfaces, the drawable's opacity otherwise.
 */
float vnAlphaCoverage(float surface_alpha, float opacity)
{
    const uint vn_mode = vnAlphaMode();
    if (vn_mode == VN_ALPHA_MODE_MASK)
    {
        // Mask: the product is a THRESHOLD, not an opacity. A survivor is opaque - the drawable stays in the opaque
        // queue and a depth pass records it whole - so the stage's output alpha is the drawable's own opacity.
        if (opacity * surface_alpha < vn_material.params.w)
        {
            discard;
        }
        return opacity;
    }
    if (vn_mode == VN_ALPHA_MODE_BLEND)
    {
        // Blend: the product IS the output alpha, and it is also the number the sort ordered this drawable by.
        return opacity * surface_alpha;
    }
    // Opaque: neither alpha takes part; the drawable's opacity is how a host fades the whole object.
    return opacity;
}

#ifdef VN_TEXTURED
#if defined(VN_TEXCOORD_CUBE)
/**
 * @brief The coverage of a material whose base slot is a CUBE map, for a stage that builds no surface.
 *
 * @details
 * - A DEPTH STAGE'S FORM: it fetches the base slot only when the mode actually reads an alpha, so an opaque caster
 *   pays no texture read per shadow-map texel. A cube map is sampled by direction, so the parameter is the geometry's
 *   three-scalar texcoord, exactly as the vertex stage interpolates it.
 *
 * @param opacity     The drawable's effective opacity.
 * @param vn_direction Unit direction the base slot is sampled along.
 * @return The alpha the stage outputs (see vnAlphaCoverage).
 */
float vnSurfaceCoverage(float opacity, vec3 vn_direction)
{
    float vn_alpha = vnAlphaFactor();
    if (vnAlphaMode() != VN_ALPHA_MODE_OPAQUE && (vn_material.flags.x & VN_MATERIAL_FLAG_BASE_COLOR) != 0u)
    {
        vn_alpha *= texture(vn_baseColorMap, vn_direction).a;
    }
    return vnAlphaCoverage(vn_alpha, opacity);
}
#else
/**
 * @brief The coverage of a material whose base slot is a 2-D map, for a stage that builds no surface.
 *
 * @details
 * - A DEPTH STAGE'S FORM: it fetches the base slot only when the mode actually reads an alpha, so an opaque caster
 *   pays no texture read per shadow-map texel.
 *
 * @param opacity The drawable's effective opacity.
 * @param vn_uv   The UV pair the base slot is sampled with (one UV set, set 0 - graphics-material.md §2.4).
 * @return The alpha the stage outputs (see vnAlphaCoverage).
 *
 * @warning The fetch sits in a branch on `vn_material.flags`, a UNIFORM block value: the branch is dynamically uniform
 *          per drawable, which is what makes implicit-LOD sampling in it well defined.
 */
float vnSurfaceCoverage(float opacity, vec2 vn_uv)
{
    float vn_alpha = vnAlphaFactor();
    if (vnAlphaMode() != VN_ALPHA_MODE_OPAQUE && (vn_material.flags.x & VN_MATERIAL_FLAG_BASE_COLOR) != 0u)
    {
        vn_alpha *= texture(vn_baseColorMap, vn_uv).a;
    }
    return vnAlphaCoverage(vn_alpha, opacity);
}
#endif
#endif
