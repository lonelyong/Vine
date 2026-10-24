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
//     #define VINE_PARAM_MATERIAL
//     #define VINE_PARAM_DRAW
//     #define VINE_PARAM_LIGHTS
//     #include "vine_params.glsl"
//
// The set/binding numbers are the CONTENT set's arrangement (.ai/design/graphics-architecture.md §9): the
// material at 0, the optional diffuse map at 1, the lights at 2. The samplers stay the declaring program's
// own - a sampler's KIND follows the geometry's texcoord width (see builtin_forward.vert), which is not a
// fact about a block.
//
// NOT HERE. `VineShadowBlock` names a set/binding that belongs to the PROGRAM that declares it (a full-screen
// program's source already occupies the low bindings of its set), so its one declaration is
// BuiltinShaders::shadowBindings(), which writes the numbers the declaring program needs. `VineViewBlock`
// has no shipped GLSL declaration at all: the content path realizes the view as the 128-byte push range
// (ContentPush.hpp), and a program that wants the block declares it where its own arrangement puts it.

#ifdef VINE_PARAM_MATERIAL
// The material the host assigned (Material -> VineMaterialBlock). `diffuse.w` is the material's own alpha: it
// is TRANSPORTED for a host program that has its own transparency model, and no engine program reads it - the
// engine's one transparency channel is the per-drawable opacity in the draw block, because a material is
// shared by every drawable that uses it.
layout(set = 0, binding = 0, std140) uniform VineMaterialBlock
{
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    float shininess;
} material;
#endif

#ifdef VINE_PARAM_DRAW
// The per-drawable values: the model matrix and four scalars the host rewrites once per drawn command
// instead of once per vertex. `params.x` is the drawable's effective opacity (scene x node x geometry);
// y, z and w are reserved.
//
// It is its OWN set (1) because it is bound PER DRAWABLE with a dynamic offset: the scene shares one buffer
// and one descriptor set, and each draw selects its slot.
layout(set = 1, binding = 0, std140) uniform VineDrawBlock
{
    mat4 model;
    vec4 params;
} draw;
#endif

#ifdef VINE_PARAM_LIGHTS
// Per-view lights, packed from the content scene each frame (LightBlock.hpp): one ambient plus up to three
// directional lights, all in VIEW space.
layout(set = 0, binding = 2, std140) uniform VineLightsBlock
{
    vec4 ambient;      // rgb + intensity
    vec4 sun_dir[3];   // view-space direction, w unused
    vec4 sun_color[3]; // rgb + intensity
} lights;
#endif
