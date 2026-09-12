#include <vine/vsg/VsgRenderBackendFactory.hpp>

#include "VsgRendererImpl.hpp"

V_VSG_NS_BEGIN

namespace
{

/**
 * @brief Statically self-registers the VSG backend factory.
 *
 * The object's constructor runs when this translation unit is linked, calling
 * RenderBackendRegistry::registerFactory. Note: static initialization order
 * across translation units is not guaranteed; call
 * RenderBackendRegistry::instance().create("vsg", ...) after the module has
 * been linked (e.g. from RenderEngine::initialize) rather than relying on
 * registration having happened before main().
 */
const vine::graphics::RenderBackendRegistry::Registrar<VsgRenderBackendFactory> s_vsg_registrar;

}  // namespace

VsgRenderBackendFactory::VsgRenderBackendFactory() = default;

VsgRenderBackendFactory::~VsgRenderBackendFactory() = default;

vine::graphics::RenderBackendInfo VsgRenderBackendFactory::info() const
{
    return vine::graphics::RenderBackendInfo{
        u8"vsg",                                  // name
        u8"VSG 渲染后端",                          // display_name
        u8"基于 VulkanSceneGraph 的渲染后端实现",   // description
        u8"1.0.0",                               // version
        u8"Vine",                                // vendor
        vine::graphics::RenderApi::Vulkan,        // api_flags
    };
}

vine::intrusive_ptr<vine::graphics::RenderBackend> VsgRenderBackendFactory::create()
{
    return vine::intrusive_ptr<vine::graphics::RenderBackend>(new VsgRenderer());
}

V_VSG_NS_END
