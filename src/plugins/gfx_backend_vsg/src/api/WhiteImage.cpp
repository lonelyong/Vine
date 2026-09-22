#include <vine/vsg/api/WhiteImage.hpp>

#include <vsg/commands/ClearImage.h>
#include <vsg/commands/Commands.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/state/Image.h>

V_VSG_NS_BEGIN

namespace
{

/// @brief The whole colour image: one mip, one layer, one aspect.
constexpr VkImageSubresourceRange kWholeImage{ VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U };

}  // namespace

struct WhiteImage::Data
{
    ::vsg::ref_ptr<::vsg::Image>     image;
    ::vsg::ref_ptr<::vsg::ImageView> view;
    ::vsg::ref_ptr<::vsg::Sampler>   sampler;
    ::vsg::ref_ptr<::vsg::Commands>  fill;
};

WhiteImage::WhiteImage() : d(std::make_unique<Data>())
{
}

std::shared_ptr<WhiteImage> WhiteImage::create()
{
    std::shared_ptr<WhiteImage> white(new WhiteImage());

    // A 1x1 RGBA8 image whose ONLY reader is a sampled descriptor, filled by a clear (see the file note):
    // TRANSFER_DST for the clear and SAMPLED for the reads.
    auto image = ::vsg::Image::create();
    if (image == nullptr) {
        return nullptr;
    }
    image->imageType     = VK_IMAGE_TYPE_2D;
    image->format        = VK_FORMAT_R8G8B8A8_UNORM;
    image->extent        = VkExtent3D{ 1U, 1U, 1U };
    image->mipLevels     = 1U;
    image->arrayLayers   = 1U;
    image->samples       = VK_SAMPLE_COUNT_1_BIT;
    image->tiling        = VK_IMAGE_TILING_OPTIMAL;
    image->usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image->sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    auto view = ::vsg::ImageView::create(image, VK_IMAGE_VIEW_TYPE_2D);
    if (view == nullptr) {
        return nullptr;
    }
    view->subresourceRange = kWholeImage;

    auto sampler = ::vsg::Sampler::create();
    if (sampler == nullptr) {
        return nullptr;
    }

    // The fill: discard whatever the image held, clear it to white, and leave it in the layout a sampled
    // descriptor declares (the same SHADER_READ_ONLY a written image ends in). Recording this node every
    // frame is idempotent and costs one 1x1 clear - it is not a render-pass command, so it lives in the
    // frame's command graph, before the content.
    auto fill = ::vsg::Commands::create();
    if (fill == nullptr) {
        return nullptr;
    }
    auto to_clear = ::vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
        kWholeImage);
    auto clear = ::vsg::ClearColorImage::create();
    if (to_clear == nullptr || clear == nullptr) {
        return nullptr;
    }
    clear->image       = image;
    clear->imageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    clear->color       = VkClearColorValue{ { 1.0F, 1.0F, 1.0F, 1.0F } };
    clear->ranges      = ::vsg::ClearColorImage::Ranges{ kWholeImage };

    auto to_sample = ::vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
        kWholeImage);
    if (to_sample == nullptr) {
        return nullptr;
    }

    fill->addChild(::vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                  0, to_clear));
    fill->addChild(clear);
    fill->addChild(::vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                                  0, to_sample));

    white->d->image   = image;
    white->d->view    = view;
    white->d->sampler = sampler;
    white->d->fill    = fill;
    return white;
}

WhiteImage::~WhiteImage() = default;

::vsg::ref_ptr<::vsg::ImageView> WhiteImage::view() const noexcept
{
    return d->view;
}

::vsg::ref_ptr<::vsg::Sampler> WhiteImage::sampler() const noexcept
{
    return d->sampler;
}

::vsg::ref_ptr<::vsg::Node> WhiteImage::fill() const noexcept
{
    return d->fill;
}

V_VSG_NS_END
