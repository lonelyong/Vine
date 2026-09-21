#include <vine/vsg/api/OffscreenTarget.hpp>

#include <vine/vsg/core/TargetPlan.hpp>

#include <cstring>
#include <utility>
#include <vector>

#include <vsg/commands/CopyImageToBuffer.h>
#include <vsg/commands/Commands.h>
#include <vsg/nodes/Group.h>
#include <vsg/state/Buffer.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/DeviceMemory.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/RenderPass.h>

V_VSG_NS_BEGIN

namespace
{

/// @brief The engine's colour format as the API's enum (the same mapping the pipeline factory uses).
VkFormat toColorFormat(vine::graphics::RenderTarget::ColorFormat format) noexcept
{
    switch (format) {
    case vine::graphics::RenderTarget::ColorFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case vine::graphics::RenderTarget::ColorFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case vine::graphics::RenderTarget::ColorFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

/// @brief The engine's depth format as the API's enum (the same mapping the pipeline factory uses).
VkFormat toDepthFormat(vine::graphics::RenderTarget::DepthFormat format) noexcept
{
    switch (format) {
    case vine::graphics::RenderTarget::DepthFormat::D16: return VK_FORMAT_D16_UNORM;
    case vine::graphics::RenderTarget::DepthFormat::D24: return VK_FORMAT_D24_UNORM_S8_UINT;
    case vine::graphics::RenderTarget::DepthFormat::D32:
    case vine::graphics::RenderTarget::DepthFormat::D32F: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_D32_SFLOAT;
}

/**
 * @brief Builds ONE render pass variant from the shape and a pass' load-op key.
 *
 * WHY A PASS PER VARIANT, AND WHY THEY STAY COMPATIBLE. Load/store operations and layouts differ per pass -
 * the first writer of a target clears it, a later writer loads what is there - and Vulkan expresses exactly
 * that difference through the attachment descriptions. What must NOT differ is the subpass structure and the
 * dependency list: both are part of render pass COMPATIBILITY (the validator says it by name:
 * VUID-vkCmdDrawIndexed-renderPass-02684 compares pDependencies between the bound pass and the one a pipeline
 * was compiled against), so two variants with different dependencies could not share one compiled pipeline,
 * and sharing it is the whole point of putting load ops in the key rather than in the compatibility half.
 * The dependency below is therefore the SUPERSET every pass of this shape may need, built from the shape
 * alone.
 *
 * finalLayout is SHADER_READ_ONLY for every COLOUR attachment: a colour target is what a later pass samples
 * (see the file note), and leaving it in that layout is what makes the sample legal without a consumer-side
 * barrier - and it is also what a variant that LOADs declares as its initial layout. The depth stays in the
 * attachment layout: it is an attachment for the next pass and a texture for a shadow that resolved it, and
 * the two uses have their own machinery.
 *
 * @param device The device to create the pass on.
 * @param color_formats One format per colour attachment, in attachment order.
 * @param depth_format Present when the shape has a depth attachment.
 * @param variant What this variant loads, stores and leaves behind (see core::LoadOpVariantKey).
 * @return The pass, or null when the API refuses the description.
 */
::vsg::ref_ptr<::vsg::RenderPass> makeOffscreenRenderPass(
    const ::vsg::ref_ptr<::vsg::Device>&                 device,
    const std::vector<vine::graphics::RenderTarget::ColorFormat>& color_formats,
    const std::optional<vine::graphics::RenderTarget::DepthFormat>& depth_format,
    const vine::vsg::core::LoadOpVariantKey&             variant)
{
    // One spelling for one conversion: the core's neutral spellings are the API's enums, bound here.
    const auto toLoadOp = [](vine::vsg::core::LoadOp load) noexcept {
        return load == vine::vsg::core::LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    };
    const auto toLayout = [](vine::vsg::core::ImageLayout layout) noexcept {
        switch (layout) {
        case vine::vsg::core::ImageLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case vine::vsg::core::ImageLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case vine::vsg::core::ImageLayout::DepthAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case vine::vsg::core::ImageLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case vine::vsg::core::ImageLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        return VK_IMAGE_LAYOUT_UNDEFINED;
    };

    ::vsg::RenderPass::Attachments attachments;
    for (std::size_t index = 0; index < color_formats.size(); ++index) {
        ::vsg::AttachmentDescription description;
        description.flags   = 0;
        description.format  = toColorFormat(color_formats[index]);
        description.samples = VK_SAMPLE_COUNT_1_BIT;
        // All colour attachments move together - a pass clears all of them or none (see planClearValues) -
        // which is why the variant carries one entry for the set.
        description.loadOp        = toLoadOp(variant.color_load);
        description.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout = toLayout(variant.color_initial);
        description.finalLayout   = toLayout(variant.color_final);
        attachments.push_back(description);
    }
    if (variant.has_depth) {
        ::vsg::AttachmentDescription description;
        description.flags   = 0;
        description.format  = toDepthFormat(depth_format.value());
        description.samples = VK_SAMPLE_COUNT_1_BIT;
        description.loadOp        = toLoadOp(variant.depth_load);
        description.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
        description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        description.initialLayout = toLayout(variant.depth_initial);
        description.finalLayout   = toLayout(variant.depth_final);
        attachments.push_back(description);
    }

    ::vsg::SubpassDescription subpass;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    for (std::uint32_t index = 0; index < color_formats.size(); ++index) {
        ::vsg::AttachmentReference reference;
        reference.attachment = index;
        reference.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        subpass.colorAttachments.push_back(reference);
    }
    if (variant.has_depth) {
        ::vsg::AttachmentReference reference;
        reference.attachment = static_cast<std::uint32_t>(color_formats.size());
        reference.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        subpass.depthStencilAttachments.push_back(reference);
    }

    ::vsg::SubpassDependency dependency;
    dependency.srcSubpass   = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass   = 0U;
    // The SOURCE scope has to cover both things an earlier submission may have done with these attachments:
    // written them, or SAMPLED them (a colour target is a texture for the next pass). The second half is what
    // orders this pass' layout transition after those reads - without it, a frame that samples a target and
    // then writes it again would be a write-while-reading hazard that only synchronisation validation sees.
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    // The DESTINATION scope covers what a LOADING pass does as well as what a clearing one does: a pass that
    // keeps the colour READS it through the attachment, and a dependency that only named the write would
    // leave that read unordered (measured: synchronisation validation reports the chain as
    // READ_AFTER_WRITE when the bit is missing). Since the masks are part of compatibility, the superset is
    // declared for EVERY variant - a clearing pass simply does not use the read half.
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (variant.has_depth) {
        // The depth attachment is written by the tests the fragments go through, which run before the colour
        // output stage: a dependency that only covered the colour stage would let the first frame's depth
        // test overlap with the previous frame's reads.
        //
        // IT IS DRIVEN BY THE SHAPE AND NOT BY THE BORROW, and that is a correctness rule rather than a
        // style choice: the DEPENDENCY MASKS ARE PART OF RENDER-PASS COMPATIBILITY (the validator says it by
        // name - VUID-vkCmdDrawIndexed-renderPass-02684 compares pDependencies[].srcAccessMask between the
        // bound pass and the one the pipeline was compiled against). A borrower's pass that declared
        // DEPTH_STENCIL_ATTACHMENT_READ (because it reads what the lender wrote) would therefore be
        // INCOMPATIBLE with the very pipeline the lender's pass uses - the same pipeline, the same shape,
        // two passes that cannot share it. Load ops and layouts are the other way round: a pass that LOADs
        // where another CLEARs is compatible, which is why the clear plan may differ per pass and this may
        // not. So the masks are the superset that any pass of this shape may need, for every pass of it.
        dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    return ::vsg::RenderPass::create(device, attachments, ::vsg::RenderPass::Subpasses{ subpass },
                                     ::vsg::RenderPass::Dependencies{ dependency });
}

/**
 * @brief How many bytes one depth texel occupies when read back, or 0 when the format cannot be read.
 *
 * The zero is the honest half: a combined depth/stencil image (D24_UNORM_S8_UINT) has no plain depth copy
 * layout, and reading its first bytes as floats would produce numbers that look like depths and are not. A
 * format this function refuses yields no depth probe at all, which is a fact a phase can act on.
 *
 * @param format The attachment's depth format.
 * @return Bytes per texel, or 0.
 */
std::uint32_t depthBytesPerTexel(vine::graphics::RenderTarget::DepthFormat format) noexcept
{
    switch (format)
    {
    case vine::graphics::RenderTarget::DepthFormat::D16: return 2U;
    case vine::graphics::RenderTarget::DepthFormat::D32:
    case vine::graphics::RenderTarget::DepthFormat::D32F: return 4U;
    case vine::graphics::RenderTarget::DepthFormat::D24: return 0U;  // depth AND stencil: refused
    }
    return 0U;
}

/**
 * @brief Fills one render graph's clear values from a pass' clear plan, in attachment order.
 *
 * One clear value per attachment: attachment 0 gets the plan's colour, the extras the transparent black the
 * plan gives them, and the depth attachment its own value. Written once and shared by the target's own graph
 * and by every per-pass graph, so the two can never disagree about which attachment is which.
 *
 * @param graph Render graph to fill; its previous clear values are replaced.
 * @param plan  The pass' load/store decisions (from `core::planClearValues`).
 */
void fillClearValues(::vsg::RenderGraph& graph, const core::PassClearPlan& plan)
{
    graph.clearValues.clear();
    for (const core::AttachmentClear& attachment : plan.colors)
    {
        VkClearValue value = {};
        value.color        = VkClearColorValue{ { attachment.clear[0], attachment.clear[1], attachment.clear[2],
                                                 attachment.clear[3] } };
        graph.clearValues.push_back(value);
    }
    if (plan.has_depth)
    {
        VkClearValue value         = {};
        value.depthStencil.depth   = plan.depth.clear;
        value.depthStencil.stencil = 0U;
        graph.clearValues.push_back(value);
    }
}

}  // namespace

struct OffscreenTarget::Data
{
    /// @brief One colour attachment and the memory its pixels are copied back into.
    struct ColorTarget
    {
        ::vsg::ref_ptr<::vsg::Image>                         image;
        ::vsg::ref_ptr<::vsg::ImageView>                     view;
        ::vsg::ref_ptr<::vsg::Commands>                      capture;
        ::vsg::ref_ptr<::vsg::Buffer>                        destination;
        ::vsg::ref_ptr<::vsg::DeviceMemory>                  destination_memory;
        ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> mapped;
    };

    ::vsg::ref_ptr<::vsg::Device>     device;
    std::vector<ColorTarget>          colors;
    ::vsg::ref_ptr<::vsg::Image>      depth_image;
    ::vsg::ref_ptr<::vsg::ImageView>  depth_view;
    std::optional<vine::graphics::RenderTarget::DepthFormat> depth_format;
    ::vsg::ref_ptr<::vsg::Commands>                      depth_capture;
    ::vsg::ref_ptr<::vsg::Buffer>                        depth_destination;
    ::vsg::ref_ptr<::vsg::DeviceMemory>                  depth_destination_memory;
    ::vsg::ref_ptr<::vsg::MappedData<::vsg::ubyteArray>> depth_mapped;
    std::uint32_t                                        depth_bytes_per_texel{0};  ///< 0 = not readable.
    const OffscreenTarget*            depth_source{nullptr};  ///< The lender, when the depth is borrowed.
    std::uint32_t                     borrowers{0};           ///< Targets loading this target's depth.
    bool                              depth_sampleable{false};  ///< The host asked for a sampleable depth.
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass;
    ::vsg::ref_ptr<::vsg::Framebuffer> framebuffer;
    ::vsg::ref_ptr<::vsg::RenderGraph> render_graph;
    std::uint32_t                      width{0};
    std::uint32_t                      height{0};
    core::TargetShape                  shape;           ///< What the render pass was built against.
    bool                               depth_borrowed{false};  ///< The depth is the lender's image.
    /// Whether anything has been recorded into the attachments yet (see OffscreenTarget::written).
    bool                               written{false};

    /// @brief One render pass this target has served a pass with: its load-op variant and the pass object.
    struct PassVariant
    {
        core::LoadOpVariantKey            key;
        ::vsg::ref_ptr<::vsg::RenderPass> render_pass;
    };

    /// Every variant this target has built, in the order the passes asked for them; the first is the one
    /// `create` built (the bootstrap variant, whose pass the constructor's graph uses).
    std::vector<PassVariant> variants;
};

OffscreenTarget::OffscreenTarget() : d(std::make_unique<Data>())
{
}

::vsg::ref_ptr<::vsg::RenderPass> OffscreenTarget::renderPassFor(const core::LoadOpVariantKey& key)
{
    for (const Data::PassVariant& variant : d->variants) {
        if (variant.key == key) {
            return variant.render_pass;
        }
    }
    // A variant this target has not served before: build it. Every variant of one target shares the subpass
    // structure and the dependency list (see makeOffscreenRenderPass), so they are compatible with one another
    // and the pipelines compiled for one are usable in all of them.
    auto render_pass = makeOffscreenRenderPass(d->device, d->shape.color_formats, d->depth_format, key);
    if (render_pass == nullptr) {
        return {};
    }
    d->variants.push_back(Data::PassVariant{ key, render_pass });
    return render_pass;
}

std::unique_ptr<OffscreenTarget> OffscreenTarget::create(::vsg::ref_ptr<::vsg::Device> device,
                                                         const Layout&                layout)
{
    TargetLayout target_layout;
    target_layout.width        = layout.width;
    target_layout.height       = layout.height;
    target_layout.color_formats = { vine::graphics::RenderTarget::ColorFormat::RGBA8 };
    target_layout.depth_format.reset();
    target_layout.clear.color = true;
    for (std::size_t index = 0; index < 4U; ++index) {
        target_layout.clear.color_value[index] = layout.clear_color[index];
    }
    return create(std::move(device), target_layout, nullptr);
}

std::unique_ptr<OffscreenTarget> OffscreenTarget::create(::vsg::ref_ptr<::vsg::Device> device,
                                                         const TargetLayout&          layout,
                                                         const OffscreenTarget*       depth_source)
{
    if (device == nullptr || layout.width == 0U || layout.height == 0U || layout.color_formats.empty()) {
        return nullptr;
    }
    if (depth_source != nullptr) {
        // A borrowed depth has to be the same KIND of image, and there has to be one: the formats are part of
        // the pass description, so a mismatch is a pass the driver would refuse at creation and a missing depth
        // is a borrowed attachment that does not exist.
        if (depth_source->d->depth_view == nullptr || !depth_source->d->depth_format.has_value() ||
            layout.depth_format != depth_source->d->depth_format) {
            return nullptr;
        }
    }

    // The target bootstraps: its images start out UNDEFINED, so the pass clears every attachment. The plan
    // says so explicitly (a fresh image cannot be loaded), and a plan that asks for a LOAD is refused below
    // rather than passed on - loading an UNDEFINED image is not a policy choice, it is a bug. The one LOAD
    // that is not a bug is a BORROWED depth: it is the lender's image, in the lender's layout, holding what
    // the lender's pass wrote - so the depth is declared preserved and the plan refuses to clear it.
    core::TargetShape shape;
    shape.color_formats = layout.color_formats;
    shape.depth_format  = layout.depth_format;
    // The DEVICE formats the images and the render pass are actually built with: the engine's spelling is a
    // projection (RGBA8 covers both a linear and an sRGB image), and a pipeline key cannot be built from a
    // projection - an sRGB window surface and this linear target are not render-pass compatible (see
    // RenderPassCompatibility).
    for (const vine::graphics::RenderTarget::ColorFormat format : layout.color_formats) {
        shape.device_color_formats.push_back(static_cast<std::uint32_t>(toColorFormat(format)));
    }
    if (layout.depth_format.has_value()) {
        shape.device_depth_format = static_cast<std::uint32_t>(toDepthFormat(layout.depth_format.value()));
    }
    const bool                depth_borrowed = depth_source != nullptr;
    const core::PassClearPlan plan = core::planClearValues(shape, layout.clear, /*bootstrap*/ true,
                                                          /*depth_preserved*/ depth_borrowed);
    for (const core::AttachmentClear& attachment : plan.colors) {
        if (attachment.load != core::LoadOp::Clear) {
            return nullptr;
        }
    }
    if (plan.has_depth && plan.depth.load != core::LoadOp::Clear && !depth_borrowed) {
        return nullptr;
    }

    auto target = std::unique_ptr<OffscreenTarget>(new OffscreenTarget());
    target->d->device = std::move(device);
    target->d->width  = layout.width;
    target->d->height = layout.height;
    target->d->shape        = shape;
    target->d->depth_borrowed = depth_borrowed;

    const VkDeviceSize byte_count = static_cast<VkDeviceSize>(layout.width) * layout.height * 4U;
    for (const vine::graphics::RenderTarget::ColorFormat format : layout.color_formats) {
        Data::ColorTarget color;
        // The image is created with SAMPLED usage as well as colour-attachment: the pass leaves colour
        // attachments in SHADER_READ_ONLY (see makeOffscreenRenderPass) so a later pass can sample them, and an
        // image view read as a sampled image has to be created for it. TRANSFER_SRC is the readback's half (the
        // capture moves the image there and back).
        color.image                 = ::vsg::Image::create();
        color.image->imageType      = VK_IMAGE_TYPE_2D;
        color.image->format         = toColorFormat(format);
        color.image->extent         = VkExtent3D{ layout.width, layout.height, 1U };
        color.image->mipLevels      = 1U;
        color.image->arrayLayers    = 1U;
        color.image->samples        = VK_SAMPLE_COUNT_1_BIT;
        color.image->tiling         = VK_IMAGE_TILING_OPTIMAL;
        color.image->usage          = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT;
        color.image->initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.image->sharingMode    = VK_SHARING_MODE_EXCLUSIVE;
        color.view                  = ::vsg::createImageView(target->d->device, color.image, VK_IMAGE_ASPECT_COLOR_BIT);
        if (color.view == nullptr) {
            return nullptr;
        }
        target->d->colors.push_back(std::move(color));
    }

    target->d->depth_format     = layout.depth_format;
    target->d->depth_sampleable = layout.depth_sampleable;
    if (plan.has_depth && depth_borrowed) {
        // Someone else's image, in the layout its own pass leaves it in, and its own pass left the depth
        // where this pass' fragment tests can read it. Nothing is created and nothing is owned.
        target->d->depth_source = depth_source;
        target->d->depth_image  = depth_source->d->depth_image;
        target->d->depth_view   = depth_source->d->depth_view;
        ++depth_source->d->borrowers;
    }
    else if (plan.has_depth) {
        target->d->depth_image            = ::vsg::Image::create();
        target->d->depth_image->imageType = VK_IMAGE_TYPE_2D;
        target->d->depth_image->format    = toDepthFormat(layout.depth_format.value());
        target->d->depth_image->extent    = VkExtent3D{ layout.width, layout.height, 1U };
        target->d->depth_image->mipLevels = 1U;
        target->d->depth_image->arrayLayers = 1U;
        target->d->depth_image->samples   = VK_SAMPLE_COUNT_1_BIT;
        target->d->depth_image->tiling    = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC is part of the depth image's usage even though nothing samples it: a target that
        // offers a depth readback MUST be created with the usage that permits the copy, or the copy is a
        // validation error (VUID-vkCmdCopyImageToBuffer-srcImage-00186) - and a readback that is refused at
        // runtime is worth less than an image built for it.
        target->d->depth_image->usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            (layout.depth_sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0U);
        target->d->depth_image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        target->d->depth_image->sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        target->d->depth_view = ::vsg::createImageView(target->d->device, target->d->depth_image,
                                                       VK_IMAGE_ASPECT_DEPTH_BIT);
        if (target->d->depth_view == nullptr) {
            return nullptr;
        }
    }

    target->d->render_pass = makeOffscreenRenderPass(target->d->device, layout.color_formats,
                                                     layout.depth_format,
                                                     core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly,
                                                                           core::ImageLayout::DepthAttachment));
    if (target->d->render_pass == nullptr) {
        return nullptr;
    }
    // The variant create() built is the target's FIRST one, and the constructor's graph records through it.
    target->d->variants.push_back(
        Data::PassVariant{ core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly,
                                                 core::ImageLayout::DepthAttachment),
                           target->d->render_pass });

    ::vsg::ImageViews attachments;
    for (const Data::ColorTarget& color : target->d->colors) {
        attachments.push_back(color.view);
    }
    if (plan.has_depth) {
        attachments.push_back(target->d->depth_view);
    }
    target->d->framebuffer =
        ::vsg::Framebuffer::create(target->d->render_pass, attachments, layout.width, layout.height, 1U);
    if (target->d->framebuffer == nullptr) {
        return nullptr;
    }

    target->d->render_graph              = ::vsg::RenderGraph::create();
    target->d->render_graph->framebuffer = target->d->framebuffer;
    target->d->render_graph->renderPass  = target->d->render_pass;
    // The render AREA is what the pass clears and what the scissor defaults to: a default-constructed
    // RenderGraph has a zero extent, and a zero-area pass records successfully while clearing nothing -
    // which reads back as an all-zero image that looks like "the copy is broken" rather than "the pass
    // never covered a pixel".
    target->d->render_graph->renderArea = VkRect2D{ { 0, 0 }, { layout.width, layout.height } };
    target->d->render_graph->contents   = VK_SUBPASS_CONTENTS_INLINE;
    // One clear value per attachment, in attachment order: the plan's colour for attachment 0, transparent
    // black for the extras, and the depth's value for the depth attachment.
    fillClearValues(*target->d->render_graph, plan);

    // Each colour attachment gets its own host-visible destination and its own copy-back node. The buffer is
    // host visible and coherent: the copy writes it, the host reads it, and no flush stands in between (the
    // same choice the block storage makes). `bufferRowLength` is the image width in texels, so the rows
    // arrive tightly packed - which is the packing a probe insists on.
    for (std::size_t index = 0; index < target->d->colors.size(); ++index) {
        Data::ColorTarget& color = target->d->colors[index];
        color.destination = ::vsg::Buffer::create(byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_SHARING_MODE_EXCLUSIVE);
        if (color.destination == nullptr) {
            return nullptr;
        }
        color.destination->compile(target->d->device.get());
        color.destination_memory = ::vsg::DeviceMemory::create(
            target->d->device.get(), color.destination->getMemoryRequirements(target->d->device->deviceID),
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (color.destination_memory == nullptr) {
            return nullptr;
        }
        color.destination->bind(color.destination_memory, 0U);
        color.mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(color.destination_memory.get(), 0U, 0U,
                                                                   static_cast<std::size_t>(byte_count));
        if (color.mapped == nullptr || color.mapped->data() == nullptr) {
            return nullptr;
        }

        VkBufferImageCopy region = {};
        region.bufferOffset      = 0U;
        region.bufferRowLength   = layout.width;
        region.bufferImageHeight = layout.height;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0U;
        region.imageSubresource.baseArrayLayer = 0U;
        region.imageSubresource.layerCount     = 1U;
        region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
        region.imageExtent                     = VkExtent3D{ layout.width, layout.height, 1U };

        auto copy            = ::vsg::CopyImageToBuffer::create();
        copy->srcImage       = color.image;
        copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy->dstBuffer      = color.destination;
        copy->regions.push_back(region);

        // The pass leaves the attachment in SHADER_READ_ONLY (it is a texture for the next pass, see
        // makeOffscreenRenderPass), so the copy has to move it to the transfer layout AND put it back: a
        // capture may be recorded between two passes, and leaving it in TRANSFER_SRC would break the next
        // descriptor that names it. The forward transition carries the pass' writes into the transfer stage,
        // which is what makes the copied bytes the frame's picture.
        const VkImageSubresourceRange color_range{ VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U };
        auto to_transfer = ::vsg::ImageMemoryBarrier::create(
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, color.image, color_range);
        auto from_transfer = ::vsg::ImageMemoryBarrier::create(
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            color.image, color_range);

        // The barrier: the GPU's writes must be visible to the host that maps this memory.
        auto buffer_barrier = ::vsg::BufferMemoryBarrier::create(
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, color.destination, 0U, byte_count);
        auto barrier = ::vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                                                      0, buffer_barrier);

        // The copy-back writes a buffer that is written EVERY time this node is recorded, and a frame may be
        // recorded again while an earlier submission's copy is still in flight (a session keeps several frames
        // in flight). Vulkan orders submissions, but it does not make one submission's transfer visible to the
        // next without a dependency - the validation layer reports the pair as
        // `SYNC-HAZARD-WRITE-AFTER-WRITE` on the destination buffer (measured: a fixture that records two
        // off-screen frames in a row, M4c). One barrier declares the order the pair has always had in
        // practice, which is all it needs: both copies carry the same bytes.
        auto previous_copy = ::vsg::BufferMemoryBarrier::create(
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED, color.destination, 0U, byte_count);

        color.capture = ::vsg::Commands::create();
        color.capture->addChild(
            ::vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                           previous_copy));
        color.capture->addChild(::vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, to_transfer));
        color.capture->addChild(copy);
        color.capture->addChild(::vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, from_transfer));
        color.capture->addChild(barrier);
    }

    // The depth copy is built only for a readable format: the rest of the target is still usable, it just
    // cannot answer "what is in the depth buffer" (see depthBytesPerTexel).
    if (plan.has_depth && target->d->depth_view != nullptr) {
        const std::uint32_t bytes_per_texel = depthBytesPerTexel(layout.depth_format.value());
        if (bytes_per_texel != 0U) {
            const VkDeviceSize depth_bytes = static_cast<VkDeviceSize>(layout.width) * layout.height * bytes_per_texel;
            target->d->depth_bytes_per_texel = bytes_per_texel;
            target->d->depth_destination = ::vsg::Buffer::create(depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                 VK_SHARING_MODE_EXCLUSIVE);
            if (target->d->depth_destination == nullptr) {
                return nullptr;
            }
            target->d->depth_destination->compile(target->d->device.get());
            target->d->depth_destination_memory = ::vsg::DeviceMemory::create(
                target->d->device.get(),
                target->d->depth_destination->getMemoryRequirements(target->d->device->deviceID),
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (target->d->depth_destination_memory == nullptr) {
                return nullptr;
            }
            target->d->depth_destination->bind(target->d->depth_destination_memory, 0U);
            target->d->depth_mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(
                target->d->depth_destination_memory.get(), 0U, 0U, static_cast<std::size_t>(depth_bytes));
            if (target->d->depth_mapped == nullptr || target->d->depth_mapped->data() == nullptr) {
                return nullptr;
            }

            // The pass leaves the depth in the attachment layout (the next pass must be able to read it as an
            // attachment), so the copy has to move it to the transfer layout AND put it back: a one-way
            // transition would make a shared depth unusable for everything that follows this node.
            const VkImageSubresourceRange depth_range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U };
            auto to_transfer = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                target->d->depth_image, depth_range);
            auto from_transfer = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target->d->depth_image, depth_range);

            VkBufferImageCopy depth_region = {};
            depth_region.bufferOffset      = 0U;
            depth_region.bufferRowLength   = layout.width;
            depth_region.bufferImageHeight = layout.height;
            depth_region.imageSubresource  = { VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 0U, 1U };
            depth_region.imageOffset       = VkOffset3D{ 0, 0, 0 };
            depth_region.imageExtent       = VkExtent3D{ layout.width, layout.height, 1U };

            auto depth_copy            = ::vsg::CopyImageToBuffer::create();
            depth_copy->srcImage       = target->d->depth_image;
            depth_copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            depth_copy->dstBuffer      = target->d->depth_destination;
            depth_copy->regions.push_back(depth_region);

            auto depth_buffer_barrier = ::vsg::BufferMemoryBarrier::create(
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, target->d->depth_destination, 0U, depth_bytes);

            target->d->depth_capture = ::vsg::Commands::create();
            target->d->depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, to_transfer));
            target->d->depth_capture->addChild(depth_copy);
            target->d->depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                from_transfer));
            target->d->depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, depth_buffer_barrier));
        }
    }
    return target;
}

OffscreenTarget::~OffscreenTarget()
{
    // Lending is a fact of the lender's frame, so the fact goes away with the borrower: a promotion that a
    // borrower revoked comes back once nothing loads this target's depth any more.
    if (d->depth_source != nullptr && d->depth_source->d->borrowers > 0U) {
        --d->depth_source->d->borrowers;
    }
}

::vsg::ref_ptr<::vsg::RenderGraph> OffscreenTarget::renderGraph() const noexcept
{
    return d->render_graph;
}

::vsg::ref_ptr<::vsg::RenderGraph> OffscreenTarget::passGraph(const core::ClearPolicy& policy, bool bootstrap,
                                                            bool depth_preserved)
{
    if (d->render_pass == nullptr || d->framebuffer == nullptr)
    {
        return {};
    }

    // What this pass does to the attachments, resolved from the plan's three inputs, and the variant that
    // spells it out: a first writer clears, a later writer loads what is there, and a preserved depth is never
    // cleared (see core::planClearValues).
    const core::PassClearPlan plan = core::planClearValues(d->shape, policy, bootstrap, depth_preserved);
    const core::LoadOpVariantKey variant =
        core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly, core::ImageLayout::DepthAttachment);
    const ::vsg::ref_ptr<::vsg::RenderPass> render_pass = renderPassFor(variant);
    if (render_pass == nullptr)
    {
        return {};
    }
    // A pass is about to be recorded into this target, so its attachments stop being "never written": the
    // NEXT frame's facts say so (see written()). Doing it here and not after a submit is deliberate - the
    // fact is "something has been recorded into it", and a frame that is recorded but dropped is not a
    // different target.
    d->written = true;

    auto graph              = ::vsg::RenderGraph::create();
    graph->framebuffer      = d->framebuffer;
    graph->renderPass       = render_pass;
    graph->renderArea       = VkRect2D{ { 0, 0 }, { d->width, d->height } };
    graph->contents         = VK_SUBPASS_CONTENTS_INLINE;
    // One clear value per attachment, in attachment order, from the SAME plan the variant was derived from -
    // so "which attachment is which" cannot drift between the render pass and the values it clears with.
    fillClearValues(*graph, plan);
    return graph;
}

std::size_t OffscreenTarget::passVariantCount() const noexcept
{
    return d->variants.size();
}

bool OffscreenTarget::written() const noexcept
{
    return d->written;
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::capture() const noexcept
{
    return capture(0U);
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::capture(std::uint32_t attachment) const
{
    if (attachment >= d->colors.size()) {
        return nullptr;
    }
    return d->colors[attachment].capture;
}

core::PixelProbe OffscreenTarget::probe() const
{
    return probe(0U);
}

core::PixelProbe OffscreenTarget::probe(std::uint32_t attachment) const
{
    if (attachment >= d->colors.size()) {
        return core::PixelProbe(0, 0, {});
    }
    const Data::ColorTarget& color = d->colors[attachment];
    if (color.mapped == nullptr || color.mapped->data() == nullptr) {
        return core::PixelProbe(0, 0, {});
    }
    const std::size_t byte_count = static_cast<std::size_t>(d->width) * d->height * 4U;
    std::vector<std::uint8_t> pixels;
    pixels.resize(byte_count);
    std::memcpy(pixels.data(), color.mapped->data(), byte_count);
    return core::PixelProbe(static_cast<int>(d->width), static_cast<int>(d->height), std::move(pixels));
}

std::uint32_t OffscreenTarget::colorAttachmentCount() const noexcept
{
    return static_cast<std::uint32_t>(d->colors.size());
}

::vsg::ref_ptr<::vsg::ImageView> OffscreenTarget::colorView(std::uint32_t attachment) const noexcept
{
    if (attachment >= d->colors.size())
    {
        return {};
    }
    return d->colors[attachment].view;
}

core::TargetShape OffscreenTarget::shape() const noexcept
{
    return d->shape;
}

bool OffscreenTarget::hasDepth() const noexcept
{
    return d->depth_view != nullptr;
}

core::DepthPlan OffscreenTarget::depth() const noexcept
{
    core::DepthFacts facts;
    facts.has_depth = d->depth_view != nullptr;
    // The host's request is a fact here, not a decision: whether it survives is the core's answer, because
    // it depends on what this frame's passes do with the depth (see core::depthPlan).
    facts.promotion   = d->depth_sampleable;
    facts.borrowed    = d->depth_source != nullptr;
    facts.source      = d->depth_source;
    facts.any_pass_preserves_depth = d->borrowers > 0U;
    return core::depthPlan(facts);
}

const OffscreenTarget* OffscreenTarget::depthSource() const noexcept
{
    return d->depth_source;
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::captureDepth() const
{
    return d->depth_capture;
}

core::DepthProbe OffscreenTarget::depthProbe() const
{
    if (d->depth_mapped == nullptr || d->depth_mapped->data() == nullptr || d->depth_bytes_per_texel == 0U
        || d->width == 0U || d->height == 0U) {
        return core::DepthProbe();
    }
    const std::size_t texels = static_cast<std::size_t>(d->width) * d->height;
    std::vector<float> values(texels);
    if (d->depth_bytes_per_texel == 4U) {
        std::memcpy(values.data(), d->depth_mapped->data(), texels * sizeof(float));
    }
    else {
        // D16_UNORM: the stored integer is the depth, scaled by its full range - the conversion the format
        // defines, not an approximation of it.
        const auto* stored = static_cast<const std::uint16_t*>(static_cast<const void*>(d->depth_mapped->data()));
        for (std::size_t index = 0; index < texels; ++index) {
            values[index] = static_cast<float>(stored[index]) / 65535.0F;
        }
    }
    return core::DepthProbe(static_cast<int>(d->width), static_cast<int>(d->height), std::move(values));
}

std::uint32_t OffscreenTarget::width() const noexcept
{
    return d->width;
}

std::uint32_t OffscreenTarget::height() const noexcept
{
    return d->height;
}

V_VSG_NS_END
