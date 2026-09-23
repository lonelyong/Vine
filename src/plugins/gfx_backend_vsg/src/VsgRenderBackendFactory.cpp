#include <vine/vsg/VsgRenderBackendFactory.hpp>

#include <vine/vsg/api/VsgBackend.hpp>

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
    // THE NAME "vsg" MEANS THE REWRITE (see .ai/design/vsg-reimplementation.md §11.16bi): the facade is the
    // SDK's seam the rewritten backend was built around - the session, the frame protocol, the content
    // world, the off-screen half and the readbacks are the ones it drives. The implementation this replaces
    // (`VsgRenderer`) is still in this module and still driven by its own tests, but nothing creates it by
    // name any more; a second registered name would just be a second answer to "which backend is 'vsg'".
    return vine::intrusive_ptr<vine::graphics::RenderBackend>(new VsgBackend());
}

V_VSG_NS_END
