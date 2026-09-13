#pragma once

#include <vsg/core/ref_ptr.h>
#include <vsg/utils/ShaderSet.h>

#include <vine/graphics/BuiltinShaders.hpp>

#include <vine/vsg/VsgPipelineFactory.hpp>

/**
 * @brief The content shader set the ENGINE builds, for tests that need "a set".
 *
 * A bridge renders its geometry through a ShaderSet, so a test that hands one over is stating which
 * ABI that geometry is bound through: the attribute names, their shader locations, the material
 * block, the per-view lights and the per-drawable values. Only one ABI exists now — the engine's own
 * (ShaderAbi.hpp / BuiltinShaders.hpp) — and every set this backend builds declares exactly it (see
 * VsgPipelineFactory.hpp). vsg's own sets (`vsg::createPhongShaderSet()`) are NOT an alternative: they
 * declare their own names, their own light source and their own material type, and a bridge that is
 * given one now reports that it declares none of the engine's attribute names and draws nothing. That
 * pairing used to work by spelling translation, which is what made it wrong to rely on.
 *
 * @return A set built from the engine's forward program for a 640x360 colour+depth target — the
 *         shape the window's presenting slot has, which is what the tests that only need "a set"
 *         are standing in for.
 */
inline ::vsg::ref_ptr<::vsg::ShaderSet> testContentSet()
{
    // The program is held rather than re-created: forwardProgram() returns a NEW program object on
    // every call, and the backend's stage cache is keyed by program IDENTITY — so a program per test
    // would have glslang recompile the same GLSL once per test. The SET is rebuilt per call: it is a
    // cheap interface declaration, and no test shares one.
    static const vine::intrusive_ptr<const vine::graphics::ShaderProgram> program = vine::graphics::forwardProgram();
    return vine::vsg::detail::makeContentShaderSet(program, VkExtent2D{ 640, 360 }, true, true, 1);
}
