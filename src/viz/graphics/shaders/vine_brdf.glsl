// The engine's ONE specular model: GGX (Trowbridge-Reitz) distribution, height-correlated Smith visibility and
// Schlick Fresnel, over a Lambert diffuse lobe weighted by what the dielectric part of the surface scatters.
//
// WHY A FRAGMENT AND NOT TWO COPIES. The forward content stage and the deferred lighting stage differ in where
// a fragment's INPUTS come from (varyings versus G-buffer texels), never in what the answer IS: the design
// compares their pictures pixel by pixel (graphics-material.md T3), and a second copy of this arithmetic is
// exactly how they would drift apart with no layer noticing. So the function lives here, both stages call it,
// and a material shades the same on either path by construction.
//
// WHAT A CALLER OWNS. This returns the BRDF's VALUE for one light direction - not radiance and not the cosine.
// The caller multiplies by the light's radiance, by n.l and by the light's attenuation, because those three are
// the caller's facts: `n.l` in particular is what the shadow ABI scales before a light's term is summed, so
// folding it in here would make a shadow unable to reach the term.
//
// THE DIFFUSE LOBE CARRIES NO 1/PI, on purpose: the engine's light intensities are authored in its own units
// (a demo sun is 0.4, an ambient fill 0.1), and the Blinn-Phong bridge these paths used to draw with had no
// 1/pi either - so the switch to a real BRDF does not re-expose every existing scene by a factor of pi. The
// specular lobe is the physically-shaped one; only the unit convention of the diffuse constant is stated here.
//
// `roughness` is PERCEPTUAL (the authored 0..1 value, and what the import table derives from a Phong exponent),
// and alpha = roughness^2 is what D and the visibility term take - that squaring is what makes an authored 0.5
// a 0.25-alpha surface, the same mapping glTF's metallic-roughness materials assume.

// The GGX distribution: how much of the microfacet population points along the half vector.
float vnDistributionGGX(float vn_n_dot_h, float vn_alpha)
{
    float vn_a2 = vn_alpha * vn_alpha;
    float vn_d  = vn_n_dot_h * vn_n_dot_h * (vn_a2 - 1.0) + 1.0;
    return vn_a2 / max(3.14159265 * vn_d * vn_d, 1e-7);
}

// Height-correlated Smith visibility, already divided by the 4 * (n.l) * (n.v) the microfacet integral needs.
// The correlated form is what keeps a grazing light from shading a black band at the silhouette.
float vnVisibilitySmith(float vn_n_dot_l, float vn_n_dot_v, float vn_alpha)
{
    float vn_a2   = vn_alpha * vn_alpha;
    float vn_l    = vn_n_dot_v * sqrt(vn_n_dot_l * vn_n_dot_l * (1.0 - vn_a2) + vn_a2);
    float vn_v    = vn_n_dot_l * sqrt(vn_n_dot_v * vn_n_dot_v * (1.0 - vn_a2) + vn_a2);
    return 0.5 / max(vn_l + vn_v, 1e-7);
}

// Schlick's approximation: the reflectance at normal incidence (f0) bends toward 1 at grazing angles.
vec3 vnFresnelSchlick(vec3 vn_f0, float vn_v_dot_h)
{
    return vn_f0 + (1.0 - vn_f0) * pow(clamp(1.0 - vn_v_dot_h, 0.0, 1.0), 5.0);
}

// The BRDF's value for a surface of these factors seen from `v` along a light `l` (all three directions of ONE
// space - this engine shades in view space, so they are view-space vectors, normalized).
//
// @param vn_base_color Linear albedo: the material's base colour with its slot and the vertex colour applied.
// @param vn_metallic   0 = dielectric, 1 = metal; the dielectric part keeps its diffuse lobe, the metal does not.
// @param vn_roughness  Perceptual roughness, 0..1; see the note above about the alpha = roughness^2 mapping.
// @param vn_n          Shading normal, normalized.
// @param vn_v          Unit direction from the surface TOWARD the eye.
// @param vn_l          Unit direction from the surface toward the light.
// @return The BRDF's value: (diffuse + specular). Multiply by radiance, n.l and attenuation at the call site.
vec3 vnEvaluateBRDF(vec3 vn_base_color, float vn_metallic, float vn_roughness, vec3 vn_n, vec3 vn_v, vec3 vn_l)
{
    vec3  vn_h       = normalize(vn_l + vn_v);
    float vn_n_dot_l = max(dot(vn_n, vn_l), 0.0);
    // A view direction on or behind the surface's own plane has no defined reflection; the clamp keeps the
    // visibility term finite instead of producing the NaN a sqrt of a negative would.
    float vn_n_dot_v = max(dot(vn_n, vn_v), 1e-4);
    float vn_n_dot_h = max(dot(vn_n, vn_h), 0.0);
    float vn_v_dot_h = max(dot(vn_v, vn_h), 0.0);
    // A floor, not a guard: roughness 0 would make alpha 0, and the distribution's 0/0 limit is the delta a
    // real surface never has (a perfectly smooth mirror needs a reflection probe, which is out of scope).
    float vn_alpha = max(vn_roughness * vn_roughness, 1e-3);

    vec3 vn_f0 = mix(vec3(0.04), vn_base_color, clamp(vn_metallic, 0.0, 1.0));
    vec3 vn_f  = vnFresnelSchlick(vn_f0, vn_v_dot_h);

    vec3 vn_specular = vn_f * (vnDistributionGGX(vn_n_dot_h, vn_alpha) * vnVisibilitySmith(vn_n_dot_l, vn_n_dot_v, vn_alpha));
    // What the dielectric part scatters, minus the light it reflects: a metal scatters nothing (1 - metallic),
    // and the (1 - f) keeps a bright Fresnel edge from adding diffuse energy that was already reflected.
    vec3 vn_diffuse = vn_base_color * (1.0 - clamp(vn_metallic, 0.0, 1.0)) * (1.0 - vn_f);

    return vn_diffuse + vn_specular;
}
