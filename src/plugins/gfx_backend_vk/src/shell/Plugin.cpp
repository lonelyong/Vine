#include <vine/backend_vk/shell/Plugin.hpp>

#include <filesystem>
#include <string_view>

#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/graphics/BackendAbi.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/backend_vk/shell/BackendFactory.hpp>

VN_BACKEND_VK_NS_BEGIN

VN_OBJECT_META_IMPL(vn::backend_vk::Plugin, vn::appfw::Plugin)

Plugin::Plugin()  = default;
Plugin::~Plugin() = default;

vn::async::Task<void> Plugin::load(vn::appfw::PluginLoadContext* context)
{
    static BackendFactory s_factory;

    // THE PIPELINE CACHE IS A PLUGIN-OWNED FILE, and the framework already answers where those live (see
    // PluginLoadContext::ensureDataDirectory): the factory hands that directory to every device that does not state one
    // of its own, which is what turns a cold start into a warm one without a host knowing this backend's name.
    if (context != nullptr) {
        const std::filesystem::path directory = context->ensureDataDirectory();
        if (!directory.empty()) {
            const std::u8string text = directory.u8string();
            s_factory.setDefaultCacheDirectory(
                vn::String::fromUtf8(std::string_view(reinterpret_cast<const char*>(text.data()), text.size())));
        }
    }

    // THE HOST CHECKS THIS BEFORE IT TRUSTS THE FACTORY (see VN_RENDER_BACKEND_ABI_VERSION): a module built against
    // another graphics SDK revision has a different RenderBackendFactory vtable, so the revision travels with the
    // registration instead of being discovered by a crash. A refusal is already reported on the registry's route; this
    // module can do nothing about a host that is older than itself, so the answer is ignored on purpose.
    (void)vn::graphics::RenderBackendRegistry::instance().registerFactory(&s_factory, ::vineRenderBackendAbi());
    co_return;
}

void Plugin::unload(vn::appfw::PluginLoadContext* context)
{
    (void)context;
    // The registry keeps the factory for the process and the module stays mapped: nothing to tear down.
}

VN_BACKEND_VK_NS_END

// What the host reads before it calls into this module (see VN_RENDER_BACKEND_ABI_VERSION): one definition per
// module, next to the plugin's own handshake.
VN_DECLARE_RENDER_BACKEND_ABI()

VN_DECLARE_PLUGIN(vn::backend_vk::Plugin,
                  u8"7f2b6c1e-3a4d-4c9b-9f60-2d8e5a1b7c34",
                  u8"gfx_backend_vk",
                  u8"vk (直连 Vulkan 学习后端)",
                  u8"0.1.0",
                  u8"直接调用 Vulkan 的渲染后端，用于学习与研究，不参与生产路径",
                  u8"Vine",
                  u8"dev@vine.example",
                  u8"https://github.com/vine/gfx_backend_vk",
                  u8"",
                  {})
