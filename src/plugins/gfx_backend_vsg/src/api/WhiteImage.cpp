#include <vine/vsg/api/WhiteImage.hpp>

#include <cstdint>

#include <vsg/core/Array2D.h>
#include <vsg/core/Array3D.h>
#include <vsg/core/MipmapLayout.h>
#include <vsg/state/Image.h>

V_VSG_NS_BEGIN

struct WhiteImage::Data
{
    ::vsg::ref_ptr<::vsg::Image>     image;
    ::vsg::ref_ptr<::vsg::ImageView> view;
    ::vsg::ref_ptr<::vsg::Sampler>   sampler;
};

WhiteImage::WhiteImage() : d(std::make_unique<Data>())
{
}

std::shared_ptr<WhiteImage> WhiteImage::create()
{
    std::shared_ptr<WhiteImage> white(new WhiteImage());

    // The white texel as DATA - one RGBA8 texel, every channel 0xFF, described the way vsg's transfer reads
    // an image (the same shape api/MaterialImages builds its white cube from).
    constexpr std::uint32_t kBytesPerTexel = 4U;

    auto storage = ::vsg::ubyteArray::create(kBytesPerTexel);
    if (storage == nullptr) {
        return nullptr;
    }
    for (std::uint32_t byte = 0; byte < kBytesPerTexel; ++byte) {
        storage->data()[byte] = 0xFFU;
    }

    ::vsg::Data::Properties properties;
    properties.format        = VK_FORMAT_R8G8B8A8_UNORM;
    properties.mipLevels     = 1U;
    properties.imageViewType = VK_IMAGE_VIEW_TYPE_2D;

    auto layout = ::vsg::MipmapLayout::create(1U);
    if (layout == nullptr) {
        return nullptr;
    }
    layout->at(0U) = ::vsg::uivec4(1U, 1U, 1U, 0U);

    auto texels = ::vsg::uintArray3D::create(storage, 0U, kBytesPerTexel, 1U, 1U, 1U, properties, layout.get());
    if (texels == nullptr) {
        return nullptr;
    }

    // A 1x1 RGBA8 image whose ONLY reader is a sampled descriptor: TRANSFER_DST for the upload and SAMPLED
    // for the reads. The transfer owns the UNDEFINED -> SHADER_READ_ONLY transition (see the file note on
    // why this is an upload and no longer a clear a frame had to record).
    auto image = ::vsg::Image::create();
    if (image == nullptr) {
        return nullptr;
    }
    image->data          = texels;
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
    view->subresourceRange = VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0U, 1U, 0U, 1U };

    auto sampler = ::vsg::Sampler::create();
    if (sampler == nullptr) {
        return nullptr;
    }

    white->d->image   = image;
    white->d->view    = view;
    white->d->sampler = sampler;
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

V_VSG_NS_END
