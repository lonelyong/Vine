#version 450
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;
layout(location = 0) in vec3 vsg_Vertex;
layout(location = 1) in vec3 vsg_Normal;
layout(location = 0) out vec3 v_view_pos;
layout(location = 1) out vec3 v_view_normal;
void main()
{
    vec4 view_pos = pc.modelView * vec4(vsg_Vertex, 1.0);
    v_view_pos = view_pos.xyz;
    v_view_normal = mat3(pc.modelView) * vsg_Normal;
    gl_Position = pc.projection * view_pos;
}
