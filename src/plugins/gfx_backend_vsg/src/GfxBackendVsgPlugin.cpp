#include <vine/vsg/GfxBackendVsgPlugin.hpp>

#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/vsg/VsgRenderBackendFactory.hpp>

VN_VSG_NS_BEGIN

VN_OBJECT_META_IMPL(GfxBackendVsgPlugin, vn::appfw::Plugin)

GfxBackendVsgPlugin::GfxBackendVsgPlugin() = default;

GfxBackendVsgPlugin::~GfxBackendVsgPlugin() = default;

vn::async::Task<void> GfxBackendVsgPlugin::load(vn::appfw::PluginLoadContext* context)
{
    (void)context;
    // Register the VSG backend factory so the app can create a backend by
    // name ("vsg") without a compile-time dependency on this plugin.
    static VsgRenderBackendFactory s_factory;
    vn::graphics::RenderBackendRegistry::instance().registerFactory(&s_factory);
    co_return;
}

void GfxBackendVsgPlugin::unload(vn::appfw::PluginLoadContext* context)
{
    (void)context;
    // Registry keeps the factory alive for the process; nothing to tear down.
}

VN_VSG_NS_END

VN_DECLARE_PLUGIN(vn::vsg::GfxBackendVsgPlugin, u8"51fd4cc0-07c2-484c-bd2b-c99a51a848e7", u8"gfx_backend_vsg", u8"VSG 渲染后端",
                 u8"1.0.0", u8"VulkanSceneGraph 渲染后端插件，通过 RenderBackendRegistry 注册 vsg 后端",
                 u8"Vine", u8"dev@vine.example", u8"https://github.com/vine/gfx_backend_vsg", u8"", {})
