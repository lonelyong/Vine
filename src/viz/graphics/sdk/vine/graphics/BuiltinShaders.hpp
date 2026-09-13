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
 * @brief The built-in fullscreen-triangle VERTEX program every fullscreen FRAGMENT stage is written against.
 *
 * This is the interface a fullscreen fragment stage compiles against, whether it is one of the SDK's
 * (deferredLightProgram / screenCopyProgram) or the host's own:
 *
 *   * the fragment stage declares `layout(location = 0) in vec2 v_uv;`, spanning [0, 1] over the
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
 * interface it shares with every other fullscreen program.
 *
 * @param with_shadow True for the variant that also shades a shadow map: it declares the map at
 *                    binding 5 and its `VineShadowBlock` at binding 6 (the shadow ABI; a pipeline
 *                    that asks for it must declare that map as an input of the lighting pass), and
 *                    scales each light's diffuse term by the map. False is the plain program, which
 *                    declares neither — so the pass that draws it must provide neither.
 * @return The fragment program (see fullscreenVertexProgram for the vertex stage), or null when the
 *         source no longer carries the markers the shadowed variant is built from.
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> deferredLightProgram(bool with_shadow);

V_GRAPHICS_NS_END
