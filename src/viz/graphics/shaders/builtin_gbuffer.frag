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
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_specular;
layout(location = 3) out vec4 out_position;
// The material this stage writes the G-buffer's albedo and specular from: the ONE declaration of the
// parameter blocks (see vine_params.glsl), so a deferred drawable and a forward one of the same material
// shade the same bytes. `base_color.a` (the material's own alpha) is not read here either - the engine's
// transparency is the per-drawable opacity, and it reaches the composite through the lighting pass, not
// through this attachment.
#define VINE_PARAM_MATERIAL
#include "vine_params.glsl"
#ifdef VINE_TEXTURED
// The same five slots the forward stage declares, at the same bindings: the base colour may be a cube map
// (kind follows the vertex stage's texcoord width), the other four are 2-D data maps. Missing slots bind their
// type's constant fallback; the block's flags short-circuit them at run time.
#if defined(VINE_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube baseColorMap;
#elif defined(VINE_TEXCOORD_UV)
layout(set = 0, binding = 1) uniform sampler2D baseColorMap;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
layout(set = 0, binding = 2) uniform sampler2D metallicRoughnessMap;
layout(set = 0, binding = 3) uniform sampler2D normalMap;
layout(set = 0, binding = 4) uniform sampler2D occlusionMap;
layout(set = 0, binding = 5) uniform sampler2D emissiveMap;
#endif
void main()
{
    // The material's colour IS the albedo, modulated by its base-colour texture when it has one. The sampling
    // is behind VINE_TEXTURED, which the backend sets only for a material carrying any texture slot: an
    // untextured drawable takes the variant with no sampler at all, so a deferred scene of untextured content
    // (the demo's opaque stack) costs exactly what it did before this stage could sample.
    //
    // This is what makes an OPAQUE textured drawable possible on the deferred path at all - and it
    // matters beyond looks: the G-buffer pass writes depth, so opaque content here occludes itself and
    // others correctly, unlike a drawable pushed into the forward overlay pass (whose depth-TEST-only
    // rule cannot self-occlude).
    vec3 albedo = material.base_color.rgb;
#ifdef VINE_TEXTURED
    albedo *= texture(baseColorMap, vine_texcoord).rgb;
#endif
    // The M1 bridge (see vine_params.glsl): the attachment carries what the lighting pass multiplies by the
    // light terms, so it is the DIFFUSE lobe - a metal reflects rather than scatters - while the specular
    // attachment carries the reflectance. Both derive from the same authored base colour.
    float metallic = clamp(material.params.x, 0.0, 1.0);
    out_albedo = vec4(albedo * (1.0 - metallic), 1.0);
    out_normal = vec4(normalize(vine_view_normal), vineMaterialShininess(material.params.y) / 256.0);
    // The attachment carries the reflectance the metallic-roughness pair implies (vineMaterialF0), which is
    // the product the lighting program multiplies by - so the deferred highlight honours the PBR factors
    // exactly like the forward program does. The alpha channel stays 1: nothing reads it yet.
    out_specular = vec4(vineMaterialF0(albedo, metallic), 1.0);
    // w = 1 marks "this pixel was written", and the deferred lighting program's background test reads it
    // (a cleared pixel keeps the transparent black it was cleared with, w = 0) - the two programs are the
    // two halves of one contract, so this channel is not spare.
    out_position = vec4(vine_view_pos, 1.0);
}
