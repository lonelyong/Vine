#include <vine/vsg/VsgReadback.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include <vsg/commands/BlitImage.h>
#include <vsg/commands/CopyImageToBuffer.h>
#include <vsg/core/Data.h>
#include <vsg/vk/CommandPool.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/DeviceMemory.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/PhysicalDevice.h>
#include <vsg/vk/SubmitCommands.h>

#include <vine/graphics/RenderTarget.hpp>

#include <vine/vsg/VsgDiagnostics.hpp>
#include <vine/vsg/VsgUtils.hpp>

V_VSG_NS_BEGIN

namespace detail
{

bool submitOneShot(const VsgRendererState& state, const ::vsg::ref_ptr<::vsg::Commands>& commands)
{
    // Generous because the wait is what makes the readback synchronous: a timeout
    // here means the GPU never finished the transfer, not that it was slow.
    constexpr std::uint64_t kReadbackTimeoutNs = 100'000'000'000ull;

    auto device   = state.window != nullptr ? state.window->getDevice() : ::vsg::ref_ptr<::vsg::Device>();
    auto physical = state.window != nullptr ? state.window->getPhysicalDevice() : ::vsg::ref_ptr<::vsg::PhysicalDevice>();
    if (device == nullptr || physical == nullptr) {
        return false; // no usable device: there is nothing to submit to
    }
    const auto queue_family = physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto       command_pool = ::vsg::CommandPool::create(device, queue_family);
    auto       fence        = ::vsg::Fence::create(device);
    auto       queue        = device->getQueue(queue_family);
    ::vsg::submitCommandsToQueue(command_pool, fence, kReadbackTimeoutNs, queue,
                                [&commands](::vsg::CommandBuffer& command_buffer) { commands->record(command_buffer); });
    return true;
}

::vsg::ref_ptr<::vsg::DeviceMemory> hostVisibleMemory(const VsgRendererState& state, ::vsg::Device* device,
                                                      const VkMemoryRequirements& requirements)
{
    return ::vsg::DeviceMemory::create(device, requirements,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

const VsgRenderTargetEntry* readbackTarget(const VsgRendererState& state,
                                           vine::graphics::RenderTarget* target)
{
    if (target == nullptr || state.viewer == nullptr || state.window == nullptr) {
        // Nothing has been rendered off-screen in this session: the request is
        // unsupported, which is what the readback's false means (base contract).
        return nullptr;
    }
    const auto entry = state.targets.find(target);
    if (entry == state.targets.end() || !entry->second.attachments_built) {
        return nullptr;
    }
    const VsgRenderTargetEntry& built = entry->second;
    return (built.width > 0 && built.height > 0) ? &built : nullptr;
}

bool readColorBuffer(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                     vine::graphics::RenderTarget* target, int attachment, std::vector<std::uint8_t>& out_pixels)
{
    auto* built = readbackTarget(state, target);
    if (built == nullptr) {
        return false;
    }
    if (attachment < 0 || static_cast<std::size_t>(attachment) >= built->color_images.size()) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Error,
                           vine::graphics::DiagnosticCategory::ContentSkipped,
                           formatDiagnostic(u8"readColorBuffer: attachment %d is out of range for this target", attachment));
        return false;
    }
    // The packed-RGBA8 output contract needs a same-format blit. A float
    // attachment would have to be converted (and the caller told how), so it is
    // reported as unsupported instead of returning wrongly packed bytes.
    if (target->colorFormat(attachment) != vine::graphics::RenderTarget::ColorFormat::RGBA8) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::ContentSkipped,
                           formatDiagnostic(u8"readColorBuffer: attachment %d is not RGBA8 (packed RGBA8 readback only)", attachment));
        return false;
    }

    const std::uint32_t width  = static_cast<std::uint32_t>(built->width);
    const std::uint32_t height = static_cast<std::uint32_t>(built->height);

    // The frame that wrote this target must be complete before its image is
    // copied out; this call is synchronous by contract. Counted, like every other
    // device-wide idle this backend takes (deviceWaitCount).
    state.retireRing.waitForIdle(state.viewer);

    auto device   = state.window->getDevice();
    auto physical = state.window->getPhysicalDevice();
    auto source   = built->color_images[attachment];
    if (device == nullptr || physical == nullptr || source == nullptr) {
        return false;
    }
    const VkFormat format = source->format;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(*physical, format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0 ||
        (properties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                           vine::graphics::DiagnosticCategory::ContentSkipped,
                           formatDiagnostic(u8"readColorBuffer: format %d cannot be blitted", static_cast<int>(format)));
        return false;
    }

    // A linear host-visible image the pixels land in (rows may carry padding,
    // see rowPitch below): the same shape the standalone colour probe uses.
    auto destination         = ::vsg::Image::create();
    destination->imageType   = VK_IMAGE_TYPE_2D;
    destination->format      = format;
    destination->extent      = VkExtent3D{ width, height, 1 };
    destination->mipLevels   = 1;
    destination->arrayLayers = 1;
    destination->samples     = VK_SAMPLE_COUNT_1_BIT;
    destination->tiling      = VK_IMAGE_TILING_LINEAR;
    destination->usage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    destination->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    destination->compile(device);
    auto memory = hostVisibleMemory(state, device.get(), destination->getMemoryRequirements(device->deviceID));
    destination->bind(memory, 0);

    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    auto                          commands = ::vsg::Commands::create();
    // The attachment is sampleable when the render pass ends (its final layout),
    // so it is made a transfer source here and handed back below: the next
    // frame samples it again.
    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, range),
        ::vsg::ImageMemoryBarrier::create(0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range)));

    VkImageBlit region{};
    region.srcSubresource = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.srcOffsets[1]  = VkOffset3D{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1 };
    region.dstSubresource = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.dstOffsets[1]  = VkOffset3D{ static_cast<std::int32_t>(width), static_cast<std::int32_t>(height), 1 };
    auto blit            = ::vsg::BlitImage::create();
    blit->srcImage       = source;
    blit->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blit->dstImage       = destination;
    blit->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blit->regions.push_back(region);
    blit->filter = VK_FILTER_NEAREST;
    commands->addChild(blit);

    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, source, range),
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, destination, range)));

    if (!submitOneShot(state, commands)) {
        return false;
    }

    VkImageSubresource  sub_resource{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    VkSubresourceLayout sub_layout{};
    vkGetImageSubresourceLayout(*device, destination->vk(device->deviceID), &sub_resource, &sub_layout);
    auto mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory, sub_layout.offset, 0,
                                                               ::vsg::Data::Properties{ format }, sub_layout.rowPitch * height);

    // Pack the rows: a linear image may pad them (rowPitch), the caller gets
    // tightly packed RGBA8.
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4u;
    out_pixels.resize(row_bytes * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        std::memcpy(out_pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                    mapped->dataPointer(static_cast<std::size_t>(row) * sub_layout.rowPitch), row_bytes);
    }
    return true;
}

bool readDepthBuffer(VsgRendererState& state, const VsgDiagnostics& diagnostics,
                     vine::graphics::RenderTarget* target, std::vector<float>& out_depths)
{
    auto* built = readbackTarget(state, target);
    if (built == nullptr) {
        return false;
    }
    if (built->depth_image == nullptr) {
        if (built->depth_source != nullptr) {
            // The depth is real but owned by the target it was borrowed from
            // (shareDepth): it is read through the SOURCE, not the borrower, so
            // this is a documented unsupported case rather than a silent one.
            diagnostics.report(vine::graphics::DiagnosticSeverity::Warning,
                               vine::graphics::DiagnosticCategory::ChannelIgnored,
                               u8"readDepthBuffer: this target borrows its depth (shareDepth); read the source target instead");
        }
        return false;
    }

    // Only the formats whose texels ARE the value are read back: the packed
    // D24_UNORM_S8_UINT needs the implementation's bit convention to decode, so
    // it is reported as unsupported rather than decoded on a guess.
    const VkFormat      format      = built->depth_image->format;
    const std::size_t   texel_bytes = (format == VK_FORMAT_D32_SFLOAT) ? 4u : (format == VK_FORMAT_D16_UNORM) ? 2u : 0u;
    if (texel_bytes == 0u) {
        diagnostics.report(vine::graphics::DiagnosticSeverity::Warning, vine::graphics::DiagnosticCategory::ContentSkipped,
                           formatDiagnostic(u8"readDepthBuffer: format %d is packed (depth + stencil in one texel) and is "
                                            u8"not decoded; use D32_SFLOAT or D16_UNORM for depth readback",
                                            static_cast<int>(format)));
        return false;
    }

    const std::uint32_t width  = static_cast<std::uint32_t>(built->width);
    const std::uint32_t height = static_cast<std::uint32_t>(built->height);

    // Synchronous by contract, and counted: see readColorBuffer.
    state.retireRing.waitForIdle(state.viewer);

    // The transfer needs the device; the physical device / queue family come from
    // the one-shot submit, which guards for them itself.
    auto device = state.window->getDevice();
    if (device == nullptr) {
        return false;
    }

    // A depth image cannot be blitted (no blit support in the format's feature
    // set), and a LINEAR depth image is not guaranteed either, so the copy goes
    // through a host-visible staging buffer: vkCmdCopyImageToBuffer is mandatory
    // for depth formats.
    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) *
                                    static_cast<VkDeviceSize>(texel_bytes);
    auto buffer = ::vsg::Buffer::create(byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE);
    buffer->compile(device);
    auto memory = hostVisibleMemory(state, device.get(), buffer->getMemoryRequirements(device->deviceID));
    buffer->bind(memory, 0);

    // The depth image is left in whatever layout the last pass of this target
    // ends in: a pass that promoted it leaves SHADER_READ_ONLY_OPTIMAL, an
    // ordinary one leaves the attachment layout. There is no single
    // target-level render pass any more, so the layout comes from the target's
    // recorded depth policy.
    const VkImageLayout depth_layout = built->depth_sampleable ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    auto                          commands = ::vsg::Commands::create();
    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                          depth_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built->depth_image, range)));

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                = 0;
    region.bufferImageHeight              = 0;
    region.imageSubresource                = VkImageSubresourceLayers{ VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
    region.imageExtent                     = VkExtent3D{ width, height, 1 };
    auto copy                              = ::vsg::CopyImageToBuffer::create();
    copy->srcImage                         = built->depth_image;
    copy->srcImageLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copy->dstBuffer                        = buffer;
    copy->regions.push_back(region);
    commands->addChild(copy);

    commands->addChild(::vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_HOST_BIT,
        0,
        ::vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_layout,
                                          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, built->depth_image, range)));

    if (!submitOneShot(state, commands)) {
        return false;
    }

    auto mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(memory, 0, 0,
                                                               ::vsg::Data::Properties{ VK_FORMAT_R8_UNORM }, byte_count);
    out_depths.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < out_depths.size(); ++i) {
        if (texel_bytes == 4u) {
            float value = 0.0f;
            std::memcpy(&value, mapped->dataPointer(i * 4u), 4u);
            out_depths[i] = value;
        }
        else {
            std::uint16_t value = 0u;
            std::memcpy(&value, mapped->dataPointer(i * 2u), 2u);
            out_depths[i] = static_cast<float>(value) / 65535.0f;
        }
    }
    return true;
}

} // namespace detail

V_VSG_NS_END
