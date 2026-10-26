#version 450
// The backend sets these per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name that is missing here is silently dropped and the branch that
// tests it stays dead. Keep the list in sync with the backend's variants - scripts/vn_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VN_VERTEX_COLOR, VN_TEXTURED, VN_TEXCOORD_UV, VN_TEXCOORD_CUBE, VN_INSTANCED)
// The same per-drawable block the forward stage reads, read here for the same reason: `params.y` is the point
// size, and a POINT_LIST pipeline needs this stage to write gl_PointSize statically.
#define VN_PARAM_DRAW
#include "vine_params.glsl"
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } vn_pc;
layout(location = 0) in vec3 vn_Vertex;
layout(location = 1) in vec3 vn_Normal;
#ifdef VN_VERTEX_COLOR
// The SAME authored colour the forward stage reads (location 2, a vec4): the G-buffer path carries it to its
// fragment stage, which modulates the albedo it stores with it - one input, one meaning on both paths.
layout(location = 2) in vec4 vn_Color;
#endif
#ifdef VN_INSTANCED
// The same slot and the same convention the forward stage states: one local model matrix per instance.
layout(location = 12) in mat4 vn_InstanceModel;
#endif
#ifdef VN_TEXTURED
// The same reserved texcoord slot the forward stage reads, in the same two widths: three scalars is a
// direction for a cube map, two is a UV pair for a 2-D map. Both kinds are NAMED and neither is a
// default (the backend sets exactly one per drawable), so the branches below stay paired with the
// fragment stage's.
#if defined(VN_TEXCOORD_CUBE)
layout(location = 8) in vec3 vn_TexCoord0;
#elif defined(VN_TEXCOORD_UV)
layout(location = 8) in vec2 vn_TexCoord0;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
#endif
layout(location = 0) out vec3 vn_view_pos;
layout(location = 1) out vec3 vn_view_normal;
#ifdef VN_VERTEX_COLOR
// Location 3, because this stage's texcoord varying owns 2 (the forward stage numbers them the other way
// round; the numbers are internal to each stage pair, so neither has to match the other).
layout(location = 3) out vec4 vn_color;
#endif
#ifdef VN_TEXTURED
#if defined(VN_TEXCOORD_CUBE)
layout(location = 2) out vec3 vn_texcoord;
#elif defined(VN_TEXCOORD_UV)
layout(location = 2) out vec2 vn_texcoord;
#else
#error a sampled texcoord slot needs one kind: define VN_TEXCOORD_UV or VN_TEXCOORD_CUBE
#endif
#endif
void main()
{
    mat4 vn_model = vn_pc.modelView;
#ifdef VN_INSTANCED
    vn_model = vn_model * vn_InstanceModel;
#endif
    vec4 vn_view_position = vn_model * vec4(vn_Vertex, 1.0);
    vn_view_pos = vn_view_position.xyz;
    vn_view_normal = mat3(vn_model) * vn_Normal;
#ifdef VN_VERTEX_COLOR
    vn_color = vn_Color;
#endif
#ifdef VN_TEXTURED
    vn_texcoord = vn_TexCoord0;
#endif
    gl_Position = vn_pc.projection * vn_view_position;
    // The same obligation the forward stage states: a POINT_LIST pipeline needs a stage that statically
    // writes gl_PointSize. The size is the draw block's `params.y` (resolved from a StateNode's
    // setPointSize or the pass' override), so a point cloud keeps its size in the G-buffer pass too.
    gl_PointSize = vn_draw.params.y;
}
