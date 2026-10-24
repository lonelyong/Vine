#version 450
// The backend toggles these per drawable through vsg's compile settings, and vsg delivers a define
// ONLY when the source asks for it on this line: a name that is missing here is silently dropped (the
// branch that tests it stays dead, with no error from any layer). Keep the list in sync with the
// backend's variants — scripts/vine_shader_check.sh compiles every combination of these names.
#pragma import_defines (VINE_VERTEX_COLOR, VINE_TEXTURED, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)
layout(location = 0) in vec3 vine_view_pos;
layout(location = 1) in vec3 vine_view_normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) in vec4 vine_color;
#endif
#ifdef VINE_TEXTURED
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 3) in vec3 vine_uv;
#elif defined(VINE_TEXCOORD_UV)
layout(location = 3) in vec2 vine_uv;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
#endif
layout(location = 0) out vec4 out_color;

// The parameter blocks this stage reads, from their ONE declaration (vine_params.glsl): the material, the
// per-drawable block and the view's lights, at the bindings of the content set. What a block MEANS is
// stated next to its bytes there; this file states only what the surface does with them.
#define VINE_PARAM_MATERIAL
#define VINE_PARAM_DRAW
#define VINE_PARAM_LIGHTS
#include "vine_params.glsl"

#ifdef VINE_TEXTURED
// The five slots of the texture set, at the content set's fixed bindings (graphics-material.md §3.3): the base
// colour may be a cube map (its kind follows the vertex stage's texcoord width), the other four are 2-D data
// maps. A slot the material does not have is bound to its type's constant fallback, and the block's flags
// short-circuit it at run time - the variant is binary, so all five are declared together.
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

// The shadow this pass declared, if any: the map it named as an INPUT and the block that places a
// fragment in it (ShaderAbi.hpp). DECLARED ALWAYS, unlike the deferred lighting program's two
// variants: a content set is shared per (target, depth mode) and a session picks the content program
// once, so a shadowed variant would double that cache for every target — and a host's own content
// program would still have no shadowed twin to pick. One text, both paths, switched at RUNTIME by
// `shadow.params.x` (0 = no shadow reached this pass), which is what the ABI's switch is for.
// The line below IS the insertion point (BuiltinShaders::forwardProgram).
// VINE_SHADOW_BINDINGS

void main()
{
#ifdef VINE_FLAT
    // Flat shading: the FACE normal, from the screen-space derivatives of the view position.
    // One normal for the whole triangle — which is what "flat" means — and no per-vertex
    // normals at all, so this preset reuses the same vertex stage as the smooth one (see
    // builtinProgram).
    //
    // The cross order is the one that faces the VIEWER: Vulkan's framebuffer rows grow downward,
    // so dFdy points the other way from the y-up convention these derivatives are usually written
    // in, and dFdx x dFdy comes out pointing away from the camera (measured: the lit side of a
    // surface facing the camera stayed at its ambient term until the operands were swapped).
    vec3 n = normalize(cross(dFdy(vine_view_pos), dFdx(vine_view_pos)));
#else
    vec3 n = normalize(vine_view_normal);
#endif
    vec3 view_dir = normalize(-vine_view_pos);
    vec3 albedo = material.base_color.rgb;
    // The drawable's opacity is a PER-DRAWABLE VALUE, not a per-vertex one: it
    // arrives in the draw block and is rewritten in place when the command's
    // opacity changes, so a translucent drawable costs O(1) per frame instead of
    // a pass over its vertices (and the vertex colour stream stays free to carry
    // authored colours, which its alpha does not compete with).
    //
    // It is also the engine's ONLY transparency input, which is why neither the
    // material's alpha nor the texture's is multiplied in here: a material is
    // shared by every drawable that uses it ("a translucent material" would make
    // all of them translucent, and the per-drawable value it would fight with is
    // the one the engine sorts by), and the deferred path discards them anyway
    // (its lighting program writes alpha 1) - so reading them here would make the
    // same asset translucent in forward and opaque in deferred.
    float alpha = draw.params.x;
#ifdef VINE_TEXTURED
    // M2 wires the BASE slot into the picture. metallic-roughness / normal / occlusion / emissive are
    // declared and bound (the variant is binary, not per slot) and become inputs of the real BRDF in M3.
    albedo *= texture(baseColorMap, vine_uv).rgb;
#endif
#ifdef VINE_VERTEX_COLOR
    // An AUTHORED vertex colour modulates the albedo only. Its alpha is
    // deliberately NOT an opacity input: opacity belongs to the draw block, so
    // one drawable's alpha cannot leak into another geometry that shares the
    // same vertex stream, and a model's fourth colour component means what the
    // model says it means.
    albedo *= vine_color.rgb;
#endif
#ifdef VINE_UNLIT
    // Unlit: the surface colour IS the answer - no light term, no specular, and the drawable's opacity
    // unchanged. It is the mode that separates "is this the shape?" from "is this the shading?" (a viewer
    // that has to answer "why is it flat?" wants to be able to switch the lighting OFF first), and the one
    // a data-driven preview wants: the authored colour, with nothing invented on top of it.
    out_color = vec4(albedo, alpha);
    return;
#endif
#ifdef VINE_NORMALS
    // View-space normals as colour, the debugging view every 3D tool carries: it answers "which way does
    // this surface face" with no light involved at all (blue = facing the camera). Alpha is 1: the picture
    // is a readout, not content, so nothing translucent about the content should show through it.
    out_color = vec4(normalize(n) * 0.5 + 0.5, 1.0);
    return;
#endif
    // The M1 bridge from the PBR factors to the Phong terms this stage still draws with (see
    // vine_params.glsl): the diffuse lobe is what a dielectric reflects, the highlight tint is what a metal
    // reflects, and both are driven by the SAME base colour the host authored. It leaves with M3's BRDF.
    float metallic  = clamp(material.params.x, 0.0, 1.0);
    vec3  diffuse   = albedo * (1.0 - metallic);
    vec3  f0        = vineMaterialF0(albedo, metallic);
    float shininess = vineMaterialShininess(material.params.y);
    vec3 color = diffuse * (lights.ambient.rgb * lights.ambient.a);
    // The shadow term below is the SAME text the deferred lighting program inserts, and it reads the
    // fragment's view position by one name in both programs: here it arrives as a varying, there it is
    // read out of the G-buffer.
    vec3 pos = vine_view_pos;
    for (int i = 0; i < 8; ++i)
    {
        // What a light IS (kind, position, range, cone) is the block's business and is evaluated in one place
        // (vineEvaluateLight, vine_params.glsl); what is left here is this surface's term.
        vec3  L;
        float attenuation = vineEvaluateLight(i, pos, L);
        if (attenuation <= 0.0)
        {
            continue;   // an unused slot, or a light that cannot reach this fragment
        }
        vec3 c = lights.color_intensity[i].rgb;
        float a = lights.color_intensity[i].a;
        float ndl = max(dot(n, L), 0.0);
        // The shadow ABI's term goes here: it scales this light's ndl by whether the map found a
        // caster in front of this fragment. The line below IS that insertion point, and it is the same
        // text the deferred lighting program carries (so the two paths cannot drift).
        // VINE_SHADOW_TERM
        color += diffuse * c * a * ndl * attenuation;
        // Specular is gated by ndl like the diffuse term: a face turned away
        // from the light must not receive a highlight (see builtin_deferred_lighting.frag
        // for the full reasoning).
        if (ndl > 0.0)
        {
            vec3 h = normalize(L + view_dir);
            float spec = pow(max(dot(n, h), 0.0), shininess);
            // The reflectance the metallic-roughness pair implies (vineMaterialF0): the same value the
            // G-buffer writer stores, so the forward and the deferred picture agree by construction.
            color += c * a * spec * f0 * ndl * attenuation;
        }
    }
    out_color = vec4(color, alpha);
}
