#pragma once

#include <vine/appfw/Plugin.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>

VN_APPFW_NS_BEGIN
class PluginLoadContext;
VN_APPFW_NS_END

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief appfw plugin exposing the direct-Vulkan backend ("vk").
 *
 * @details
 * - On `load()` it registers the "vk" factory into `RenderBackendRegistry`, so a host can create the backend by name
   *   without a compile-time dependency on this plugin (`.ai/design/vk-backend.md` 2.1).
 * - `unload()` tears nothing down: the registry does not own the factory and the module stays mapped for the process
   *   lifetime, which is `Plugin`'s contract.
 */
class VN_BACKEND_VK_API Plugin final : public vn::appfw::Plugin {
    VN_OBJECT_META_DECL;

  public:
    Plugin();
    ~Plugin() override;

  public:
    /**
     * @brief Registers the backend factory.
     *
     * @param context Load context (unused: registering a factory is a pointer store).
     * @return A task that completes when the factory is registered.
     */
    vn::async::Task<void> load(vn::appfw::PluginLoadContext* context) override;

    /**
     * @brief Does nothing (see the class note).
     *
     * @param context Load context.
     */
    void unload(vn::appfw::PluginLoadContext* context) override;
};

VN_BACKEND_VK_NS_END
