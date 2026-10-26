#version 450
// The backend sets this per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests
// it stays dead. Keep the list in sync with the backend's variants - scripts/vn_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VN_TEXCOORD_UV, VN_TEXCOORD_CUBE)
layout(location = 0) in vec3 vn_dir;
layout(location = 0) out vec4 vn_out_color;
// The material's texture, at the binding every content program samples it from (set 0 / binding 1,
// the base-colour texture slot). The KIND follows the coordinate the vertex stage declared, exactly
// like the engine's own content stages, so the backend's kind check can keep a mismatched material
// from becoming an unbindable descriptor. The pair branch is VN_TEXCOORD_UV and the no-kind build
// the vertex stage explains (a sky whose channel is not a direction).
//
// `vn_skyMap` is an ABI key, not a local: the backend routes an image BY NAME
// (ContentImages::imageOriginOf keys on this exact spelling), so the prefix marks the drawable's own texture -
// renaming one side alone silently rebinds the slot to a pass input.
#if defined(VN_TEXCOORD_CUBE)
layout(set = 0, binding = 1) uniform samplerCube vn_skyMap;
#else
layout(set = 0, binding = 1) uniform sampler2D vn_skyMap;
#endif
void main()
{
    // Unlit, and that is the point: a sky is not a surface the scene's sun lights, it is the thing the
    // sun is in. The cube sampler normalises the interpolated direction itself.
#if defined(VN_TEXCOORD_CUBE)
    vn_out_color = vec4(texture(vn_skyMap, vn_dir).rgb, 1.0);
#else
    // The two-scalar branch samples the pair as a UV rather than a direction: it exists so that a
    // mis-widthed channel is a valid draw with a wrong picture instead of a shader that does not
    // compile (see skyboxProgram's contract).
    vn_out_color = vec4(texture(vn_skyMap, vn_dir.xy).rgb, 1.0);
#endif
}
