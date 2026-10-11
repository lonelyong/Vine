#pragma once
#include "graphics_global.hpp"

#include <optional>
#include <vector>

#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Diagnostics.hpp"
#include "FrameDescription.hpp"
#include "FrameStats.hpp"
#include "RenderDevice.hpp"
#include "RenderFrameExecutor.hpp"
#include "RenderPass.hpp"
#include "WindowPresentTarget.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderTarget;

/**
 * @brief The backend a factory returns: ONE object answering the three faces a renderer is made of.
 *
 * WHY IT STILL EXISTS. The three questions a renderer answers are separate contracts - what a GPU has
 * (RenderDevice), how a frame comes out (RenderFrameExecutor), and how a window is presented
 * (WindowPresentTarget, created by the device) - and they are separate because they change for different reasons
 * and live for different times: a device outlives every window on top of it, and a window outlives no frame it is
 * not named by. Today one object is all three, and this type is that aggregate: what a RenderBackendFactory
 * returns, what the engine is handed (RenderEngine::setDevice), and what a test double implements. Splitting the
 * contracts is what lets a device hand out another window, or run frames for several areas, without any of the
 * three having to be re-spelled - the aggregate keeps compiling for a backend that does all three at once.
 *
 * WHAT A HOST SEES. A host talks to the faces: it asks the device what it can do (RenderDevice::capabilities),
 * asks it for a window (RenderDevice::createSurfaceTarget - the target an area then holds, see
 * RenderSurface::bindTarget), announces what it will draw into (RenderDevice::setRenderTarget), and drives frames
 * through the executor (RenderFrameExecutor::executeFrame) the device hands out (RenderDevice::executor).
 * Everything those faces document applies here: the one-wire frame value, the optional window half, the borrowed
 * arguments of a frame, what may be retained, the threading rules and the failure rules.
 *
 * RenderBackend is reference-counted: factories return an intrusive_ptr and
 * RenderEngine keeps its own reference, so ownership and lifetime are
 * explicit.
 */
class VN_GRAPHICS_API RenderBackend : public Object,
                                      public RefCounted<RenderBackend>,
                                      public RenderDevice,
                                      public RenderFrameExecutor {
    VN_OBJECT_META_DECL;

  public:
    virtual ~RenderBackend() = default;
};

VN_GRAPHICS_NS_END
