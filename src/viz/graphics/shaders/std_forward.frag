#version 450
// The backend toggles these per drawable through vsg's compile settings, and vsg delivers a define
// ONLY when the source asks for it on this line: a name that is missing here is silently dropped (the
// branch that tests it stays dead, with no error from any layer). Keep the list in sync with the
// backend's variants — scripts/vine_shader_check.sh compiles every combination of these names.
#pragma import_defines (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_TEXCOORD_CUBE)
layout(location = 0) in vec3 v_view_pos;
layout(location = 1) in vec3 v_view_normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) in vec4 v_color;
#endif
#ifdef VINE_DIFFUSE_MAP
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 3) in vec3 v_uv;
#else
layout(location = 3) in vec2 v_uv;
#endif
#endif
layout(location = 0) out vec4 out_color;

// The material the host assigned (the same std140 block the deferred path's
// G-buffer stage declares, so one Vine material value feeds both).
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    vec4 emissive;
    float shininess;
    float alphaMask;
    float alphaMaskCutoff;
} material;

#ifdef VINE_DIFFUSE_MAP
// The sampler kind follows the coordinate the vertex stage declared: a cube map is sampled by
// direction, a 2-D map by UV. Both branches bind set 0 / binding 1, and the texture the host
// resolves must be the matching kind (the backend binds a white cube when it is not).
#if defined(VINE_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube diffuseMap;
#else
layout(set = 0, binding = 1) uniform sampler2D diffuseMap;
#endif
#endif

// Per-drawable values (VineDrawBlock): the model matrix and four scalars the host
// rewrites once per drawn command instead of once per vertex. `params.x` is the
// drawable's effective opacity (scene x node x geometry); the remaining components
// are reserved. This is the L1 block itself, declared exactly as the SDK defines it
// (see ShaderAbi.hpp), so a backend without a push range binds the same 80 bytes.
//
// It lives in its OWN set (1) because it is bound PER DRAWABLE with a dynamic offset:
// the scene shares one buffer and one descriptor set, and each draw selects its slot.
layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} draw;

// Per-view lights, packed from the content scene each frame (VineLightsBlock):
// one ambient plus up to three directional lights, all in VIEW space.
layout(set = 0, binding = 2, std140) uniform VineLightsBlock
{
    vec4 ambient;      // rgb + intensity
    vec4 sun_dir[3];   // view-space direction, w unused
    vec4 sun_color[3]; // rgb + intensity
} lights;

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
    vec3 n = normalize(cross(dFdy(v_view_pos), dFdx(v_view_pos)));
#else
    vec3 n = normalize(v_view_normal);
#endif
    vec3 view_dir = normalize(-v_view_pos);
    vec3 albedo = material.diffuse.rgb;
    // The drawable's opacity is a PER-DRAWABLE VALUE, not a per-vertex one: it
    // arrives in the draw block and is rewritten in place when the command's
    // opacity changes, so a translucent drawable costs O(1) per frame instead of
    // a pass over its vertices (and the vertex colour stream stays free to carry
    // authored colours, which its alpha does not compete with).
    float alpha = material.diffuse.a * draw.params.x;
#ifdef VINE_DIFFUSE_MAP
    vec4 texel = texture(diffuseMap, v_uv);
    albedo *= texel.rgb;
    alpha *= texel.a;
#endif
#ifdef VINE_VERTEX_COLOR
    // An AUTHORED vertex colour modulates the albedo only. Its alpha is
    // deliberately NOT an opacity input: opacity belongs to the draw block, so
    // one drawable's alpha cannot leak into another geometry that shares the
    // same vertex stream, and a model's fourth colour component means what the
    // model says it means.
    albedo *= v_color.rgb;
#endif
    vec3 color = albedo * (lights.ambient.rgb * lights.ambient.a);
    float shininess = max(material.shininess, 1.0);
    // The shadow term below is the SAME text the deferred lighting program inserts, and it reads the
    // fragment's view position by one name in both programs: here it arrives as a varying, there it is
    // read out of the G-buffer.
    vec3 pos = v_view_pos;
    for (int i = 0; i < 3; ++i)
    {
        vec3 d = lights.sun_dir[i].xyz;
        if (dot(d, d) < 1e-6) continue;
        vec3 c = lights.sun_color[i].rgb;
        float a = lights.sun_color[i].a;
        vec3 L = normalize(-d);
        float ndl = max(dot(n, L), 0.0);
        // The shadow ABI's term goes here: it scales this light's ndl by whether the map found a
        // caster in front of this fragment. The line below IS that insertion point, and it is the same
        // text the deferred lighting program carries (so the two paths cannot drift).
        // VINE_SHADOW_TERM
        color += albedo * c * a * ndl;
        // Specular is gated by ndl like the diffuse term: a face turned away
        // from the light must not receive a highlight (see deferred_light.frag
        // for the full reasoning).
        if (ndl > 0.0)
        {
            vec3 h = normalize(L + view_dir);
            float spec = pow(max(dot(n, h), 0.0), shininess);
            color += c * a * spec * material.specular.rgb * ndl;
        }
    }
    out_color = vec4(color, alpha);
}
