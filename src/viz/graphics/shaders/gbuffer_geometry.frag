#version 450
layout(location = 0) in vec3 v_view_pos;
layout(location = 1) in vec3 v_view_normal;
layout(location = 0) out vec4 out_albedo;
layout(location = 1) out vec4 out_normal;
layout(location = 2) out vec4 out_specular;
layout(location = 3) out vec4 out_position;
layout(set = 0, binding = 0, std140) uniform MaterialBlock
{
    vec4 ambient;
    vec4 diffuse;
    vec4 specular;
    vec4 emissive;
    float shininess;
    float alphaMask;
    float alphaMaskCutoff;
} material;
void main()
{
    out_albedo = vec4(material.diffuse.rgb, 1.0);
    out_normal = vec4(normalize(v_view_normal), clamp(material.shininess / 256.0, 0.0, 1.0));
    out_specular = vec4(clamp(material.specular.rgb, 0.0, 1.0), 1.0);
    out_position = vec4(v_view_pos, 1.0);
}
