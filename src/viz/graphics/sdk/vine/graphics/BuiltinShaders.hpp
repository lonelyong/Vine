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
 * @brief The built-in fullscreen deferred-lighting program.
 *
 * @return The fragment program (a backend supplies its own fullscreen vertex stage).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> deferredLightProgram();

V_GRAPHICS_NS_END
