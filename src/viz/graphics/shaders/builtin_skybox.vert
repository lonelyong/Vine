#version 450
// The backend sets this per drawable (vsg compile settings), and vsg delivers a define ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests
// it stays dead. Keep the list in sync with the backend's variants - scripts/vn_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VN_TEXCOORD_UV, VN_TEXCOORD_CUBE)
// A sky box is drawn as a large box the camera stands INSIDE, so every ray leaves through exactly one
// face: the authored direction channel carries the box-centre -> vertex vector, so the interpolated
// value IS this fragment's own vector and the cube sampler normalises it. Nothing here is lit - the
// map IS the sky, which is what separates this stage from builtin_forward.
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } vn_pc;
layout(location = 0) in vec3 vn_Vertex;
// The reserved texcoord slot, in the width the data carries: three scalars is a direction for a cube
// map (VN_TEXCOORD_CUBE), and the pair is everything else - VN_TEXCOORD_UV, and a build with NO
// kind at all, which is a real state for this program: VsgPipelineFactory compiles a program's stages
// once with no defines, before any drawable states its variant, and a sky has no map gate to hide that
// compile behind (a sky always has its map). Both are "not a direction", which is why the pair
// branch covers them (see skyboxProgram's contract).
#if defined(VN_TEXCOORD_CUBE)
layout(location = 8) in vec3 vn_TexCoord0;
#else
layout(location = 8) in vec2 vn_TexCoord0;
#endif
layout(location = 0) out vec3 vn_dir;
void main()
{
    // The two-scalar branch is not a sky: it is what keeps a mis-widthed channel a valid draw instead
    // of a descriptor nobody can bind (see skyboxProgram's contract).
#if defined(VN_TEXCOORD_CUBE)
    vn_dir = vn_TexCoord0;
#else
    vn_dir = vec3(vn_TexCoord0, -1.0);
#endif
    gl_Position = vn_pc.projection * vn_pc.modelView * vec4(vn_Vertex, 1.0);
}
