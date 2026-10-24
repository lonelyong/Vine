// The G-buffer's packing contract: what each attachment of the canonical deferred target carries, and the
// normal encoding its writer and its reader share.
//
// THE FOUR ATTACHMENTS (graphics-material.md 2.8, ChainPresets::defaultGbufferTarget):
//
//   0  base colour : rgb = the material's linear albedo x its base slot x the vertex colour, a = 1
//   1  normal      : rg  = the view-space SHADING normal, octahedral-encoded (this file)
//                    b  = perceptual roughness, a = metallic
//   2  emissive    : rgb = linear emission (HDR - hence an RGBA16F attachment), a = occlusion
//   3  position    : xyz = view position, w = 1 for every fragment the geometry pass wrote
//
// WHY THE FACTORS RIDE THE NORMAL ATTACHMENT AND NOT A FIFTH ONE. Metallic and roughness are per-texel scalars
// with the normal's own lifetime (they come from the same material), and the octahedral encoding frees exactly
// two channels: four attachments then carry a full metallic-roughness surface, and the deferred lighting stage
// can rebuild the same inputs the forward stage hands its BRDF. The reflectance f0 is NOT stored: it is
// `mix(vec3(0.04), base, metallic)` (ShaderAbi / vine_brdf.glsl), and storing a derived value is how the two
// paths would come to disagree about it.
//
// WHY OCTAHEDRAL. A view-space normal is three numbers where the surface has two degrees of freedom. The
// octahedral map packs the unit sphere into [-1, 1]^2 with one fold, decodes in a handful of instructions and is
// continuous everywhere except the single fold - which is what keeps a half-float attachment (about 10 bits of
// mantissa) from banding the picture the way the naive two-channel projection does near the equator.
//
// The encoding is stored RAW, in [-1, 1], not remapped to [0, 1]: the attachment is signed float, so the
// remap would only spend bits. A cleared attachment decodes to (0, 0, 1), a fact nothing depends on - the
// write mask in attachment 3 is what says "this pixel was drawn".

/**
 * @brief Encodes a unit view-space normal into the two channels attachment 1's rg carries.
 *
 * @param n Shading normal (not required to be normalized; the projection normalizes by the L1 norm).
 * @return The octahedral coordinates, each in [-1, 1].
 */
vec2 vineOctahedralEncode(vec3 n)
{
    // Project onto the L1 sphere: every direction lands on the octahedron |x| + |y| + |z| = 1.
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    vec2 sign_xy = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    // The lower hemisphere folds up onto the upper one, keeping the sign that makes the fold reversible.
    return n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * sign_xy;
}

/**
 * @brief Decodes the two channels of attachment 1's rg back into a unit view-space normal.
 *
 * @param encoded The octahedral coordinates, each in [-1, 1].
 * @return The unit normal the writer encoded.
 */
vec3 vineOctahedralDecode(vec2 encoded)
{
    vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (n.z < 0.0)
    {
        vec2 sign_xy = vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * sign_xy;
    }
    return normalize(n);
}
