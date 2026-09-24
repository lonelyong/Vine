#pragma once
#include "vsg_global.hpp"

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/raw_ptr.hpp>

VN_VSG_NS_BEGIN

/**
 * @brief Factory creating the VSG backend, self-registered into the render backend registry.
 *
 * Lets the application create the VSG backend by name ("vsg") through
 * vn::graphics::RenderBackendRegistry without a compile-time dependency on
 * this module or on VulkanSceneGraph.
 *
 * WHAT THE NAME CREATES: the rewrite's facade (`vn::vsg::VsgBackend`, see
 * .ai/design/vsg-reimplementation.md §11.16bi). The previous implementation
 * (`VsgRenderer`) is still in this module and still covered by its own tests, but
 * no registered name creates it any more: a second name would be a second answer
 * to "which backend is 'vsg'", which is the very thing the rewrite exists to
 * remove.
 */
class VN_VSG_API VsgRenderBackendFactory : public vn::graphics::RenderBackendFactory {
  public:
    VsgRenderBackendFactory();
    ~VsgRenderBackendFactory() override;

  public:
    /** @brief Gets the backend's static metadata ("vsg").
     *
     * @return The VSG backend metadata.
     */
    vn::graphics::RenderBackendInfo info() const override;

    /** @brief Creates the VSG backend (the rewrite's facade).
     *
     * @return New backend; the caller owns it.
     */
    vn::intrusive_ptr<vn::graphics::RenderBackend> create() override;
};

VN_VSG_NS_END
