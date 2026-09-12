#include <vine/vsg/VsgTextureCache.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>

#include <vsg/core/Array2D.h>
#include <vsg/core/MipmapLayout.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/vsg/OwnedCache.hpp>

V_VSG_NS_BEGIN

// The retained-cache machinery (OwnedCacheEntry and friends) lives in this namespace; the device-free
// texture rules are under detail, so those are the ones that need pulling in.
using detail::classifyTexture;
using detail::TextureReject;
using detail::vkFormatFor;

namespace
{

/**
 * @brief Wraps a mip chain in the vsg 2D array whose ELEMENT is one texel.
 *
 * The element width has to be the texel width, not one byte, because vsg sizes the staging buffer as
 * `valueCount() * valueSize()` (see `Data::dataSize()`): over a ubyte array that product counts pixels,
 * so an RGBA8 chain would stage a quarter of itself. The element is never interpreted as a number — only
 * its size matters — so the mapping just has to pick a type of the right width.
 *
 * @param storage Whole chain, in the module's compact layout.
 * @param layout  Per-level texel extents and byte offsets (see makeMipmapLayout).
 * @param width   Base level width in pixels.
 * @param height  Base level height in pixels.
 * @param stride  Base level row stride in bytes.
 * @param properties Format / mip count / stride the array is declared with.
 * @param bytes_per_texel Texel width in bytes.
 * @return The array over @p storage, or null when no vsg type is that wide.
 */
::vsg::ref_ptr<::vsg::Data> makeTexelArray(::vsg::ref_ptr<::vsg::ubyteArray> storage,
                                           ::vsg::ref_ptr<::vsg::MipmapLayout> layout, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t stride,
                                           const ::vsg::Data::Properties& properties, std::uint32_t bytes_per_texel)
{
    const auto make = [&](auto* typed) -> ::vsg::ref_ptr<::vsg::Data> {
        using Array = std::remove_pointer_t<decltype(typed)>;
        return Array::create(storage, 0u, stride, width, height, properties, layout.get());
    };

    switch (bytes_per_texel) {
        case 1u:
            return make(static_cast<::vsg::ubyteArray2D*>(nullptr));
        case 2u:
            return make(static_cast<::vsg::ushortArray2D*>(nullptr));
        case 4u:
            return make(static_cast<::vsg::uintArray2D*>(nullptr));
        case 8u:
            return make(static_cast<::vsg::doubleArray2D*>(nullptr));
        case 16u:
            return make(static_cast<::vsg::uivec4Array2D*>(nullptr));
        default:
            return {};
    }
}

/**
 * @brief Describes every mip level's extent and where it starts inside the chain.
 *
 * Attaching this is what makes the upload exact: with a layout vsg takes each copy region's extent AND
 * byte offset straight from these entries. Without one it derives them itself, and then the only lever is
 * `properties.blockWidth` — which multiplies the IMAGE's width to get the region width, so it is for
 * block-compressed formats and cannot be used to describe a byte-packed chain.
 *
 * @param source Chain to describe.
 * @return One entry per level: texel width, texel height, depth 1, byte offset.
 */
::vsg::ref_ptr<::vsg::MipmapLayout> makeMipmapLayout(const vine::imaging::Image& source)
{
    const auto bytes_per_texel = static_cast<std::size_t>(vine::imaging::bytesPerPixel(source.format()));
    const auto level_count     = static_cast<std::size_t>(source.mipCount());

    auto layout = ::vsg::MipmapLayout::create(level_count);

    std::size_t offset = 0;
    for (std::size_t level = 0; level < level_count; ++level) {
        const auto width  = static_cast<std::uint32_t>(source.mipWidth(static_cast<int>(level)));
        const auto height = static_cast<std::uint32_t>(source.mipHeight(static_cast<int>(level)));

        layout->at(level) = ::vsg::uivec4(width, height, 1u, static_cast<std::uint32_t>(offset));
        offset += static_cast<std::size_t>(width) * height * bytes_per_texel;
    }

    return layout;
}

/**
 * @brief Builds the vsg image that carries a mip chain.
 *
 * The chain is copied ONCE, as a contiguous block, because that is already its layout: the module stores
 * each level packed, at the standard halved extents, which is what the MipmapLayout below records.
 *
 * @param source Base-level pixels plus the whole chain, in the module's compact layout.
 * @param format Vulkan format both the data and the image are declared with.
 * @return The image, ready to be uploaded by a descriptor bind, or null when the texel width has no vsg
 *         array type (the caller reports it).
 */
::vsg::ref_ptr<::vsg::Image> makeImage(const vine::imaging::Image& source, VkFormat format)
{
    const auto width           = static_cast<std::uint32_t>(source.width());
    const auto height          = static_cast<std::uint32_t>(source.height());
    const auto mip_levels      = static_cast<std::uint32_t>(source.mipCount());
    const auto bytes_per_texel = static_cast<std::uint32_t>(vine::imaging::bytesPerPixel(source.format()));
    const auto chain_bytes     = static_cast<std::uint32_t>(source.totalByteSize());

    auto storage = ::vsg::ubyteArray::create(chain_bytes);
    std::memcpy(storage->data(), source.mipData(0).data(), chain_bytes);

    ::vsg::Data::Properties properties;
    properties.format    = format;
    properties.mipLevels = static_cast<std::uint8_t>(mip_levels);
    properties.stride    = width * bytes_per_texel;

    auto layout = makeMipmapLayout(source);
    auto texels = makeTexelArray(storage, layout, width, height, properties.stride, properties, bytes_per_texel);
    if (texels == nullptr) {
        return {};
    }

    auto image = ::vsg::Image::create();
    image->data          = texels;
    image->imageType     = VK_IMAGE_TYPE_2D;
    image->format        = format;
    image->extent        = { width, height, 1u };
    image->mipLevels     = mip_levels;
    image->arrayLayers   = 1u;
    image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return image;
}

/**
 * @brief Builds the sampler a texture is read through.
 *
 * @param mip_levels How many levels the image has.
 * @return The sampler for it.
 */
::vsg::ref_ptr<::vsg::Sampler> makeSampler(std::uint32_t mip_levels)
{
    auto sampler = ::vsg::Sampler::create();
    sampler->maxLod = static_cast<float>(mip_levels);
    // A single-level image has nothing to interpolate BETWEEN: leaving the mipmap mode on linear would
    // make the GPU filter across levels that do not exist.
    sampler->mipmapMode = (mip_levels > 1u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return sampler;
}

} // namespace

/**
 * @brief The cache's retained state.
 *
 * An entry owns the texture it is keyed by (see the header), and `clock` orders insertions so a trim
 * evicts the oldest rather than an arbitrary one.
 */
struct VsgTextureCache::Data
{
    /**
     * @brief The retained resources for one texture, plus the content revision they were built from.
     *
     * The revision is what makes a re-filled texture re-upload: the address is the same, so without it
     * the entry would keep serving the image built from the OLD pixels.
     */
    struct Resource
    {
        ::vsg::ref_ptr<::vsg::ImageInfo> info;
        std::uint64_t                    revision = 0;
    };

    using Entry = OwnedCacheEntry<vine::graphics::Texture, Resource>;
    using Map   = std::unordered_map<vine::raw_ptr<const vine::graphics::Texture>, Entry>;

    Map                              cache;
    InsertionClock                   clock;
    ::vsg::ref_ptr<::vsg::ImageInfo> white;
};

VsgTextureCache::VsgTextureCache()
  : d(new Data())
{
}

VsgTextureCache::~VsgTextureCache() = default;

::vsg::ref_ptr<::vsg::ImageInfo> VsgTextureCache::whiteFallback()
{
    if (d->white == nullptr) {
        // A 1x1 opaque white texel, built through the same path a real texture takes so the shader cannot
        // tell them apart — that is the point of having a fallback instead of a branch.
        auto white_bytes = ::vsg::ubyteArray::create(4u);
        white_bytes->data()[0] = 0xFFu;
        white_bytes->data()[1] = 0xFFu;
        white_bytes->data()[2] = 0xFFu;
        white_bytes->data()[3] = 0xFFu;

        ::vsg::Data::Properties properties;
        properties.format    = VK_FORMAT_R8G8B8A8_UNORM;
        properties.mipLevels = 1u;
        properties.stride    = 4u;

        // Declared the way makeImage() declares a real chain: a texel-wide element type plus a layout
        // naming the level's extent. Setting blockWidth here instead would multiply the IMAGE's width by 4
        // to size the copy region — a region four times wider than a 1x1 image, which is a validation error.
        auto layout = ::vsg::MipmapLayout::create(1u);
        layout->at(0u) = ::vsg::uivec4(1u, 1u, 1u, 0u);

        auto texels = ::vsg::uintArray2D::create(white_bytes, 0u, 4u, 1u, 1u, properties, layout.get());

        auto image = ::vsg::Image::create();
        image->data          = texels;
        image->imageType     = VK_IMAGE_TYPE_2D;
        image->format        = VK_FORMAT_R8G8B8A8_UNORM;
        image->extent        = { 1u, 1u, 1u };
        image->mipLevels     = 1u;
        image->arrayLayers   = 1u;
        image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        d->white = ::vsg::ImageInfo::create(makeSampler(1u), ::vsg::ImageView::create(image),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return d->white;
}

::vsg::ref_ptr<::vsg::ImageInfo> VsgTextureCache::getOrCreate(
    vine::raw_ptr<const vine::graphics::Texture> texture, TextureReject& reason)
{
    reason = classifyTexture(texture);
    if (reason != TextureReject::Ok) {
        return whiteFallback();
    }

    // Held before the map is touched: replacing an entry can drop the last other reference to the texture,
    // and the raw pointer this call was handed would then dangle mid-function.
    vine::intrusive_ptr<const vine::graphics::Texture> owner(texture);
    const std::uint64_t                                revision = texture->revision();

    const auto it = d->cache.find(texture);
    if (it != d->cache.end() && it->second.payload().revision == revision) {
        return it->second.payload().info;
    }

    const vine::imaging::Image* source = texture->source(0);
    if (source == nullptr) {
        // classifyTexture() only accepts a complete texture, so this cannot happen for a texture that is
        // not being mutated concurrently; it is checked rather than asserted because a null dereference
        // here would take the whole frame down.
        reason = TextureReject::Incomplete;
        return whiteFallback();
    }

    const VkFormat format    = vkFormatFor(source->format());
    auto           vsg_image = makeImage(*source, format);
    if (vsg_image == nullptr) {
        // makeImage() reports this when no vsg array type is as wide as the texel. Every 3-byte format is
        // already turned away by vkFormatFor(), so nothing classifyTexture() accepts reaches here; it is
        // checked rather than asserted because uploading a null image would take the whole frame down.
        reason = TextureReject::UnsupportedFormat;
        return whiteFallback();
    }

    auto info = ::vsg::ImageInfo::create(makeSampler(static_cast<std::uint32_t>(source->mipCount())),
                                         ::vsg::ImageView::create(vsg_image),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // The entry owns the texture: the address is the key, and an entry that did not hold it could outlive
    // a destroyed texture and then serve its GPU image to a new texture allocated at the same address.
    d->cache.insert_or_assign(texture, Data::Entry(std::move(owner), Data::Resource{ info, revision },
                                                   d->clock.tick()));
    // A trimmed entry may have been the last owner of its texture AND of the image it referenced; a
    // texture the scene still uses simply rebuilds on its next draw, which is what bounds the cache.
    trimToCapacity(d->cache, kMaxEntries);

    return info;
}

std::size_t VsgTextureCache::releaseAbandoned()
{
    return eraseAbandoned(d->cache);
}

void VsgTextureCache::clear()
{
    d->cache.clear();
    d->white = nullptr;
}

std::size_t VsgTextureCache::count() const
{
    return d->cache.size();
}

bool VsgTextureCache::has(vine::raw_ptr<const vine::graphics::Texture> texture) const
{
    return d->cache.find(texture) != d->cache.end();
}

V_VSG_NS_END
