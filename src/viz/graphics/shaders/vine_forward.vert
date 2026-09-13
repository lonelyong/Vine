#version 450
// The backend toggles these per drawable through vsg's compile settings, and vsg delivers a define
// ONLY when the source asks for it on this line: a name that is missing here is silently dropped (the
// branch that tests it stays dead, with no error from any layer). Keep the list in sync with the
// backend's variants — scripts/vine_shader_check.sh compiles every combination of these names.
#pragma import_defines (VINE_VERTEX_COLOR, VINE_DIFFUSE_MAP, VINE_TEXCOORD_CUBE)
layout(push_constant) uniform PushConstants
{
    mat4 projection;
    mat4 modelView;
} pc;
layout(location = 0) in vec3 vsg_Vertex;
layout(location = 1) in vec3 vsg_Normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) in vec4 vsg_Color;
#endif
#ifdef VINE_DIFFUSE_MAP
#if defined(VINE_TEXCOORD_CUBE)
// A cube map is sampled BY DIRECTION, so the same slot carries three scalars (xyz) instead of a UV
// pair. The kind is a per-drawable compile variant: the backend sets VINE_TEXCOORD_CUBE when the
// geometry's texcoord channel is an xyz one, and the fragment stage below declares the matching
// samplerCube (see VsgPipelineFactory / SceneBridgePipeline).
layout(location = 8) in vec3 vsg_TexCoord0;
#else
layout(location = 8) in vec2 vsg_TexCoord0;
#endif
#endif
layout(location = 0) out vec3 v_view_pos;
layout(location = 1) out vec3 v_view_normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) out vec4 v_color;
#endif
#ifdef VINE_DIFFUSE_MAP
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 3) out vec3 v_uv;
#else
layout(location = 3) out vec2 v_uv;
#endif
#endif
void main()
{
    // View space on purpose: the lights arrive in view space (VineLightsBlock),
    // so the fragment stage never needs the world matrix and the model matrix
    // stays the only per-drawable data vsg has to push.
    vec4 view_pos = pc.modelView * vec4(vsg_Vertex, 1.0);
    v_view_pos = view_pos.xyz;
    v_view_normal = mat3(pc.modelView) * vsg_Normal;
#ifdef VINE_VERTEX_COLOR
    v_color = vsg_Color;
#endif
#ifdef VINE_DIFFUSE_MAP
    v_uv = vsg_TexCoord0;
#endif
    // Reverse-Z: the projection matrix maps near to 1 and far to 0, which is
    // what this backend's depth compare (GREATER) expects. See the depth
    // convention note in SceneBridgePipeline.cpp.
    gl_Position = pc.projection * view_pos;
}
