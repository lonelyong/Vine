#pragma once

#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The factory that names this backend to the registry as "vk".
 *
 * @details
 * - Registration is a pointer store, so the factory is a static object in its own translation unit and the name is the
 *   only thing a host needs: `RenderBackendRegistry::instance().create(u8"vk")`.
   * - "vk" says the IMPLEMENTATION path (Vulkan directly), not the API family: a scene-graph-based backend would be
   *   Vulkan too.
 * - WHICH ONE RUNS IS THE APPLICATION'S DECISION, never the registry's (it answers by name, because "the first one"
 *   would pick by the alphabet): `app_shell`'s `chooseRenderBackend()` names `"vk"` and honours `VN_RENDER_BACKEND`,
   *   which is how another installed backend is selected in a normal build. What this backend's own acceptance still
   *   wants is in `.ai/backlog/graphics.md` T35 and `.ai/decisions/ADR-2026-11-14-vk-becomes-the-default-target.md`;
   *   this header does not restate that evidence.
 */
class VN_BACKEND_VK_API BackendFactory final : public vn::graphics::RenderBackendFactory {
  public:
    BackendFactory();
    ~BackendFactory() override;

  public:
    /** @brief Gets this backend's static metadata (name "vk", API family Vulkan). */
    [[nodiscard]] vn::graphics::RenderBackendInfo info() const override;

    /**
     * @brief Creates the device (inert until initialize(), see Device).
     *
     * @details
     * - A DEVICE THAT DID NOT STATE A CACHE DIRECTORY GETS @ref defaultCacheDirectory HERE (see
     *   DeviceDesc::cache_directory): the framework already decided where a plugin's own files live, so a host does not
     *   have to know this backend's name to get a warm start - and a host that stated a directory keeps it.
     *
     * @param desc How the device is meant to present, and where it may persist state.
     * @return The new backend; the caller owns the reference.
     */
    vn::intrusive_ptr<vn::graphics::RenderBackend> create(const vn::graphics::DeviceDesc& desc) override;

    /**
     * @brief States the directory a device without one of its own may persist state in.
     *
     * @details
     * - SET BY THE PLUGIN'S LOAD STEP from `appfw::PluginLoadContext::ensureDataDirectory`, which is the framework's
     *   one answer to "where may a plugin keep files" - so no caller has to invent a path.
     * - IT IS A DEFAULT, NOT A POLICY: a `DeviceDesc` that states a directory overrides it, and a factory nobody set
     *   leaves devices keeping nothing (a tool, a bake, a test).
     * - Set it once, before devices are created: it is read when `create` runs, and a device already created keeps what
     *   it was created with.
     *
     * @param directory The directory (empty states "no default").
     */
    void setDefaultCacheDirectory(vn::String directory);

    /** @brief Gets the directory stated by @ref setDefaultCacheDirectory (empty when none was). */
    [[nodiscard]] const vn::String& defaultCacheDirectory() const noexcept;

  private:
    vn::String default_cache_directory_{};
};

VN_BACKEND_VK_NS_END
