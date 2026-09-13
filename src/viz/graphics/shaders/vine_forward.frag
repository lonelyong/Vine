#version 450
layout(location = 0) in vec3 v_view_pos;
layout(location = 1) in vec3 v_view_normal;
#ifdef VINE_VERTEX_COLOR
layout(location = 2) in vec4 v_color;
#endif
#ifdef VINE_DIFFUSE_MAP
layout(location = 3) in vec2 v_uv;
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
layout(set = 0, binding = 1) uniform sampler2D diffuseMap;
#endif

// Per-view lights, packed from the content scene each frame (VineLightsBlock):
// one ambient plus up to three directional lights, all in VIEW space.
layout(set = 0, binding = 2, std140) uniform VineLightsBlock
{
    vec4 ambient;      // rgb + intensity
    vec4 sun_dir[3];   // view-space direction, w unused
    vec4 sun_color[3]; // rgb + intensity
} lights;

void main()
{
    vec3 n = normalize(v_view_normal);
    vec3 view_dir = normalize(-v_view_pos);
    vec3 albedo = material.diffuse.rgb;
    float alpha = material.diffuse.a;
#ifdef VINE_DIFFUSE_MAP
    vec4 texel = texture(diffuseMap, v_uv);
    albedo *= texel.rgb;
    alpha *= texel.a;
#endif
#ifdef VINE_VERTEX_COLOR
    // Vertex colour MODULATES the albedo, and its alpha is the per-drawable opacity:
    // SceneBridge keeps the carrier's alpha in step with the drawable's opacity,
    // exactly as the built-in path does. A fully opaque drawable does not bind this
    // attribute at all (the set drops it), which is why both uses are gated.
    albedo *= v_color.rgb;
    alpha *= v_color.a;
#endif
    vec3 color = albedo * (lights.ambient.rgb * lights.ambient.a);
    float shininess = max(material.shininess, 1.0);
    for (int i = 0; i < 3; ++i)
    {
        vec3 d = lights.sun_dir[i].xyz;
        if (dot(d, d) < 1e-6) continue;
        vec3 c = lights.sun_color[i].rgb;
        float a = lights.sun_color[i].a;
        vec3 L = normalize(-d);
        float ndl = max(dot(n, L), 0.0);
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
