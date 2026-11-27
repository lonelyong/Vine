#version 450
// The backend sets this per drawable by compiling one variant per define set, and a define is delivered ONLY when the
// source asks for it on this line: a name missing here is silently dropped and the branch that tests
// it stays dead. Keep the list in sync with the backend's variants - scripts/vn_shader_check.sh
// compiles every combination of these names.
#pragma import_defines (VN_TEXCOORD_UV, VN_TEXCOORD_CUBE)
// A BACKGROUND IS NOT A SURFACE. Draw a direction box the camera sits at the centre of and every ray leaves through
// exactly one face, so the authored direction channel (box-centre -> vertex, in the frame the map is authored in) is
// the direction the fragment is sampled with; the cube sampler normalises it. Nothing here is lit - the map IS the
// sky, which is what separates this stage from builtin_forward.
//
// THE DEPTH CONTRACT LIVES HERE, in four statements (the render state that goes with them is skyboxRenderState):
//  1. the camera's TRANSLATION is removed from view * model, so the box is centred on the eye wherever the eye is:
//     its radius never has to enclose the camera, and a camera that flies anywhere still stands inside it;
//  2. the projection then sees a box of the authored size, SCALED by the window's own extent - a no-op for a
//     perspective projection (its coverage is angular) and exactly what an orthographic one needs (its rays are
//     parallel, so the box has to cover the window laterally instead of subtending it);
//  3. clip z is the FAR PLANE (kFarClipZ, see ShaderAbi.hpp) and not the box's distance, so the sky cannot occlude
//     anything and neither the camera's far plane nor the content has to agree with the box's size;
//  4. the NEAR shell is culled (CullMode::Front). It lies behind the eye, and once both shells are at the far plane
//     the depth test cannot tell them apart - an orthographic view has no w to clip the near shell away with, so
//     culling is what keeps the picture from being sampled through the hemisphere facing away from the eye.
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } vn_pc;
layout(location = 0) in vec3 vn_Vertex;
// The reserved texcoord slot, in the width the data carries: three scalars is a direction for a cube
// map (VN_TEXCOORD_CUBE), and the pair is everything else - VN_TEXCOORD_UV, and a build with NO
// kind at all, which is a real state for this program: a program's stages are compiled once with no defines, before any
// drawable states its variant, and a sky has no map gate to hide that compile behind (a sky always has its map). Both are
// "not a direction", which is why the pair branch covers them (see skyboxProgram's contract).
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
    // (1) Centre the box on the eye: the translation column of view * model is where the world origin sits in view
    // space, so removing it leaves the geometry's own rotation and scale - the box follows any camera for free.
    vec4 view_position = vn_pc.modelView * vec4(vn_Vertex, 1.0);
    view_position.xyz -= vn_pc.modelView[3].xyz;
    // (2) Cover the window: 1 / projection[0][0] and 1 / projection[1][1] are the window's half extents in view
    // units - tan of half the field of view for a perspective projection (under 1 at any sane angle, so `max` leaves
    // that picture alone), and exactly the half width / height for an orthographic one.
    const float window_extent = max(1.0 / abs(vn_pc.projection[0][0]), 1.0 / abs(vn_pc.projection[1][1]));
    vec4        clip          = vn_pc.projection * vec4(view_position.xyz * max(1.0, window_extent), 1.0);
    // (3) The far plane with the fragment's own w (see kFarClipZ): the divide lands on the depth an empty pixel holds.
    gl_Position = vec4(clip.xy, 0.0, clip.w);
}
