#version 450
// The backend sets these per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests it
// stays dead. Keep the list in sync with the backend's variants - scripts/vine_shader_check.sh compiles
// every combination of these names.
#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)
layout(location = 0) in vec3 vine_view_pos;
layout(location = 1) in vec3 vine_view_normal;
#ifdef VINE_DIFFUSE_MAP
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
// The material the host assigned (the same std140 block the forward content stage declares, so one
// Vine material value feeds both). `diffuse.w` carries the material's own alpha and is NOT read here:
// the engine's transparency is the per-drawable opacity, and it reaches the composite through the
// lighting pass, not through this attachment.
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    float shininess;
} material;
#ifdef VINE_DIFFUSE_MAP
// The material's texture, at the same binding the forward content stage samples: the sampler KIND
// follows the coordinate the vertex stage declared (see its comment), so the two must be kept paired.
#if defined(VINE_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube diffuseMap;
#elif defined(VINE_TEXCOORD_UV)
layout(set = 0, binding = 1) uniform sampler2D diffuseMap;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
#endif
void main()
{
    // The material's colour IS the albedo, modulated by its texture when it has one. The sampling is
    // behind VINE_DIFFUSE_MAP, which the backend sets only for a material with a real texture: an
    // untextured drawable takes the variant with no sampler at all, so a deferred scene of untextured
    // content (the demo's opaque stack) costs exactly what it did before this stage could sample.
    //
    // This is what makes an OPAQUE textured drawable possible on the deferred path at all - and it
    // matters beyond looks: the G-buffer pass writes depth, so opaque content here occludes itself and
    // others correctly, unlike a drawable pushed into the forward overlay pass (whose depth-TEST-only
    // rule cannot self-occlude).
    vec3 albedo = material.diffuse.rgb;
#ifdef VINE_DIFFUSE_MAP
    albedo *= texture(diffuseMap, vine_texcoord).rgb;
#endif
    out_albedo = vec4(albedo, 1.0);
    out_normal = vec4(normalize(vine_view_normal), clamp(material.shininess / 256.0, 0.0, 1.0));
    // The attachment carries the specular COLOUR SCALED BY ITS INTENSITY (the material's alpha), which is
    // the product the lighting program multiplies by - so the deferred highlight honours the SDK's "A is
    // intensity" exactly like the forward program does (wired 2026-09-25, see
    // .ai/design/vsg-reimplementation.md §11.16cj). The alpha channel itself stays 1: nothing reads it.
    out_specular = vec4(clamp(material.specular.rgb, 0.0, 1.0) * clamp(material.specular.a, 0.0, 1.0), 1.0);
    // w = 1 marks "this pixel was written", and the deferred lighting program's background test reads it
    // (a cleared pixel keeps the transparent black it was cleared with, w = 0) - the two programs are the
    // two halves of one contract, so this channel is not spare.
    out_position = vec4(vine_view_pos, 1.0);
}
