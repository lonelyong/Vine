#version 450
// The canonical full-screen triangle: every full-screen FRAGMENT stage in the engine is written
// against this interface (see BuiltinShaders::fullscreenVertexProgram), whether it is one of the
// SDK's fragment programs or the host's own post-process.
//
//   * vine_uv (location 0, out here / in there) spans [0, 1] over the destination rectangle, with
//     (0, 0) at the TOP-LEFT of the source image as the read-back API returns it — so a copy
//     samples it directly, with no Y flip;
//   * the triangle is drawn from gl_VertexIndex with no vertex buffer and no push constants:
//     Draw(3, 1, 0, 0) is the whole geometry, and vine_uv is the barycentric-free trick below.
layout(location = 0) out vec2 vine_uv;
void main()
{
    vine_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(vine_uv * 2.0 - 1.0, 0.0, 1.0);
}
