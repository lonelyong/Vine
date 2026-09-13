#pragma once
#include "graphics_global.hpp"

#include <vine/intrusive_ptr.hpp>

#include "ShaderPreset.hpp"
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
 * The backend takes a program and materialises it; the SDK never compiles. A
 * program is returned fresh per call, like the other SDK factories; the caller
 * caches whatever it compiles, keyed by the program's content revision.
 */

/**
 * @brief The built-in scene-shading program for @p preset.
 *
 * The preset axis is semantic: this function is the ShaderPreset -> program
 * mapping that replaces a backend-side preset switch, so a backend able to
 * materialise the program needs no knowledge of the shading itself.
 *
 * @param preset Preset to load.
 * @return The program, or null when this preset has no SDK-built shading yet
 *         (the backend then keeps its own fallback mapping).
 */
V_GRAPHICS_API intrusive_ptr<ShaderProgram> builtinProgram(ShaderPreset preset);

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
