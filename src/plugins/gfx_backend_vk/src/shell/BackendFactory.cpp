#include <vine/backend_vk/shell/BackendFactory.hpp>

#include <vine/backend_vk/internal/Device.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Statically self-registers the factory.
 *
 * Inside a plugin MODULE this runs when the module is loaded; a test that compiles the sources directly gets it at
 * static-initialisation time. Either way the name is what a host uses, never the pointer.
 */
const vn::graphics::RenderBackendRegistry::Registrar<BackendFactory> s_vk_registrar;

} // namespace

BackendFactory::BackendFactory()  = default;
BackendFactory::~BackendFactory() = default;

vn::graphics::RenderBackendInfo BackendFactory::info() const
{
    return vn::graphics::RenderBackendInfo{
        u8"vk",                                          // name
        u8"vk (直连 Vulkan 学习后端)",                    // display_name
        u8"直接调用 Vulkan 的渲染后端，用于学习与研究，不参与生产路径", // description
        u8"0.1.0",                                        // version
        u8"Vine",                                         // vendor
        vn::graphics::RenderApi::Vulkan,                   // api_flags
    };
}

vn::intrusive_ptr<vn::graphics::RenderBackend> BackendFactory::create(const vn::graphics::DeviceDesc& desc)
{
    return vn::intrusive_ptr<vn::graphics::RenderBackend>(new Device(desc));
}

VN_BACKEND_VK_NS_END
