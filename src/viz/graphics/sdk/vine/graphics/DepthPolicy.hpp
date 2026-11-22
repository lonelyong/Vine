#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Depth handling of a pass' content relative to the target's depth.
 *
 * @details
 * - Declared by the pass (RenderPass::depthPolicy) and honoured by the backend for every command that
 *   does not author a depth state of its own (DrawItem::authored_state.depth): TestAndWrite for opaque
 *   scene content, TestOnly for translucent content that tests against existing depth without writing,
 *   Disabled for HUD content drawn on top.
 * - Independent of clearing (RenderPass::setClearEnabled) and of lighting: whether the content is lit
 *   comes from the lights of the scene it renders.
 * - A POLICY, not a state: it is what the pass chooses for content that authored no depth state, which is why this
 *   type is separate from `DepthState` (a draw's own test/write/compare value, see RenderState.hpp).
 * - ADDING AN ENUMERATOR IS A BACKEND QUESTION, NOT ONLY AN SDK ONE: what a value means to a pipeline
 *   ("test depth", "write depth") and which baked shader set it selects are both derived on the BACKEND
 *   side, in switches that name every value and have NO default arm — so a new value makes the compiler
 *   point at every place that must be extended instead of falling through to a "no depth at all" arm.
 */
enum class DepthPolicy
{
    Disabled,     // No depth test / write (drawn on top - HUD overlays).
    TestOnly,     // Depth test on, depth write off (translucent content).
    TestAndWrite, // Depth test + write on (opaque scene content).
};

VN_GRAPHICS_NS_END
