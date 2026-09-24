#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Depth handling of a pass' content relative to the target's depth.
 *
 * Declared by the pass (RenderPass::depthMode) and honoured by the backend for
 * every command that does not author a depth state of its own
 * (RenderCommand::depthExplicit): TestAndWrite for opaque scene content,
 * TestOnly for translucent content that tests against existing depth without
 * writing, Disabled for HUD content drawn on top.
 *
 * Independent of clearing (RenderPass::clearEnabled) and of lighting: whether
 * the content is lit comes from the lights of the scene it renders.
 *
 * ADDING AN ENUMERATOR IS A BACKEND QUESTION, NOT ONLY AN SDK ONE: what a mode means to a pipeline
 * ("test depth", "write depth") and which baked shader set it selects are derived in the vsg
 * backend (detail::depthTestWrite / detail::shaderSetFor), each a switch with NO default arm — so a
 * new mode makes the compiler point at those two places instead of falling through to the "no depth
 * at all" arm in one of them.
 */
enum class DepthMode
{
    Disabled,     ///< No depth test / write (drawn on top - HUD overlays).
    TestOnly,     ///< Depth test on, depth write off (translucent content).
    TestAndWrite, ///< Depth test + write on (opaque scene content).
};

VN_GRAPHICS_NS_END
