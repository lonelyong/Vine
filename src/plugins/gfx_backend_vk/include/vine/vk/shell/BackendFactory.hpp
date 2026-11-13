#pragma once

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/vk/vk_global.hpp>

VN_VK_NS_BEGIN

/**
 * @brief The factory that names this backend to the registry as "vk".
 *
 * @details
 * - Registration is a pointer store, so the factory is a static object in its own translation unit and the name is the
 *   only thing a host needs: `RenderBackendRegistry::instance().create(u8"vk")`.
 * - "vk" says the IMPLEMENTATION path (Vulkan directly), not the API family - the "vsg" backend is Vulkan too. "vk" is
 *   not the default either way: `"vsg"` remains the production backend (see
 *   `.ai/research/graphics-2026-11-13-gpu-resource-abstraction.md` §8.5), and nothing selects "vk" implicitly.
 */
class VN_VK_API BackendFactory final : public vn::graphics::RenderBackendFactory {
  public:
    BackendFactory();
    ~BackendFactory() override;

  public:
    /** @brief Gets this backend's static metadata (name "vk", API family Vulkan). */
    [[nodiscard]] vn::graphics::RenderBackendInfo info() const override;

    /**
     * @brief Creates the device (inert until initialize(), see Device).
     *
     * @param desc How the device is meant to present.
     * @return The new backend; the caller owns the reference.
     */
    vn::intrusive_ptr<vn::graphics::RenderBackend> create(const vn::graphics::DeviceDesc& desc) override;
};

VN_VK_NS_END
