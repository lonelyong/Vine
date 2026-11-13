#include <vine/vk/shell/Plugin.hpp>

#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/vk/shell/BackendFactory.hpp>

VN_VK_NS_BEGIN

VN_OBJECT_META_IMPL(vn::vk::Plugin, vn::appfw::Plugin)

Plugin::Plugin()  = default;
Plugin::~Plugin() = default;

vn::async::Task<void> Plugin::load(vn::appfw::PluginLoadContext* context)
{
    (void)context;
    static BackendFactory s_factory;
    vn::graphics::RenderBackendRegistry::instance().registerFactory(&s_factory);
    co_return;
}

void Plugin::unload(vn::appfw::PluginLoadContext* context)
{
    (void)context;
    // The registry keeps the factory for the process and the module stays mapped: nothing to tear down.
}

VN_VK_NS_END

VN_DECLARE_PLUGIN(vn::vk::Plugin,
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
