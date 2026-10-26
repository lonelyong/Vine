// The engine's parameter blocks, declared ONCE (see .ai/design/graphics-shader-params.md).
//
// The GLSL half of the shader ABI; ShaderAbi.hpp is the C++ half (and LightBlock.hpp is the lights block's,
// while the vsg backend is its only consumer). A shader that declares one of these blocks itself is a SECOND
// owner, and two owners drift silently: the picture keeps drawing, with half of it reading bytes nobody
// wrote. The build inlines this file into every shader that includes it (cmake/VineEmbedShaders.cmake), so a
// program text is one self-contained source and nothing is resolved at run time.
//
// A shader NAMES the blocks it declares - the text IS the program's descriptor interface, so a block nobody
// reads would still be a binding somebody has to fill:
//
//     #define VN_PARAM_MATERIAL
//     #define VN_PARAM_DRAW
//     #define VN_PARAM_LIGHTS
//     #include "vine_params.glsl"
//
// The set/binding numbers are the CONTENT set's arrangement (.ai/design/graphics-material.md §3.3): the
// material at 0, its five texture slots at 1-5, the lights at 6, the shadow pair at 7/8. The samplers stay the
// declaring program's own - their names and kinds are the texture set's policy (api/ContentImages), which is
// not a fact about a block.
//
// NOT HERE. `VineShadowBlock` names a set/binding that belongs to the PROGRAM that declares it (a full-screen
// program's source already occupies the low bindings of its set), so its one declaration is
// BuiltinShaders::shadowBindings(), which writes the numbers the declaring program needs. `VineViewBlock`
// has no shipped GLSL declaration at all: the content path realizes the view as the 128-byte push range
// (ContentPush.hpp), and a program that wants the block declares it where its own arrangement puts it.

#ifdef VN_PARAM_MATERIAL
// The material the host assigned (Material -> VineMaterialBlock): the four 16-byte rows of graphics-material.md
// §3.4. `base_color` is the linear albedo and its alpha is the MATERIAL's own alpha; the effective alpha is
// that times the drawable's opacity (vn_draw.params.x), because a material is shared by every drawable that uses
// it - and `flags`'s bits 8-9 say how the alpha is meant to be used (0 opaque, 1 mask, 2 blend).
//
// The slot bits are what lets ONE variant serve every material (graphics-material.md 2.5): the program declares
// all five samplers (see vine_material.glsl) and a slot the flags leave clear is never read, so the shader
// itself decides what an absent slot contributes instead of depending on the stand-in image's value.
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4  base_color;   // rgb = linear albedo, a = material alpha
    vec4  emissive;     // rgb = linear emission, a = strength
    vec4  params;       // x metallic, y roughness, z occlusion, w alpha cutoff
    uvec4 flags;        // bit0-4 = texture slots, bit8-9 = alpha mode (0 opaque, 1 mask, 2 blend)
} vn_material;

// The slot-exists bits of `flags.x`, in `TextureSlot` order. ShaderAbi.hpp is their C++ spelling
// (kMaterialFlagBaseColor .. kMaterialFlagEmissive, kMaterialAlphaModeShift); the numbers are repeated here
// because GLSL cannot ask C++, and a test pins the two lists against each other (EmbeddedShadersTest).
const uint VN_MATERIAL_FLAG_BASE_COLOR         = 1u << 0;
const uint VN_MATERIAL_FLAG_METALLIC_ROUGHNESS = 1u << 1;
const uint VN_MATERIAL_FLAG_NORMAL             = 1u << 2;
const uint VN_MATERIAL_FLAG_OCCLUSION          = 1u << 3;
const uint VN_MATERIAL_FLAG_EMISSIVE           = 1u << 4;
const uint VN_MATERIAL_ALPHA_MODE_SHIFT        = 8u;
#endif

#ifdef VN_PARAM_DRAW
// The per-drawable values: the model matrix and four scalars the host rewrites once per drawn command
// instead of once per vertex. `params.x` is the drawable's effective opacity (scene x node x geometry);
// `params.y` is the point size in framebuffer pixels (the vertex stages write it to gl_PointSize; 1.0 when
// nobody authored one); z and w are reserved.
//
// It is its OWN set (1) because it is bound PER DRAWABLE with a dynamic offset: the scene shares one buffer
// and one descriptor set, and each draw selects its slot.
layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} vn_draw;
#endif

#ifdef VN_PARAM_LIGHTS
// Per-view lights, packed from the content scene each frame (LightBlock.hpp): one ambient plus EIGHT typed
// lights, positions AND directions already in VIEW space (so no world matrix is needed here).
//
// `position_kind[i].w` is the kind (LightType: 1 = directional, 2 = point, 3 = spot; -1 = an unused slot),
// `direction_range[i].w` is the range (<= 0 = no cutoff), `cone[i]` holds cos(inner) / cos(outer) for a spot.
// The arrays are PARALLEL rather than an array of structs because the ABI scanner sizes built-in member types
// only (see ProgramAbi) - a struct array would be a declaration no layer could measure.
//
// The binding is the same on BOTH drawing paths (the full-screen deferred program puts its G-buffer samplers at
// 0..3 and its depth at 4, then this block at 6), which is why the number is written here and not
// parameterised: one binding, one declaration.
layout(set = 0, binding = 6, std140) uniform VineLightsBlock
{
    vec4 ambient;               // rgb + intensity
    vec4 position_kind[8];      // xyz = view-space position, w = kind
    vec4 direction_range[8];    // xyz = view-space direction, w = range
    vec4 color_intensity[8];    // rgb + intensity
    vec4 cone[8];               // x = cos(inner), y = cos(outer)
} vn_lights;

// Evaluates one light at a view-space fragment position: puts the unit direction TO the light in `to_light` and
// returns its contribution factor (1 for a directional light, inverse-square with the range window for a point,
// times the cone's smoothstep for a spot). A slot that is unused or cannot be evaluated returns 0, which is what
// the shading loops skip on.
//
// ONE function rather than a loop per program: the two programs that light (forward and deferred) differ in
// where their surface data comes from, not in what a light IS - and a second copy of the attenuation/cone
// arithmetic is exactly the drift this file exists to prevent.
float vnEvaluateLight(int vn_index, vec3 vn_pos, out vec3 vn_to_light)
{
    vn_to_light = vec3(0.0);
    float vn_kind = vn_lights.position_kind[vn_index].w;
    if (vn_kind < -0.5)
    {
        return 0.0;   // an unused slot
    }
    if (vn_kind < 1.5)
    {
        // Directional: a direction only, no distance.
        vec3 vn_d = vn_lights.direction_range[vn_index].xyz;
        if (dot(vn_d, vn_d) < 1e-6)
        {
            return 0.0;
        }
        vn_to_light = normalize(-vn_d);
        return 1.0;
    }
    // Point and spot: the vector from the fragment to the light, in view space.
    vec3 vn_to_position = vn_lights.position_kind[vn_index].xyz - vn_pos;
    float vn_distance = length(vn_to_position);
    if (vn_distance < 1e-6)
    {
        return 0.0;
    }
    vn_to_light = vn_to_position / vn_distance;
    float vn_attenuation = 1.0 / (vn_distance * vn_distance + 1.0);
    float vn_range = vn_lights.direction_range[vn_index].w;
    if (vn_range > 0.0)
    {
        // Windowed inverse square: EXACTLY zero at the range, so a light cannot leak past the distance the host
        // bounded it by (the classic (1 - (d/r)^4)^2 window on top of 1/(d^2 + 1)).
        float vn_window = clamp(1.0 - pow(vn_distance / vn_range, 4.0), 0.0, 1.0);
        vn_attenuation *= vn_window * vn_window;
    }
    if (vn_kind > 2.5)
    {
        // Spot: the cone is tested on the angle between the light's axis and the direction to the fragment.
        vec3 vn_raw_axis = vn_lights.direction_range[vn_index].xyz;
        if (dot(vn_raw_axis, vn_raw_axis) < 1e-6)
        {
            return 0.0;
        }
        float vn_cosine = dot(-vn_to_light, normalize(vn_raw_axis));
        float vn_inner = vn_lights.cone[vn_index].x;
        float vn_outer = vn_lights.cone[vn_index].y;
        // A zero-width soft edge (inner == outer) is a hard cut; smoothstep would divide by zero there.
        vn_attenuation *= vn_inner > vn_outer ? smoothstep(vn_outer, vn_inner, vn_cosine) : step(vn_inner, vn_cosine);
    }
    return vn_attenuation;
}
#endif
