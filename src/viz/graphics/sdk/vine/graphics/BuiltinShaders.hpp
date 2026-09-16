#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>

#include "ShaderProgram.hpp"

V_GRAPHICS_NS_BEGIN

/**
 * @brief The engine's built-in shading programs.
 *
 * The SDK owns the shading TEXT; a render backend owns how it is turned into a
 * pipeline (compilation, the binding ABI, the pipeline states). Every program
 * here is assembled from the GLSL in src/viz/graphics/shaders/, embedded at
 * build time (see cmake/VineShaders.cmake) — so editing that file, and nothing
 * else, is how the built-in shading changes.
 *
 * A PROGRAM IS THE ONLY WAY TO SELECT SHADING. There is no shading-model enum:
 * "which shading" is answered by which program a drawable is shaded with — an
 * explicit choice (RenderCommand::program / Node::setProgram, or the engine's
 * default content program), never an implicit lookup. Two built-in programs
 * exist because they differ in the TEXT (one define), not because the engine
 * has two "modes".
 *
 * A backend materialises whatever program it is handed; the SDK never compiles.
 * A program is returned fresh per call, like the other SDK factories; the caller
 * caches whatever it compiles, keyed by the program's revision.
 *
 * A program is NAMED AFTER THE FILE IT IS BUILT FROM: `builtin_forward.vert` /
 * `builtin_forward.frag` make the program `builtin_forward`, and a file that
 * serves more than one program adds the difference as a suffix
 * (`builtin_forward_flat`, `builtin_deferred_lighting_shadowed`). A diagnostic
 * that names a program therefore names the GLSL to go and read, and the name
 * rule is the same one the files follow (see cmake/VineShaders.cmake).
 */

/**
 * @brief The built-in scene-shading program: lit Phong, per-material diffuse/specular.
 *
 * Reads the engine's ABI: positions/normals (+ optional colour and texture), the material value,
 * the per-view lights and the per-drawable block (see ShaderAbi.hpp).
 *
 * @return The vertex + fragment program (fresh per call).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> forwardProgram();

/**
 * @brief The built-in scene-shading program with the per-FACE normal instead of the vertex normal.
 *
 * The SAME stages and the SAME lighting as forwardProgram() — ambient, the directional lights and the
 * specular term are all still evaluated — with one difference: the fragment stage derives the normal
 * from the screen-space derivatives of the view position (`cross(dFdy, dFdx)`), so every fragment of a
 * triangle shades with one normal and the surface reads faceted. The vertex normal channel is then not
 * read at all (geometry with no authored normals shades the same as geometry with them).
 *
 * NOT unlit: turning the lights off still darkens it, and two faces at different angles still shade
 * differently — flat says WHERE the normal comes from, not whether lighting happens.
 *
 * @return The vertex + fragment program (fresh per call).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> flatForwardProgram();

/**
 * @brief The built-in G-buffer geometry program (scene -> MRT) of the Deferred preset.
 *
 * @return The vertex + fragment program.
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> gbufferGeometryProgram();

/**
 * @brief The built-in SKY BOX program: an UNLIT cube-map lookup by direction.
 *
 * Draw a large box the camera stands inside, with a texcoord channel that carries a direction per
 * vertex (see Geometry::setTexcoords3), and the material's texture as the map: every ray then leaves
 * through exactly one face and samples the sky in the direction it is heading. Nothing is lit - a sky
 * is not a surface the scene's sun lights, it is the thing the sun is in - so this program declares no
 * material or lights block and the map IS the colour.
 *
 * The sampler KIND follows the texcoord channel's width, exactly like the engine's own content stages
 * (three scalars: a cube map; two: a 2-D map), which is what keeps a material of the wrong kind from
 * becoming an unbindable descriptor. The returned program therefore names BOTH kinds in its import
 * pragma (VINE_TEXCOORD_UV / VINE_TEXCOORD_CUBE) and samples the pair when neither is set — which is the
 * shape its PROGRAM-LEVEL compile gets (a stage set is built once with no defines, see
 * VsgPipelineFactory) and the shape a mis-widthed channel takes. A backend that hands the sampler kind to
 * the texcoord width (which is what the vsg backend's rule does) keeps it honest by setting exactly one
 * of the two, and one that sets neither still gets a program whose sampler matches the coordinates its
 * data carries.
 *
 * @return The vertex + fragment program (fresh per call).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> skyboxProgram();

/**
 * @brief The built-in fullscreen-triangle VERTEX program every fullscreen FRAGMENT stage is written against.
 *
 * This is the interface a fullscreen fragment stage compiles against, whether it is one of the SDK's
 * (deferredLightProgram / screenCopyProgram) or the host's own:
 *
 *   * the fragment stage declares `layout(location = 0) in vec2 vine_uv;`, spanning [0, 1] over the
 *     destination rectangle with (0, 0) at the TOP-LEFT of the source image AS THE READ-BACK API
 *     RETURNS IT — a copy samples it directly, with no Y flip;
 *   * it declares its own outputs (`layout(location = 0) out vec4 out_color;`);
 *   * source textures arrive at binding 0..N-1 (see ScreenPass::setProgram);
 *   * there is no vertex buffer and no vertex-side push constant: the triangle is three vertices
 *     generated from gl_VertexIndex.
 *
 * The engine owns this text (a full-screen triangle is what every fullscreen picture in the engine
 * has been sampled through since long before it was written down here). A backend may use its own
 * vertex stage instead, but it has to provide THIS interface — the fragment stages cannot tell.
 *
 * @return The vertex program (fresh per call; a backend composes it with a fragment stage).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> fullscreenVertexProgram();

/**
 * @brief The built-in plain screen copy: one colour attachment of the source, sampled 1:1.
 *
 * The text behind what a bare ScreenPass used to get implicitly — now a program the host NAMES, so a
 * copy that wants different filtering, a colour transform or a tonemap is a different program rather
 * than a backend setting (see ScreenPass::setProgram).
 *
 * The BINDING is the attachment: @p attachment is written into the fragment stage as its sampler's
 * binding, which is how the fullscreen program ABI says "the source's attachment N". Use 0 for the
 * common copy of a target's first colour attachment.
 *
 * @param attachment Colour attachment of the source to sample (binding index in the generated text).
 * @return The fragment program (fresh per call; see fullscreenVertexProgram for the vertex stage).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> screenCopyProgram(int attachment = 0);

/**
 * @brief The built-in fullscreen deferred-lighting program.
 *
 * Reads a G-buffer's colour attachments by binding (albedo / normal+shininess / specular / view
 * position) and the pass camera's lights; see fullscreenVertexProgram for the fragment-stage
 * interface it shares with every other fullscreen program. It declares neither a shadow map nor a
 * shadow block, so the pass that draws it must provide neither — use shadowedDeferredLightProgram()
 * for the variant that consumes one.
 *
 * @return The fragment program (see fullscreenVertexProgram for the vertex stage).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> deferredLightProgram();

/**
 * @brief The deferred lighting program that also shades a shadow map.
 *
 * The same program with the shadow ABI added: it declares the map at binding 5 and its
 * `VineShadowBlock` at binding 6 (a pipeline that asks for it must declare that map as an input of the
 * lighting pass), and scales each light's diffuse term by the map. A NAMED variant rather than a
 * boolean argument, because the two programs have different interfaces: whoever calls this one owes the
 * map (see Light::castShadow for the pipeline rule that decides between them).
 *
 * @return The fragment program, or null when the source no longer carries the markers the shadowed
 *         variant is built from.
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> shadowedDeferredLightProgram();

V_GRAPHICS_NS_END
