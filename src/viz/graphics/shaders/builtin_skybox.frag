#version 450
// The backend sets this per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests
// it stays dead. Keep the list in sync with the backend's variants - scripts/vine_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VINE_TEXCOORD_UV, VINE_TEXCOORD_CUBE)
layout(location = 0) in vec3 vine_dir;
layout(location = 0) out vec4 out_color;
// The material's texture, at the binding every content program samples it from (set 0 / binding 1,
// the ABI's `diffuseMap` slot). The KIND follows the coordinate the vertex stage declared, exactly
// like the engine's own content stages, so the backend's kind check can keep a mismatched material
// from becoming an unbindable descriptor. The pair branch is VINE_TEXCOORD_UV and the no-kind build
// the vertex stage explains (a sky whose channel is not a direction).
#if defined(VINE_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube skyMap;
#else
layout(set = 0, binding = 1) uniform sampler2D skyMap;
#endif
void main()
{
    // Unlit, and that is the point: a sky is not a surface the scene's sun lights, it is the thing the
    // sun is in. The cube sampler normalises the interpolated direction itself.
#if defined(VINE_TEXCOORD_CUBE)
    out_color = vec4(texture(skyMap, vine_dir).rgb, 1.0);
#else
    // The two-scalar branch samples the pair as a UV rather than a direction: it exists so that a
    // mis-widthed channel is a valid draw with a wrong picture instead of a shader that does not
    // compile (see skyboxProgram's contract).
    out_color = vec4(texture(skyMap, vine_dir.xy).rgb, 1.0);
#endif
}
