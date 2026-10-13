#pragma once
#include "graphics_global.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What a backend can do, as ONE value (see RenderDevice::capabilities).
 *
 * @details
 * - WHY A VALUE. Every "can you do X" question used to be its own bool on the contract, with its own default and its own sentence per backend that does not do it - `supportsRenderTargets()` was the last one left. A capability is a DESCRIPTION of the backend, and descriptions are values: asked once, answered whole, and extended by adding a field (which every answer keeps compiling, because the fields are defaulted).
 * - WHAT BELONGS HERE, AND WHAT DOES NOT. A capability is a property of a backend that does not change while it is initialized, and that a host has to know BEFORE it plans a frame. What does NOT belong here is anything the call itself answers better: whether a readback works is answered by that call's own machine-readable result (see ReadbackResult - `Unsupported` is the documented answer for "this backend cannot do it", and a host gates the feature on it), so a flag here would be a second spelling of a fact that already has one.
 * - It lives in its own header because it is a LEAF of the SDK's vocabulary: the engine answers it (RenderEngine::capabilities) without knowing the backend contract, so this must not drag that in.
 */
struct VN_GRAPHICS_API Capabilities
{
    /// Off-screen render targets are built, drawn into and sampled (see RenderDevice::setRenderTarget); false
    /// means a non-null target is ignored and the pass draws into the default framebuffer.
    bool render_targets{false};

    /// Frames can be presented somewhere: this session has a window to show a picture through (see
    /// RenderDevice::createSurfaceTarget, and RenderSurface::bindTarget for the window an area is bound to).
    ///
    /// False is a first-class state, not an error: a session brought up to draw only into render targets runs
    /// whole frames and presents nothing (an offline bake, a display-less run). It answers "was this session
    /// opened with a presentation intent at all" - which is why it is asked before a host plans a frame, rather
    /// than discovered from a present that did nothing.
    bool presentation{false};
};

VN_GRAPHICS_NS_END
