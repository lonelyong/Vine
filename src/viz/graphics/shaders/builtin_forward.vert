#version 450
// The backend toggles these per drawable through vsg's compile settings, and vsg delivers a define
// ONLY when the source asks for it on this line: a name that is missing here is silently dropped (the
// branch that tests it stays dead, with no error from any layer). Keep the list in sync with the
// backend's variants — scripts/vine_shader_check.sh compiles every combination of these names.
#pragma import_defines (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE, VINE_INSTANCED)
// The per-drawable block, read here for `params.y` (the point size). The vertex stage used to see only the push
// constants; the block's set is already visible to this stage, so the size costs one read, not a new binding.
#define VINE_PARAM_DRAW
#include "vine_params.glsl"
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} pc;
layout(location = 0) in vec3 vine_Vertex;
layout(location = 1) in vec3 vine_Normal;
#ifdef VINE_INSTANCED
// One instance's model matrix, applied in the drawable's local space: `pc.modelView` already carries the
// scene node's world, and this is what places the instance relative to it.
layout(location = 12) in mat4 vine_InstanceModel;
#endif
#ifdef VINE_VERTEX_COLOR
layout(location = 2) in vec4 vine_Color;
#endif
#ifdef VINE_DIFFUSE_MAP
// The slot's KIND is NAMED, never defaulted: the backend sets exactly ONE of the two names for every
// drawable, from the width of the geometry's texcoord channel (three scalars is a direction, two is a
// UV pair - see SceneBridgePipeline), so a variant that samples the slot without saying what the slot
// IS cannot be built at all. The fragment stage below declares the matching sampler.
#if defined(VINE_TEXCOORD_CUBE)
// A cube map is sampled BY DIRECTION, so the same slot carries three scalars (xyz) instead of a UV
// pair (see VsgPipelineFactory / SceneBridgePipeline).
layout(location = 8) in vec3 vine_TexCoord0;
#elif defined(VINE_TEXCOORD_UV)
layout(location = 8) in vec2 vine_TexCoord0;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
#endif
layout(location = 0) out vec3 vine_view_pos;
layout(location = 1) out vec3 vine_view_normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) out vec4 vine_color;
#endif
#ifdef VINE_DIFFUSE_MAP
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 3) out vec3 vine_uv;
#elif defined(VINE_TEXCOORD_UV)
layout(location = 3) out vec2 vine_uv;
#else
#error a sampled texcoord slot needs one kind: define VINE_TEXCOORD_UV or VINE_TEXCOORD_CUBE
#endif
#endif
void main()
{
    // View space on purpose: the lights arrive in view space (VineLightsBlock),
    // so the fragment stage never needs the world matrix and the model matrix
    // stays the only per-drawable data vsg has to push. An instanced draw multiplies
    // the instance's local matrix on the right, which is exactly the drawn node's placement
    // in the drawable's frame.
    mat4 model = pc.modelView;
#ifdef VINE_INSTANCED
    model = model * vine_InstanceModel;
#endif
    vec4 view_pos = model * vec4(vine_Vertex, 1.0);
    vine_view_pos = view_pos.xyz;
    vine_view_normal = mat3(model) * vine_Normal;
#ifdef VINE_VERTEX_COLOR
    vine_color = vine_Color;
#endif
#ifdef VINE_DIFFUSE_MAP
    vine_uv = vine_TexCoord0;
#endif
    // Reverse-Z: the projection matrix maps near to 1 and far to 0, which is
    // what this backend's depth compare (GREATER) expects. See the depth
    // convention note in SceneBridgePipeline.cpp.
    gl_Position = pc.projection * view_pos;
    // A POINT_LIST pipeline needs a vertex stage that STATICALLY writes gl_PointSize: without
    // maintenance5 (and with no tessellation / geometry stage) a point draw from a stage that never
    // writes it is invalid, at pipeline creation and at every draw (VUID-...-topology-08773 and
    // VUID-...-primitiveTopology-10748). 1.0 is Vulkan's own default, so any geometry of this engine
    // may be drawn as points and nothing changes for triangles.
    //
    // THE SIZE COMES FROM THE DRAW BLOCK (`params.y`): a StateNode's setPointSize, or the pass'
    // point-size override, already resolved and floored at 1.0 by resolveRenderState. It is data, not
    // state, so changing it never recompiles this pipeline; triangles and lines ignore the value, and a
    // device wider than one pixel needs the largePoints feature (which the backend requires).
    gl_PointSize = draw.params.y;
}
