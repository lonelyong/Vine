#pragma once
#include "graphics_global.hpp"

V_GRAPHICS_NS_BEGIN

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
 */
enum class DepthMode
{
    Disabled,     ///< No depth test / write (drawn on top - HUD overlays).
    TestOnly,     ///< Depth test on, depth write off (translucent content).
    TestAndWrite, ///< Depth test + write on (opaque scene content).
};

V_GRAPHICS_NS_END
