#include <vine/vsg/api/OffscreenTarget.hpp>

#include <vine/vsg/VsgBackendUtility.hpp>
#include <vine/vsg/core/TargetPlan.hpp>

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vsg/commands/CopyImageToBuffer.h>
#include <vsg/core/Exception.h>
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

VN_VSG_NS_BEGIN

namespace
{

/// @brief The engine's colour format as the API's enum (the same mapping the pipeline factory uses).
VkFormat toColorFormat(vn::graphics::RenderTarget::ColorFormat format) noexcept
{
    switch (format) {
    case vn::graphics::RenderTarget::ColorFormat::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case vn::graphics::RenderTarget::ColorFormat::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case vn::graphics::RenderTarget::ColorFormat::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

/// @brief The engine's depth format as the API's enum (the same mapping the pipeline factory uses).
VkFormat toDepthFormat(vn::graphics::RenderTarget::DepthFormat format) noexcept{
    switch (format) {
    case vn::graphics::RenderTarget::DepthFormat::D16: return VK_FORMAT_D16_UNORM;
    case vn::graphics::RenderTarget::DepthFormat::D24: return VK_FORMAT_D24_UNORM_S8_UINT;
    case vn::graphics::RenderTarget::DepthFormat::D32:
    case vn::graphics::RenderTarget::DepthFormat::D32F: return VK_FORMAT_D32_SFLOAT;
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

/**
 * @brief Turns one of the core's neutral layouts into the API's enum.
 *
 * One spelling for one conversion: the render pass description and the capture barriers both name layouts,
 * and a second switch would be a second chance to disagree.
 *
 * @param layout The core's layout.
 * @return The API's layout (UNDEFINED for a layout the core does not have).
 */
VkImageLayout toVkLayout(vn::vsg::core::ImageLayout layout) noexcept
{
    switch (layout) {
    case vn::vsg::core::ImageLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case vn::vsg::core::ImageLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case vn::vsg::core::ImageLayout::DepthAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case vn::vsg::core::ImageLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case vn::vsg::core::ImageLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}
::vsg::ref_ptr<::vsg::RenderPass> makeOffscreenRenderPass(
    const ::vsg::ref_ptr<::vsg::Device>&                 device,
    const std::vector<vn::graphics::RenderTarget::ColorFormat>& color_formats,
    const std::optional<vn::graphics::RenderTarget::DepthFormat>& depth_format,
    const vn::vsg::core::LoadOpVariantKey&             variant)
{
    // One spelling for one conversion: the core's neutral spellings are the API's enums, bound here.
    const auto toLoadOp = [](vn::vsg::core::LoadOp load) noexcept {
        return load == vn::vsg::core::LoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    };
    const auto toLayout = [](vn::vsg::core::ImageLayout layout) noexcept { return toVkLayout(layout); };

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
    ::vsg::ref_ptr<::vsg::Device>     device;
    /// Everything whose description contains the extent: replaced as ONE set by a resize, and parked as one set
    /// when it is (see OffscreenTarget::Attachments).
    Attachments                    attachments;
    std::optional<vn::graphics::RenderTarget::DepthFormat> depth_format;
    const OffscreenTarget*            depth_source{nullptr};  ///< The lender, when the depth is borrowed.
    std::shared_ptr<std::uint32_t> lending{};  ///< The count of targets loading this depth: SHARED with them.
    bool                              depth_sampleable{false};  ///< The host asked for a sampleable depth.
    /// The clear policy `create` was given: the initial graph's clear values come from it, and a rebuilt graph
    /// has to use the same policy - values a caller never re-announces must not change under it.
    core::ClearPolicy                 clear_policy{};
    /// Bumped every time the attachments are replaced (see resize): a caller can tell "the same target, new
    /// images" from "the same images" without comparing pointers.
    std::uint64_t                     generation{0};
    ::vsg::ref_ptr<::vsg::RenderPass> render_pass;
    std::uint32_t                      width{0};
    std::uint32_t                      height{0};
    core::TargetShape                  shape;           ///< What the render pass was built against.
    bool                               depth_borrowed{false};  ///< The depth is the lender's image.
    /// Whether anything has been recorded into the attachments yet (see OffscreenTarget::written).
    bool                               written{false};
    /// Whether a submission into these attachments was lost: they exist and were written into, but their
    /// contents can no longer be trusted (see OffscreenTarget::invalidateAttachments). The plan answers
    /// Repair(Bootstrap) for such a target, so the next frame's first writer clears instead of loading.
    bool                               attachments_invalidated{false};

    /// @brief Whether the CURRENT image set has been named for the validation layer (see nameImages).
    ///
    /// A fresh set starts unnamed, so the flag is cleared wherever attachments are built; the executor
    /// retries every frame until the images are allocated and the names stick.
    bool                               names_set{false};
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

core::ImageLayout OffscreenTarget::depthSteadyLayout() const noexcept
{
    // The image's OWNER answers: a borrowed depth is wherever the lender leaves it, and a lender that leaves it
    // sampleable (a shadow map) makes the borrower's pass START in that layout - the alternative is a pass that
    // declares the attachment layout for an image that is not in it.
    const OffscreenTarget* owner = d->depth_source != nullptr ? d->depth_source : this;
    return core::depthFinalLayout(owner->d->shape, owner->d->depth_sampleable);
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

core::TargetShape OffscreenTarget::shapeOf(const TargetLayout& layout)
{
    core::TargetShape shape;
    shape.color_formats = layout.color_formats;
    shape.depth_format  = layout.depth_format;
    // The DEVICE formats the images and the render pass are actually built with: the engine's spelling is a
    // projection (RGBA8 covers both a linear and an sRGB image), and a pipeline key cannot be built from a
    // projection - an sRGB window surface and this linear target are not render-pass compatible (see
    // RenderPassCompatibility).
    for (const vn::graphics::RenderTarget::ColorFormat format : layout.color_formats)
    {
        shape.device_color_formats.push_back(static_cast<std::uint32_t>(toColorFormat(format)));
    }
    if (layout.depth_format.has_value())
    {
        shape.device_depth_format = static_cast<std::uint32_t>(toDepthFormat(layout.depth_format.value()));
    }
    return shape;
}

OffscreenTarget::TargetLayout OffscreenTarget::layout() const noexcept
{
    TargetLayout out;
    out.width            = d->width;
    out.height           = d->height;
    out.color_formats    = d->shape.color_formats;
    out.depth_format     = d->depth_format;
    out.depth_sampleable = d->depth_sampleable;
    out.clear            = d->clear_policy;
    return out;
}

std::unique_ptr<OffscreenTarget> OffscreenTarget::create(::vsg::ref_ptr<::vsg::Device> device,
                                                         const Layout&                layout)
{
    TargetLayout target_layout;
    target_layout.width         = layout.width;
    target_layout.height        = layout.height;
    target_layout.color_formats = { vn::graphics::RenderTarget::ColorFormat::RGBA8 };
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
    if (device == nullptr || layout.width == 0U || layout.height == 0U ||
        (layout.color_formats.empty() && !layout.depth_format.has_value())) {
        // A target with NOTHING to attach is not a target; a DEPTH-ONLY one is the shadow-map shape and is
        // allowed (no colour attachment, a depth attachment, and the host's decision whether a shader may
        // sample it - see core::depthFinalLayout).
        return nullptr;
    }
    if (depth_source != nullptr) {
        // A borrowed depth has to be the same KIND of image, and there has to be one: the formats are part of
        // the pass description, so a mismatch is a pass the driver would refuse at creation and a missing depth
        // is a borrowed attachment that does not exist.
        if (depth_source->d->attachments.depth_view == nullptr || !depth_source->d->depth_format.has_value() ||
            layout.depth_format != depth_source->d->depth_format) {
            return nullptr;
        }
    }

    // The target bootstraps: its images start out UNDEFINED, so the pass clears every attachment. The plan
    // says so explicitly (a fresh image cannot be loaded), and a plan that asks for a LOAD is refused below
    // rather than passed on - loading an UNDEFINED image is not a policy choice, it is a bug. The one LOAD
    // that is not a bug is a BORROWED depth: it is the lender's image, in the lender's layout, holding what
    // the lender's pass wrote - so the depth is declared BORROWED and the plan refuses to clear it.
    const core::TargetShape shape = shapeOf(layout);
    const bool                depth_borrowed = depth_source != nullptr;
    const core::PassClearPlan plan =
        core::planClearValues(shape, layout.clear, /*bootstrap*/ true, /*depth_borrowed*/ depth_borrowed);
    for (const core::AttachmentClear& attachment : plan.colors) {
        if (attachment.load != core::LoadOp::Clear) {
            return nullptr;
        }
    }
    if (plan.has_depth && plan.depth.load != core::LoadOp::Clear && !depth_borrowed) {
        return nullptr;
    }

    auto target = std::unique_ptr<OffscreenTarget>(new OffscreenTarget());
    target->d->device           = std::move(device);
    target->d->width            = layout.width;
    target->d->height           = layout.height;
    target->d->shape            = shape;
    target->d->depth_borrowed   = depth_borrowed;
    target->d->depth_format     = layout.depth_format;
    target->d->depth_sampleable = layout.depth_sampleable;
    target->d->clear_policy     = layout.clear;
    // The borrower count lives in storage the LENDER and every borrower share, not in the lender's object: a
    // borrower counts DOWN at destruction, and it may well outlive the lender (two registry entries released in
    // the lender's order, a host that drops the lender and keeps drawing into the borrower for a frame) - a
    // count kept in the lender would make that destruction a read of freed memory (measured: a double free in
    // the test suite, and the same two lines could be reached by a host's release order).
    target->d->lending = std::make_shared<std::uint32_t>(0U);
    if (depth_borrowed) {
        target->d->depth_source = depth_source;
        target->d->lending      = depth_source->d->lending;
    }

    // The pass and its first variant are the SHAPE's, built once for the target's life: an extent is not part of
    // render-pass compatibility (see TargetShape), so a resize keeps them and a pipeline compiled for this target
    // stays the pipeline for it (see resize).
    target->d->render_pass = makeOffscreenRenderPass(target->d->device, layout.color_formats,
                                                     layout.depth_format,
                                                     core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly,
                                                                           target->depthSteadyLayout(),
                                                                           target->depthSteadyLayout()));
    if (target->d->render_pass == nullptr) {
        return nullptr;
    }
    target->d->variants.push_back(
        Data::PassVariant{ core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly,
                                                 target->depthSteadyLayout(), target->depthSteadyLayout()),
                           target->d->render_pass });

    // Everything whose description contains the EXTENT is built by the same function a resize uses. The target
    // counts as a borrower only once that build succeeded: a create that failed halfway must not leave a lender
    // believing something loads its depth.
    Attachments built;
    if (!target->buildAttachments(layout.width, layout.height, built)) {
        return nullptr;
    }
    target->d->attachments = std::move(built);
    if (depth_borrowed) {
        ++*target->d->lending;
    }
    return target;
}

bool OffscreenTarget::buildAttachments(std::uint32_t width, std::uint32_t height,
                                        Attachments& out)
{
    // Fresh images: whatever names the old set had belong to the old set (see nameImages).
    d->names_set = false;

    const core::PassClearPlan plan =
        core::planClearValues(d->shape, d->clear_policy, /*bootstrap*/ true, /*depth_borrowed*/ d->depth_borrowed);
    for (const vn::graphics::RenderTarget::ColorFormat format : d->shape.color_formats) {
        OffscreenTarget::Attachments::Color color;
        // The image is created with SAMPLED usage as well as colour-attachment: the pass leaves colour
        // attachments in SHADER_READ_ONLY (see makeOffscreenRenderPass) so a later pass can sample them, and an
        // image view read as a sampled image has to be created for it. TRANSFER_SRC is the readback's half (the
        // capture moves the image there and back).
        color.image                 = ::vsg::Image::create();
        color.image->imageType      = VK_IMAGE_TYPE_2D;
        color.image->format         = toColorFormat(format);
        color.image->extent         = VkExtent3D{ width, height, 1U };
        color.image->mipLevels      = 1U;
        color.image->arrayLayers    = 1U;
        color.image->samples        = VK_SAMPLE_COUNT_1_BIT;
        color.image->tiling         = VK_IMAGE_TILING_OPTIMAL;
        color.image->usage          = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT;
        color.image->initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        color.image->sharingMode    = VK_SHARING_MODE_EXCLUSIVE;
        color.view                  = ::vsg::createImageView(d->device, color.image, VK_IMAGE_ASPECT_COLOR_BIT);
        if (color.view == nullptr) {
            return false;
        }
        out.colors.push_back(std::move(color));
    }

    if (plan.has_depth && d->depth_borrowed) {
        // Someone else's image, in the layout its own pass leaves it in, and its own pass left the depth
        // where this pass' fragment tests can read it. Nothing is created and nothing is owned here: the
        // lender keeps its reference, and the borrower count is the CALLER's business (create counts one
        // only once this build succeeded; a resize never does, because it refuses while one exists).
        out.depth_image = d->depth_source->d->attachments.depth_image;
        out.depth_view  = d->depth_source->d->attachments.depth_view;
    }
    else if (plan.has_depth) {
        out.depth_image            = ::vsg::Image::create();
        out.depth_image->imageType = VK_IMAGE_TYPE_2D;
        out.depth_image->format    = toDepthFormat(d->depth_format.value());
        out.depth_image->extent    = VkExtent3D{ width, height, 1U };
        out.depth_image->mipLevels = 1U;
        out.depth_image->arrayLayers = 1U;
        out.depth_image->samples   = VK_SAMPLE_COUNT_1_BIT;
        out.depth_image->tiling    = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC is part of the depth image's usage even though nothing samples it: a target that
        // offers a depth readback MUST be created with the usage that permits the copy, or the copy is a
        // validation error (VUID-vkCmdCopyImageToBuffer-srcImage-00186) - and a readback that is refused at
        // runtime is worth less than an image built for it.
        out.depth_image->usage     = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            (d->depth_sampleable ? VK_IMAGE_USAGE_SAMPLED_BIT : 0U);
        out.depth_image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        out.depth_image->sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        out.depth_view = ::vsg::createImageView(d->device, out.depth_image,
                                                       VK_IMAGE_ASPECT_DEPTH_BIT);
        if (out.depth_view == nullptr) {
            return false;
        }
    }

    ::vsg::ImageViews attachments;
    for (const OffscreenTarget::Attachments::Color& color : out.colors) {
        attachments.push_back(color.view);
    }
    if (plan.has_depth) {
        attachments.push_back(out.depth_view);
    }
    out.framebuffer = ::vsg::Framebuffer::create(d->render_pass, attachments, width, height, 1U);
    if (out.framebuffer == nullptr) {
        return false;
    }

    out.render_graph              = ::vsg::RenderGraph::create();
    out.render_graph->framebuffer = out.framebuffer;
    out.render_graph->renderPass  = d->render_pass;
    // The render AREA is what the pass clears and what the scissor defaults to: a default-constructed
    // RenderGraph has a zero extent, and a zero-area pass records successfully while clearing nothing -
    // which reads back as an all-zero image that looks like "the copy is broken" rather than "the pass
    // never covered a pixel".
    out.render_graph->renderArea = VkRect2D{ { 0, 0 }, { width, height } };
    out.render_graph->contents   = VK_SUBPASS_CONTENTS_INLINE;
    // One clear value per attachment, in attachment order: the plan's colour for attachment 0, transparent
    // black for the extras, and the depth's value for the depth attachment.
    fillClearValues(*out.render_graph, plan);

    // Each READABLE colour attachment gets its own host-visible destination and its own copy-back node. The
    // buffer is host visible and coherent: the copy writes it, the host reads it, and no flush stands in
    // between (the same choice the block storage makes). `bufferRowLength` is the image width in texels, so
    // the rows arrive tightly packed - which is the packing a probe insists on.
    //
    // A format this backend cannot pack for the CPU (a float attachment: see core::colorReadbackOf) gets NO
    // destination and NO copy: the copy would have to be sized by a format the probe cannot read, and a
    // buffer sized as if the texels were RGBA8 would make vkCmdCopyImageToBuffer write past its end
    // (VUID-vkCmdCopyImageToBuffer-pRegions-00183). The pass still renders into it; only the readback is
    // refused, and it says so (core::ReadbackRefusal::UnreadableFormat).
    for (std::size_t index = 0; index < out.colors.size(); ++index) {
        OffscreenTarget::Attachments::Color& color = out.colors[index];
        const core::ReadbackFormat           packed = core::colorReadbackOf(d->shape.color_formats[index]);
        if (!packed.readable) {
            continue;
        }
        const VkDeviceSize color_byte_count = static_cast<VkDeviceSize>(width) * height * packed.bytes_per_texel;
        color.destination = ::vsg::Buffer::create(color_byte_count, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                  VK_SHARING_MODE_EXCLUSIVE);
        if (color.destination == nullptr) {
            return false;
        }
        color.destination->compile(d->device.get());
        color.destination_memory = ::vsg::DeviceMemory::create(
            d->device.get(), color.destination->getMemoryRequirements(d->device->deviceID),
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (color.destination_memory == nullptr) {
            return false;
        }
        color.destination->bind(color.destination_memory, 0U);
        color.mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(color.destination_memory.get(), 0U, 0U,
                                                                   static_cast<std::size_t>(color_byte_count));
        if (color.mapped == nullptr || color.mapped->data() == nullptr) {
            return false;
        }

        VkBufferImageCopy region = {};
        region.bufferOffset      = 0U;
        region.bufferRowLength   = width;
        region.bufferImageHeight = height;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0U;
        region.imageSubresource.baseArrayLayer = 0U;
        region.imageSubresource.layerCount     = 1U;
        region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
        region.imageExtent                     = VkExtent3D{ width, height, 1U };

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
            VK_QUEUE_FAMILY_IGNORED, color.destination, 0U, color_byte_count);
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
            VK_QUEUE_FAMILY_IGNORED, color.destination, 0U, color_byte_count);

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
    // cannot answer "what is in the depth buffer" (see core::depthReadbackOf, and the refusal it makes the
    // target report).
    if (plan.has_depth && out.depth_view != nullptr) {
        const core::ReadbackFormat packed = core::depthReadbackOf(d->depth_format.value());
        if (packed.readable) {
            const VkDeviceSize depth_bytes = static_cast<VkDeviceSize>(width) * height * packed.bytes_per_texel;
            out.depth_destination = ::vsg::Buffer::create(depth_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                                 VK_SHARING_MODE_EXCLUSIVE);
            if (out.depth_destination == nullptr) {
                return false;
            }
            out.depth_destination->compile(d->device.get());
            out.depth_destination_memory = ::vsg::DeviceMemory::create(
                d->device.get(),
                out.depth_destination->getMemoryRequirements(d->device->deviceID),
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (out.depth_destination_memory == nullptr) {
                return false;
            }
            out.depth_destination->bind(out.depth_destination_memory, 0U);
            out.depth_mapped = ::vsg::MappedData<::vsg::ubyteArray>::create(
                out.depth_destination_memory.get(), 0U, 0U, static_cast<std::size_t>(depth_bytes));
            if (out.depth_mapped == nullptr || out.depth_mapped->data() == nullptr) {
                return false;
            }

            // The pass leaves the depth in ITS OWN steady layout - the attachment layout for an ordinary
            // depth, SHADER_READ_ONLY for a sampleable depth-only shape (see core::depthFinalLayout) - so the
            // copy has to move it to the transfer layout AND put it back into exactly that layout: a one-way
            // transition, or one that names the wrong layout on the way out, would make a shared depth
            // unusable for everything that follows this node (measured: the transition that claimed
            // DEPTH_STENCIL_ATTACHMENT_OPTIMAL for a shadow map in SHADER_READ_ONLY is
            // VUID-VkImageMemoryBarrier-oldLayout-01197).
            const VkImageSubresourceRange depth_range{ VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 1U, 0U, 1U };
            const VkImageLayout steady_layout = toVkLayout(depthSteadyLayout());
            auto to_transfer = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, steady_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, out.depth_image, depth_range);
            auto from_transfer = ::vsg::ImageMemoryBarrier::create(
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, steady_layout, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, out.depth_image, depth_range);

            VkBufferImageCopy depth_region = {};
            depth_region.bufferOffset      = 0U;
            depth_region.bufferRowLength   = width;
            depth_region.bufferImageHeight = height;
            depth_region.imageSubresource  = { VK_IMAGE_ASPECT_DEPTH_BIT, 0U, 0U, 1U };
            depth_region.imageOffset       = VkOffset3D{ 0, 0, 0 };
            depth_region.imageExtent       = VkExtent3D{ width, height, 1U };

            auto depth_copy            = ::vsg::CopyImageToBuffer::create();
            depth_copy->srcImage       = out.depth_image;
            depth_copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            depth_copy->dstBuffer      = out.depth_destination;
            depth_copy->regions.push_back(depth_region);

            auto depth_buffer_barrier = ::vsg::BufferMemoryBarrier::create(
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT, VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED, out.depth_destination, 0U, depth_bytes);

            out.depth_capture = ::vsg::Commands::create();
            out.depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, to_transfer));
            out.depth_capture->addChild(depth_copy);
            out.depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, from_transfer));
            out.depth_capture->addChild(::vsg::PipelineBarrier::create(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, depth_buffer_barrier));
        }
    }
    // A build that REACHED this point has handed out every image.
    return true;
}

OffscreenTarget::~OffscreenTarget()
{
    // Lending is a fact of the lender's frame, so the fact goes away with the borrower: a promotion that a
    // borrower revoked comes back once nothing loads this target's depth any more. The count is the SHARED one
    // (see create), so this does not touch the lender - which may already be gone, and must not be read then.
    if (d->depth_source != nullptr && d->lending != nullptr && *d->lending > 0U) {
        --*d->lending;
    }
}

::vsg::ref_ptr<::vsg::RenderGraph> OffscreenTarget::renderGraph() const noexcept
{
    return d->attachments.render_graph;
}

::vsg::ref_ptr<::vsg::RenderGraph> OffscreenTarget::passGraph(const core::ClearPolicy& policy, bool bootstrap)
{
    if (d->render_pass == nullptr || d->attachments.framebuffer == nullptr)
    {
        return {};
    }

    // What this pass does to the attachments, resolved from the plan's three inputs, and the variant that
    // spells it out: a first writer clears, a later writer loads what is there, and a preserved depth is never
    // cleared (see core::planClearValues).
    const core::PassClearPlan plan = core::planClearValues(d->shape, policy, bootstrap, d->depth_borrowed);
    const core::LoadOpVariantKey variant =
        core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly, depthSteadyLayout(), depthSteadyLayout());
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
    // An INVALIDATED target is repaired by exactly this kind of pass: one that bootstraps CLEARS the
    // attachments, which is what turns "contents unknown" back into "contents known - the clear colour".
    // Only a bootstrapping frame may do it: a pass that loads cannot repair an image nobody can trust, and
    // letting it clear the flag would hand the next frame a LOAD of unknown contents.
    if (bootstrap)
    {
        d->attachments_invalidated = false;
    }

    auto graph              = ::vsg::RenderGraph::create();
    graph->framebuffer      = d->attachments.framebuffer;
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

core::TargetInstance OffscreenTarget::instance() const noexcept
{
    core::TargetInstance instance;
    instance.desc = core::TargetDesc{ static_cast<int>(d->width), static_cast<int>(d->height), d->shape };
    instance.generation = d->generation;
    // "Built" means LOADABLE, not "the images exist": the plan's Repair(Bootstrap) is what tells a pass to
    // clear an image nobody has written into, and `written` is that fact (see the declaration).
    instance.built                   = d->written;
    instance.attachments_invalidated = d->attachments_invalidated;
    return instance;
}

bool OffscreenTarget::repointBorrowedDepth(const core::FrameTimeline& timeline,
                                           core::RetirementQueue& retirement)
{
    if (d->depth_source == nullptr || d->attachments.render_graph == nullptr)
    {
        return false;  // no borrowed depth, or nothing built to re-point
    }
    const OffscreenTarget& lender = *d->depth_source;
    if (lender.d->attachments.depth_view == nullptr ||
        lender.d->attachments.depth_view == d->attachments.depth_view)
    {
        return false;  // the lender serves the same image this target already names
    }
    if (lender.d->width < d->width || lender.d->height < d->height)
    {
        // A framebuffer attachment must be at least as large as the framebuffer it is attached to
        // (VUID-VkFramebufferCreateInfo-pAttachments-00861), so a lender that shrank BELOW this target cannot
        // back it: re-pointing anyway would be a silent validation error instead of a refusal. False here is
        // the same answer "nothing to do" gives, and the caller keeps what it had - the state is not made
        // worse, and the resize that created it is the one to refuse (VsgExecutor::applyTargetPlans refuses a
        // lender's shrink while a borrower is larger, so a host-driven pair never reaches this).
        return false;
    }

    // ONLY the framebuffer is replaced, and that is the whole point of doing this instead of a resize: the
    // COLOUR attachments are this target's own and keep their contents, so the passes already planned for
    // this frame keep their load-ops (a pass that LOADs still reads what it wrote) while the depth test now
    // reads the depth the lender's own pass writes this frame. Replacing the colour images would need a
    // bootstrap clear, and the plan for this frame was compiled before this call.
    ::vsg::ImageViews views;
    views.reserve(d->attachments.colors.size() + 1U);
    for (const Attachments::Color& color : d->attachments.colors)
    {
        views.push_back(color.view);
    }
    views.push_back(lender.d->attachments.depth_view);

    const ::vsg::ref_ptr<::vsg::Framebuffer> replaced = d->attachments.framebuffer;
    auto framebuffer = ::vsg::Framebuffer::create(d->render_pass, views, d->width, d->height, 1U);
    if (framebuffer == nullptr)
    {
        return false;
    }
    d->attachments.framebuffer                    = framebuffer;
    d->attachments.depth_image                    = lender.d->attachments.depth_image;
    d->attachments.depth_view                     = lender.d->attachments.depth_view;
    // The target's own graph is a marker as much as a graph (its framebuffer is what "built" is read from),
    // and the per-pass graphs are built from the attachments each frame (see passGraph) - so they see the new
    // framebuffer on their own.
    d->attachments.render_graph->framebuffer      = framebuffer;
    ++d->generation;

    // The replaced framebuffer may still be named by a frame in flight (submissions overlap): the same
    // custody a resize uses, so a refusal cannot free it while a command buffer still names it.
    auto custody = std::make_shared<::vsg::ref_ptr<::vsg::Framebuffer>>(std::move(replaced));
    if (!retirement.retire(timeline, [custody]() { *custody = {}; }))
    {
        retirement.noteDeviceWait();
        if (d->device != nullptr)
        {
            vkDeviceWaitIdle(*d->device);
        }
        *custody = {};
    }
    return true;
}

void OffscreenTarget::invalidateAttachments() noexcept
{
    // Not a teardown: the images stay, the FACT about their contents changes. `written` is deliberately left
    // alone - "something was recorded" is still true, and the two facts answer different questions.
    d->attachments_invalidated = true;
}

bool OffscreenTarget::nameImages(const char* label) noexcept
{
    if (d->names_set) {
        return true;
    }
    if (d->device == nullptr || label == nullptr || d->attachments.colors.empty()) {
        return false;
    }
    // EVERY image has to exist before any of them is named: naming one half of a set and then failing would
    // make the flag record something that is not true.
    const std::uint32_t device_id = d->device->deviceID;
    for (const OffscreenTarget::Attachments::Color& color : d->attachments.colors) {
        if (color.image == nullptr || color.image->vk(device_id) == nullptr) {
            return false;
        }
    }
    const bool has_own_depth = d->attachments.depth_image != nullptr && !d->depth_borrowed;
    if (has_own_depth && d->attachments.depth_image->vk(device_id) == nullptr) {
        return false;
    }

    for (std::size_t index = 0; index < d->attachments.colors.size(); ++index) {
        const std::string name = std::string("vine ") + label + " colour " + std::to_string(index);
        (void)detail::nameVulkanObject(
            *d->device, reinterpret_cast<std::uint64_t>(d->attachments.colors[index].image->vk(device_id)),
            VK_OBJECT_TYPE_IMAGE, name.c_str());
    }
    if (has_own_depth) {
        // A BORROWED depth is the lender's image and the lender names it: two targets fighting over one
        // image's name would name it after whichever recorded last.
        const std::string name = std::string("vine ") + label + " depth";
        (void)detail::nameVulkanObject(*d->device,
                                       reinterpret_cast<std::uint64_t>(d->attachments.depth_image->vk(device_id)),
                                       VK_OBJECT_TYPE_IMAGE, name.c_str());
    }
    d->names_set = true;
    return true;
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::capture() const noexcept
{
    return capture(0U);
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::capture(std::uint32_t attachment) const
{
    if (attachment >= d->attachments.colors.size()) {
        return nullptr;
    }
    // Handing the node out is the recording half of a readback: from here on a probe is meaningful (see the
    // declaration). The flag lives in the ATTACHMENT SET, so a resize starts it false - the new buffers hold
    // nothing until a frame copies into them.
    OffscreenTarget::Attachments::Color& color = d->attachments.colors[attachment];
    if (color.capture != nullptr) {
        color.captured = true;
    }
    return color.capture;
}

core::PixelProbe OffscreenTarget::probe() const
{
    return probe(0U);
}

core::PixelProbe OffscreenTarget::probe(std::uint32_t attachment) const
{
    // The same table readbackResult() answers with: "can this be served, and if not why" is one question,
    // and a probe that decided it for itself could disagree with the classification a caller asked for.
    const core::ReadbackResult check =
        readbackResult(core::ReadbackRequest{ core::ReadbackKind::Color, attachment });
    if (!check.ok || attachment >= d->attachments.colors.size()) {
        return core::PixelProbe(0, 0, {});
    }
    const OffscreenTarget::Attachments::Color& color = d->attachments.colors[attachment];
    if (color.mapped == nullptr || color.mapped->data() == nullptr) {
        return core::PixelProbe(0, 0, {});
    }
    const std::size_t byte_count = static_cast<std::size_t>(d->width) * d->height *
                                   core::colorReadbackOf(d->shape.color_formats[attachment]).bytes_per_texel;
    std::vector<std::uint8_t> pixels;
    pixels.resize(byte_count);
    std::memcpy(pixels.data(), color.mapped->data(), byte_count);
    return core::PixelProbe(static_cast<int>(d->width), static_cast<int>(d->height), std::move(pixels));
}

core::ReadbackResult OffscreenTarget::readbackResult(const core::ReadbackRequest& request) const noexcept
{
    core::ReadbackState state;
    state.color_attachments = static_cast<std::uint32_t>(d->attachments.colors.size());
    if (request.kind == core::ReadbackKind::Color && request.attachment < d->shape.color_formats.size())
    {
        state.color_format = d->shape.color_formats[request.attachment];
        state.color_captured = d->attachments.colors[request.attachment].captured;
    }
    if (d->depth_format.has_value())
    {
        state.depth_format   = d->depth_format;
        state.depth_captured = d->attachments.depth_captured;
    }
    return core::readbackOf(state, request);
}

std::uint32_t OffscreenTarget::colorAttachmentCount() const noexcept
{
    return static_cast<std::uint32_t>(d->attachments.colors.size());
}

::vsg::ref_ptr<::vsg::ImageView> OffscreenTarget::colorView(std::uint32_t attachment) const noexcept
{
    if (attachment >= d->attachments.colors.size())
    {
        return {};
    }
    return d->attachments.colors[attachment].view;
}

::vsg::ref_ptr<::vsg::ImageView> OffscreenTarget::depthView() const noexcept
{
    return d->attachments.depth_view;
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::readback() const noexcept
{
    if (!d->attachments.colors.empty()) {
        return capture(0U);
    }
    // A depth-only target IS its depth: that is what a phase asks it about (see captureDepth).
    return captureDepth();
}

core::TargetShape OffscreenTarget::shape() const noexcept
{
    return d->shape;
}

bool OffscreenTarget::hasDepth() const noexcept
{
    return d->attachments.depth_view != nullptr;
}

core::DepthPlan OffscreenTarget::depth() const noexcept
{
    core::DepthFacts facts;
    facts.has_depth = d->attachments.depth_view != nullptr;
    // The host's request is a fact here, not a decision: whether it survives is the core's answer, because
    // it depends on what this frame's passes do with the depth (see core::depthPlan).
    facts.promotion   = d->depth_sampleable;
    facts.borrowed    = d->depth_source != nullptr;
    facts.source      = d->depth_source;
    facts.any_pass_preserves_depth = d->lending != nullptr && *d->lending > 0U;
    return core::depthPlan(facts);
}

const OffscreenTarget* OffscreenTarget::depthSource() const noexcept
{
    return d->depth_source;
}

::vsg::ref_ptr<::vsg::Node> OffscreenTarget::captureDepth() const
{
    if (d->attachments.depth_capture != nullptr) {
        d->attachments.depth_captured = true;
    }
    return d->attachments.depth_capture;
}

core::DepthProbe OffscreenTarget::depthProbe() const
{
    // One classification (see readbackResult) and one decoder (core::decodeDepth), so "why is there no
    // probe" and "what do the numbers mean" cannot drift apart between the entry points.
    const core::ReadbackResult check =
        readbackResult(core::ReadbackRequest{ core::ReadbackKind::Depth, 0U });
    if (!check.ok || d->attachments.depth_mapped == nullptr || d->attachments.depth_mapped->data() == nullptr)
    {
        return core::DepthProbe();
    }
    const core::ReadbackFormat packed = core::depthReadbackOf(d->depth_format.value());
    const std::size_t          texels = static_cast<std::size_t>(d->width) * d->height;
    const auto* bytes = static_cast<const std::byte*>(static_cast<const void*>(d->attachments.depth_mapped->data()));
    std::vector<float> values =
        core::decodeDepth(d->depth_format.value(), std::span<const std::byte>(bytes, texels * packed.bytes_per_texel));
    if (values.size() != texels)
    {
        return core::DepthProbe();  // a copy that is not a whole image is not a picture of anything
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

std::uint64_t OffscreenTarget::generation() const noexcept
{
    return d->generation;
}

OffscreenTarget::Resized OffscreenTarget::resize(std::uint32_t width, std::uint32_t height,
                                                 const core::FrameTimeline& timeline,
                                                 core::RetirementQueue& retirement)
{
    // The PLAN decides, not this function. The wanted shape is the target's own: the attachment formats and the
    // render pass are fixed at create (a different shape is a different target, not a resize), so the arms that
    // can answer here are None (the extent already is what was asked), Repair (an extent of 0: nothing can be
    // built) and ResizeInPlace. Rebuild is handled with it rather than assumed impossible - the two differ in
    // what else has to follow (pipelines), and this function would still replace the extensional objects.
    const core::TargetDesc wanted{ static_cast<int>(width), static_cast<int>(height), d->shape };
    const core::TargetInstance current{ core::TargetDesc{ static_cast<int>(d->width), static_cast<int>(d->height),
                                                          d->shape },
                                        d->generation, /*built*/ d->attachments.render_graph != nullptr,
                                        /*attachments_invalidated*/ false };
    const core::TargetDecision decision = core::planTarget(current, wanted);

    Resized result;
    result.decision   = decision;
    result.generation = d->generation;
    if (decision.action != core::TargetAction::ResizeInPlace && decision.action != core::TargetAction::Rebuild)
    {
        return result;
    }

    // A LEASED pair used to refuse in BOTH directions, and that made it impossible to grow: the lender
    // refused because its borrowers' framebuffers named the image the resize would replace, the borrower
    // refused because its own framebuffer would name an image smaller than itself - so neither ever moved.
    // Measured on the engine's own deferred pipeline: its G-buffer and the composite that borrows its depth
    // were told "resize to the surface" every frame and both stayed at the size they were built at, which
    // put a crop of the picture's top-left corner on screen (the G-buffer previews and the lit window were
    // that crop, stretched).
    //
    // The rule is the caller's ORDER and its follow-up (see VsgExecutor::applyTargetPlans):
    //   * a LENDER may resize. Its borrowers' framebuffers name the image that is being replaced - a
    //     STALE read, never a dangling one (the replaced image is kept alive by the parking window) - and
    //     the caller re-points them at the image the lender now serves, in the same frame. This is the
    //     rule the reference reads as "the borrow points at another image".
    //   * a BORROWER may resize while its lender's CURRENT image covers the new extent. That is what the
    //     refusal was really about: a framebuffer attachment must be at least as large as the framebuffer
    //     it is attached to (VUID-VkFramebufferCreateInfo-pAttachments-00861), and a lender applied first
    //     makes it true in the same frame.
    if (d->depth_source != nullptr)
    {
        const OffscreenTarget& lender = *d->depth_source;
        if (lender.d->attachments.depth_view == nullptr || width > lender.d->width || height > lender.d->height)
        {
            result.refused = true;
            return result;
        }
    }

    // The replacement exists BEFORE what it replaces is touched: a build that fails leaves the target serving
    // the extent it had, with the generation it had ("the same images" stays true), and nothing to park.
    Attachments built;
    if (!buildAttachments(width, height, built))
    {
        return result;
    }

    Attachments previous = std::move(d->attachments);
    d->attachments       = std::move(built);
    d->width             = width;
    d->height            = height;
    // The new images have never been written into, so the target's facts have to say so: a caller that derives
    // "this pass is the first writer" from written() clears rather than loads, which is the only thing an
    // UNDEFINED image accepts (see passGraph).
    d->written = false;
    // The invalidation belonged to the OLD set: these images are new, and "the plan says ResizeInPlace" is
    // already the fact that makes the next writer clear (see core::FrameCompiler's freshAttachments).
    d->attachments_invalidated = false;
    ++d->generation;
    result.replaced   = true;
    result.generation = d->generation;

    // What was replaced may still be named by a frame in flight (submissions overlap), so it does not die here.
    // It is parked through a custody the queue can DROP without destroying anything: retire() refuses by
    // dropping the callback, and a callback that owned the objects by value would free them right there, with
    // no device idle and while a submitted command buffer may still name them.
    auto custody   = std::make_shared<Attachments>(std::move(previous));
    result.parked  = retirement.retire(timeline, [custody]() { *custody = Attachments{}; });
    if (!result.parked)
    {
        // No parking window (a caller that never learned how many frames may be in flight): the objects have
        // no safe window, so they are destroyed only under a COUNTED device idle - the count is what keeps
        // "the frame path never idles the device" checkable.
        retirement.noteDeviceWait();
        if (d->device != nullptr)
        {
            vkDeviceWaitIdle(*d->device);
        }
        *custody = Attachments{};
    }
    return result;
}

OffscreenTarget::Rebuilt OffscreenTarget::rebuild(const TargetLayout& wanted, const core::FrameTimeline& timeline,
                                                  core::RetirementQueue& retirement)
{
    Rebuilt result;
    // Every path that decides "nothing is replaced" has to answer with the generation still in force, and
    // there are five of them: one place to say it (a failure that forgot would look like a replacement).
    const auto give_up = [&]() -> Rebuilt {
        result.generation = d->generation;
        return result;
    };

    // The PLAN decides, like it does for a resize. It is handed the shape this layout describes and the
    // facts this target reports, so the arms that are not Rebuild are answered rather than assumed: a shape
    // that matches (None, a resize, or the load-op repair of a target nobody has written yet) means this
    // call has no structural work - and a shape change outranks those repairs (see core::planTarget).
    const core::TargetShape shape = shapeOf(wanted);
    result.decision               = core::planTarget(
        instance(), core::TargetDesc{ static_cast<int>(wanted.width), static_cast<int>(wanted.height), shape });
    if (result.decision.action != core::TargetAction::Rebuild)
    {
        return give_up();
    }

    // The lease refusal a resize makes applies here for the same reason: every borrower's framebuffer names
    // the LENDER's image, and a target does not know who its borrowers are. A borrowed depth is refused as
    // well - this target's new pass would have to adopt an image whose extent and layout belong to a
    // lender this call was not told about.
    if ((d->lending != nullptr && *d->lending > 0U) || d->depth_source != nullptr)
    {
        result.refused = true;
        return give_up();
    }

    // What the new pass does to its attachments, from the same function create starts a fresh target with:
    // a rebuilt target's images are as fresh as a created target's, so every attachment must clear - and a
    // description that says otherwise (a colour attachment that is loaded, an own depth that is loaded) is
    // refused exactly where create refuses it.
    const core::PassClearPlan plan = core::planClearValues(shape, wanted.clear, /*bootstrap*/ true,
                                                           /*depth_borrowed*/ false);
    for (const core::AttachmentClear& attachment : plan.colors)
    {
        if (attachment.load != core::LoadOp::Clear)
        {
            return give_up();
        }
    }
    if (plan.has_depth && plan.depth.load != core::LoadOp::Clear)
    {
        return give_up();
    }

    // The new pass, under the bootstrap variant's key. The steady depth layout comes from the WANTED shape
    // (this target's own fields still describe the old one until the build below).
    const core::ImageLayout           steady_depth = core::depthFinalLayout(shape, wanted.depth_sampleable);
    const core::LoadOpVariantKey      key =
        core::loadOpVariantOf(plan, core::ImageLayout::ShaderReadOnly, steady_depth, steady_depth);
    ::vsg::ref_ptr<::vsg::RenderPass> pass =
        makeOffscreenRenderPass(d->device, wanted.color_formats, wanted.depth_format, key);
    if (pass == nullptr)
    {
        return give_up();
    }

    // Install the new shape BEFORE the build, because buildAttachments answers from the target's own fields
    // (the shape, the depth format, the clear policy and the pass its framebuffer names) - and put every one
    // of them back when the build fails: a rebuild that failed leaves the target serving the shape it had,
    // exactly like a resize. `buildAttachments` writes only into `built`, so nothing else can have moved.
    const core::TargetShape                 previous_shape      = std::move(d->shape);
    const std::optional<vn::graphics::RenderTarget::DepthFormat> previous_depth = d->depth_format;
    const bool                              previous_sampleable = d->depth_sampleable;
    const core::ClearPolicy                 previous_clear      = d->clear_policy;
    const ::vsg::ref_ptr<::vsg::RenderPass> previous_pass       = d->render_pass;
    d->shape            = shape;
    d->depth_format     = wanted.depth_format;
    d->depth_sampleable = wanted.depth_sampleable;
    d->clear_policy     = wanted.clear;
    d->render_pass      = pass;

    Attachments built;
    bool        built_ok = false;
    try
    {
        built_ok = buildAttachments(wanted.width, wanted.height, built);
    }
    catch (const ::vsg::Exception&)
    {
        // vsg THROWS from a framebuffer the driver refuses (Framebuffer::create), and the promise above -
        // "a build that fails leaves the target as it was" - has to hold for that failure too.
        built_ok = false;
    }
    if (!built_ok)
    {
        d->shape            = std::move(previous_shape);
        d->depth_format     = previous_depth;
        d->depth_sampleable = previous_sampleable;
        d->clear_policy     = previous_clear;
        d->render_pass      = previous_pass;
        return give_up();
    }

    Attachments previous = std::move(d->attachments);
    d->attachments       = std::move(built);
    d->width             = wanted.width;
    d->height            = wanted.height;
    // Fresh images, so the facts move with them: nothing has been recorded yet (the next pass in clears,
    // see written) and the invalidation belonged to the OLD attachments - "the plan said Rebuild" is
    // already the fact that makes the next writer clear (see core::FrameCompiler's freshAttachments).
    d->written                 = false;
    d->attachments_invalidated = false;
    ++d->generation;
    result.replaced   = true;
    result.generation = d->generation;

    // The old shape's load-op variants go with the old attachments: their render passes were built from the
    // OLD formats, and a pass of the new shape must not be handed one of them (the keys carry the load ops,
    // not the formats - that is what makes them reusable within one shape and wrong across two). The key
    // the new pass was built under is the only variant the target serves from here.
    std::vector<Data::PassVariant> previous_variants = std::move(d->variants);
    d->variants.clear();
    d->variants.push_back(Data::PassVariant{ key, pass });

    // What was replaced may still be named by a frame in flight, and for a rebuilt target that is TWO kinds
    // of object: the attachments (images, the framebuffer, the graphs) - and the old RENDER PASSES, which a
    // recorded render pass instance names directly. Parking them together is what keeps both alive for the
    // window a submission may still need them (see the declaration; no window means a COUNTED device idle).
    struct Replaced
    {
        Attachments                    attachments;
        std::vector<Data::PassVariant> passes;
    };
    auto custody  = std::make_shared<Replaced>(Replaced{ std::move(previous), std::move(previous_variants) });
    result.parked = retirement.retire(timeline, [custody]() { *custody = Replaced{}; });
    if (!result.parked)
    {
        retirement.noteDeviceWait();
        if (d->device != nullptr)
        {
            vkDeviceWaitIdle(*d->device);
        }
        *custody = Replaced{};
    }
    return result;
}

VN_VSG_NS_END
