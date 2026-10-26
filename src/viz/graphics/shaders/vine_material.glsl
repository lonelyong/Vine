// How a material's texture set becomes the surface a light can shade: the five slots, their channel packing,
// the constant an ABSENT slot contributes and the tangent basis a normal map needs - sampled in ONE place.
//
// WHY A FRAGMENT. Two stages build the same surface: the forward content stage (which shades it) and the
// G-buffer writer (which stores it, so the deferred lighting stage rebuilds it from texels). If one of them read
// roughness from the metallic-roughness map's G channel and the other from B, or if one treated an absent slot
// as white while the other treated it as its factor, the two paths would draw different pictures of one material
// and nothing in the engine would report it (graphics-material.md R3/T3 exist for exactly this). So the
// samplers, the packing and the fallbacks are stated here, once, and a stage only says which coordinate it has.
//
// THE PACKING IS glTF 2.0's, because the importer copies glTF materials verbatim (graphics-material.md 2.1):
// metallic-roughness is ONE map with roughness in G and metallic in B, occlusion reads R, and a normal map is a
// tangent-space RGB texture in the OpenGL orientation.
//
// AN ABSENT SLOT IS SHORT-CIRCUITED BY THE BLOCK'S FLAGS, never by the value of the stand-in image. The flags'
// bits 0-4 say which slots the material has (ShaderAbi.hpp kMaterialFlagBaseColor .. kMaterialFlagEmissive);
// a slot the material does not have contributes what its FACTOR means on its own - white for the three
// MULTIPLICATIVE slots (base colour, metallic-roughness, occlusion: multiplying by white leaves the factor),
// (0, 0, 1) for the normal slot (no perturbation) and black for emissive. The backend still binds a stand-in of
// the right KIND for an absent slot (content-set resolution, ContentSets), and that is all the stand-in is for:
// a white texel read as a normal map would tilt every surface 45 degrees, and read as emissive it would paint
// the object white.
//
// CUBE-MAPPED MATERIALS ARE DIRECTION-SAMPLED and have no UV pair at all, so only their base colour slot takes
// part (graphics-material.md 2.4: a cube map is legal in the base slot and nowhere else): the four data maps
// parameterise a surface, and there is no surface parameterisation to read them with.

/**
 * @brief What the shading needs to know about a surface, whatever wrote it.
 *
 * The forward stage fills it from the material's factors and slots; the G-buffer writer fills it and stores the
 * parts a deferred pass cannot re-derive; the deferred lighting stage fills the same fields back from texels.
 */
struct VineSurface
{
    vec3  base_color;   ///< Linear albedo: the base factor x its slot x the vertex colour.
    vec3  emissive;     ///< Linear emission: the factor x its strength x its slot.
    vec3  normal;       ///< View-space SHADING normal (tangent-perturbed when the material has a normal map).
    float metallic;     ///< 0 = dielectric, 1 = metal.
    float roughness;    ///< Perceptual roughness, 0..1.
    float occlusion;    ///< The occlusion factor x its slot (1 = unoccluded).
    float alpha;        ///< The material's own alpha: the base factor's a x its slot's a.
};

#ifdef VN_TEXTURED
// The five slots at the content set's fixed bindings (graphics-material.md 3.3): the base colour may be a cube
// map, whose kind follows the vertex stage's texcoord width; the other four are 2-D data maps. The variant is
// BINARY (one VN_TEXTURED bit for the whole set, see 2.5), so all five are declared together even though a
// material rarely has more than two.
//
// THE FIVE NAMES ARE ABI KEYS, not this file's locals: the backend decides what a declared sampler receives BY
// NAME (ContentImages::imageOriginOf keys on these exact spellings), so the prefix is not decoration - it marks
// the drawable's own maps as the engine's vocabulary, and renaming one side alone silently rebinds them to the
// pass' inputs.
#if defined(VN_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube vn_baseColorMap;
#elif defined(VN_TEXCOORD_UV)
layout(set = 0, binding = 1) uniform sampler2D vn_baseColorMap;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
layout(set = 0, binding = 2) uniform sampler2D vn_metallicRoughnessMap;
layout(set = 0, binding = 3) uniform sampler2D vn_normalMap;
layout(set = 0, binding = 4) uniform sampler2D vn_occlusionMap;
layout(set = 0, binding = 5) uniform sampler2D vn_emissiveMap;
#endif

/**
 * @brief Builds the surface a material WITHOUT any texture slot describes: its factors alone.
 *
 * @param vn_n View-space geometric normal (the normal slot cannot perturb it - there is no slot).
 * @return The surface the factors describe.
 */
VineSurface vnSurfaceFromFactors(vec3 vn_n)
{
    VineSurface vn_surface;
    vn_surface.base_color = vn_material.base_color.rgb;
    // The emissive factor is rgb and its STRENGTH is alpha: a material can emit without carrying a map, which
    // is why the factor is applied here and the slot multiplies on top of it (glTF's emissiveFactor rule).
    vn_surface.emissive   = vn_material.emissive.rgb * vn_material.emissive.a;
    vn_surface.normal     = vn_n;
    vn_surface.metallic   = clamp(vn_material.params.x, 0.0, 1.0);
    vn_surface.roughness  = clamp(vn_material.params.y, 0.0, 1.0);
    vn_surface.occlusion  = clamp(vn_material.params.z, 0.0, 1.0);
    // The material's own alpha: the MODE decides what it does (a threshold for Mask, the blend factor for Blend,
    // nothing at all for Opaque - see the stages), and its second input is the drawable's opacity, which arrives
    // per drawable and is not a material fact (graphics-material.md 2.7).
    vn_surface.alpha      = clamp(vn_material.base_color.a, 0.0, 1.0);
    return vn_surface;
}

#ifdef VN_TEXTURED
#ifdef VN_TEXCOORD_UV
/**
 * @brief The tangent basis a tangent-space normal map is expressed in, from screen-space derivatives.
 *
 * This engine has no tangent vertex channel and no second UV set (graphics-material.md 1.3), so the basis is
 * rebuilt per fragment from the view position and the UV pair: the standard cotangent frame. It is an
 * approximation exactly where the UV mapping is degenerate (a mirrored island flips the bitangent's sign, which
 * cannot be recovered from derivatives alone) - stated as a known limit, not a bug (9.2).
 *
 * @param vn_n        View-space geometric normal the frame is orthogonalized against.
 * @param vn_position View-space fragment position (the very value the deferred path stores).
 * @param vn_uv       The UV pair the normal map is sampled with.
 * @return A matrix whose columns are the tangents along +u, along +v and the normal.
 */
mat3 vnCotangentFrame(vec3 vn_n, vec3 vn_position, vec2 vn_uv)
{
    vec3 vn_dp1 = dFdx(vn_position);
    vec3 vn_dp2 = dFdy(vn_position);
    vec2 vn_duv1 = dFdx(vn_uv);
    vec2 vn_duv2 = dFdy(vn_uv);
    // The two tangents are (dP/du) and (dP/dv) recovered from the screen-space derivatives by the same
    // Gram-Schmidt step that makes them orthogonal to n - solving the 2x2 system the derivatives state.
    vec3 vn_dp2perp = cross(vn_dp2, vn_n);
    vec3 vn_dp1perp = cross(vn_n, vn_dp1);
    vec3 vn_t = vn_dp2perp * vn_duv1.x + vn_dp1perp * vn_duv2.x;
    vec3 vn_b = vn_dp2perp * vn_duv1.y + vn_dp1perp * vn_duv2.y;
    // A degenerate mapping (all vertices sharing one UV) leaves both tangents at zero; scaling by 0 gives a
    // basis that drops the perturbation instead of a NaN that poisons the whole picture.
    float vn_scale = inversesqrt(max(dot(vn_t, vn_t), dot(vn_b, vn_b)) + 1e-12);
    return mat3(vn_t * vn_scale, vn_b * vn_scale, vn_n);
}
#endif

#if defined(VN_TEXCOORD_CUBE)
/**
 * @brief Builds the surface of a CUBE-mapped material: the base slot sampled by direction, factors for the rest.
 *
 * @param vn_n         View-space geometric normal.
 * @param vn_position  View-space fragment position (unused; the parameter keeps the two overloads' shapes equal).
 * @param vn_direction Unit direction the base slot is sampled along (the geometry's three-scalar texcoord).
 * @return The surface.
 */
VineSurface vnSampleMaterial(vec3 vn_n, vec3 vn_position, vec3 vn_direction)
{
    VineSurface vn_surface = vnSurfaceFromFactors(vn_n);
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_BASE_COLOR) != 0u)
    {
        // rgb is the albedo and a is the slot's own alpha: BOTH travel, because a cut-out leaf's shape is in its
        // map's alpha (a Mask material's whole point) and a blended surface's coverage is too.
        vec4 vn_texel         = texture(vn_baseColorMap, vn_direction);
        vn_surface.base_color *= vn_texel.rgb;
        vn_surface.alpha      *= vn_texel.a;
    }
    // The four data slots are not sampled: a direction is not a surface parameterisation (see the file note).
    return vn_surface;
}
#else
/**
 * @brief Builds the surface a material's five slots describe.
 *
 * @param vn_n        View-space geometric normal (the frame the normal slot perturbs).
 * @param vn_position View-space fragment position, for the derivative tangent basis.
 * @param vn_uv       The UV pair every slot is sampled with (one UV set, set 0 - graphics-material.md 2.4).
 * @return The surface.
 *
 * @warning The fetches sit in branches on `vn_material.flags`, which is a UNIFORM block value: the branches are
 *          dynamically uniform per drawable, which is what makes implicit-LOD sampling in them well defined. A
 *          per-fragment flag would need explicit LODs here.
 * @note The parameters are unused when the material has no slot, and the branches are the reason the fetches
 *       are not unconditional: a slotless material in the textured variant must not pay four texture reads.
 */
VineSurface vnSampleMaterial(vec3 vn_n, vec3 vn_position, vec2 vn_uv)
{
    VineSurface vn_surface = vnSurfaceFromFactors(vn_n);
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_BASE_COLOR) != 0u)
    {
        vec4 vn_texel          = texture(vn_baseColorMap, vn_uv);
        vn_surface.base_color *= vn_texel.rgb;
        // The slot's own alpha multiplies the material's: a cut-out leaf's shape lives in the map, and a blended
        // surface's coverage does too.
        vn_surface.alpha      *= vn_texel.a;
    }
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_METALLIC_ROUGHNESS) != 0u)
    {
        // One map, two factors: G is roughness and B is metallic, multiplied by the material's own factors -
        // which is what makes an absent map's white stand-in the multiplicative identity (see the file note).
        vec4 vn_texel            = texture(vn_metallicRoughnessMap, vn_uv);
        vn_surface.roughness    *= vn_texel.g;
        vn_surface.metallic     *= vn_texel.b;
    }
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_OCCLUSION) != 0u)
    {
        vn_surface.occlusion *= texture(vn_occlusionMap, vn_uv).r;
    }
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_EMISSIVE) != 0u)
    {
        vn_surface.emissive *= texture(vn_emissiveMap, vn_uv).rgb;
    }
    if ((vn_material.flags.x & VN_MATERIAL_FLAG_NORMAL) != 0u)
    {
        vec3 vn_tangent_normal = texture(vn_normalMap, vn_uv).xyz * 2.0 - 1.0;
        // glTF's normal maps are authored for OpenGL, where +Y points UP the image, while this engine's v grows
        // DOWN it (Vulkan's origin is the top row - the same fact that makes the G-buffer upright). The basis
        // is built from the sampler's own coordinate system, so the green channel is flipped here; without it
        // every normal map is lit as if the surface were mirrored about its u axis.
        vn_tangent_normal.y = -vn_tangent_normal.y;
        vn_surface.normal   = normalize(vnCotangentFrame(vn_n, vn_position, vn_uv) * vn_tangent_normal);
    }
    return vn_surface;
}
#endif
#endif
