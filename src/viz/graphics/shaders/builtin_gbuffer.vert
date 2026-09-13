#version 450
// The backend sets these per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name that is missing here is silently dropped and the branch that
// tests it stays dead. Keep the list in sync with the backend's variants - scripts/vine_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VINE_DIFFUSE_MAP, VINE_TEXCOORD_CUBE)
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;
layout(location = 0) in vec3 vine_Vertex;
layout(location = 1) in vec3 vine_Normal;
#ifdef VINE_DIFFUSE_MAP
// The same reserved texcoord slot the forward stage reads, in the same two widths: three scalars is a
// direction for a cube map, two is a UV pair for a 2-D map. The backend selects the sampler kind from
// this width, so the two branches below must stay paired with the fragment stage's.
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 8) in vec3 vine_TexCoord0;
#else
layout(location = 8) in vec2 vine_TexCoord0;
#endif
#endif
layout(location = 0) out vec3 v_view_pos;
layout(location = 1) out vec3 v_view_normal;
#ifdef VINE_DIFFUSE_MAP
#if defined(VINE_TEXCOORD_CUBE)
layout(location = 2) out vec3 v_texcoord;
#else
layout(location = 2) out vec2 v_texcoord;
#endif
#endif
void main()
{
    vec4 view_pos = pc.modelView * vec4(vine_Vertex, 1.0);
    v_view_pos = view_pos.xyz;
    v_view_normal = mat3(pc.modelView) * vine_Normal;
#ifdef VINE_DIFFUSE_MAP
    v_texcoord = vine_TexCoord0;
#endif
    gl_Position = pc.projection * view_pos;
}
