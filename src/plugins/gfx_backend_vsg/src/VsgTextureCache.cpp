#include <vine/vsg/VsgTextureCache.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>

#include <vsg/core/Array2D.h>
#include <vsg/core/Array3D.h>
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
using detail::anisotropyFor;
using detail::classifyTexture;
using detail::levelExtent;
using detail::textureDataMatchesExtent;
using detail::TextureReject;
using detail::vkFormatFor;

namespace
{

/**
 * @brief Wraps the staged bytes in the vsg array whose ELEMENT is one texel.
 *
 * Two things matter here and neither is the element's type:
 *
 *   - the element WIDTH has to be the texel width. vsg reads `properties.stride` as the size of one element
 *     and multiplies it by texel counts, and over a byte-wide array those products count pixels instead of
 *     bytes.
 *   - the array's DIMENSIONALITY has to match the layer count, because that is where vsg takes its layer
 *     count from. TransferTask derives `arrayLayers` from the image view type AND THE DATA'S DEPTH
 *     (`case VK_IMAGE_VIEW_TYPE_CUBE: arrayLayers = faceDepth;`). A six-layer texture described by a 2D
 *     array therefore reports a depth of 1, gets exactly ONE copy region, and leaves every layer past the
 *     first holding whatever the allocation happened to contain — with no validation error, because the one
 *     region it did copy is perfectly legal. That is the whole reason the layers are declared as a depth.
 *
 * The element is never interpreted as a number, only its width is used, so the mapping just has to pick a
 * type of the right size — and a 3D one whenever there is more than a single layer.
 *
 * @param storage Whole staged chain, in the layout makeMipmapLayout describes.
 * @param layout  Per-level extents and byte offsets (see makeMipmapLayout).
 * @param width   Base level width in pixels.
 * @param height  Base level height in pixels.
 * @param layers  Layer count; also the depth a multi-layer array declares.
 * @param stride  One element's width in bytes (see the contract above) — NOT a row stride.
 * @param properties Format / mip count / view type the array is declared with.
 * @param bytes_per_texel Texel width in bytes.
 * @return The array over @p storage, or null when no vsg type is that wide.
 */
::vsg::ref_ptr<::vsg::Data> makeTexelArray(::vsg::ref_ptr<::vsg::ubyteArray> storage,
                                           ::vsg::ref_ptr<::vsg::MipmapLayout> layout, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t layers, std::uint32_t stride,
                                           const ::vsg::Data::Properties& properties, std::uint32_t bytes_per_texel)
{
    const auto make2d = [&](auto* typed) -> ::vsg::ref_ptr<::vsg::Data> {
        using Array = std::remove_pointer_t<decltype(typed)>;
        return Array::create(storage, 0u, stride, width, height, properties, layout.get());
    };
    const auto make3d = [&](auto* typed) -> ::vsg::ref_ptr<::vsg::Data> {
        using Array = std::remove_pointer_t<decltype(typed)>;
        return Array::create(storage, 0u, stride, width, height, layers, properties, layout.get());
    };

    switch (bytes_per_texel) {
        case 1u:
            return (layers > 1u) ? make3d(static_cast<::vsg::ubyteArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::ubyteArray2D*>(nullptr));
        case 2u:
            return (layers > 1u) ? make3d(static_cast<::vsg::ushortArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::ushortArray2D*>(nullptr));
        case 4u:
            return (layers > 1u) ? make3d(static_cast<::vsg::uintArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::uintArray2D*>(nullptr));
        case 8u:
            return (layers > 1u) ? make3d(static_cast<::vsg::doubleArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::doubleArray2D*>(nullptr));
        case 16u:
            return (layers > 1u) ? make3d(static_cast<::vsg::vec4Array3D*>(nullptr))
                                 : make2d(static_cast<::vsg::uivec4Array2D*>(nullptr));
        default:
            return {};
    }
}

/**
 * @brief Describes every mip level's extent and where it starts inside the staged bytes.
 *
 * ONE entry per level — not per layer. vsg advances this table once per level and reaches the remaining
 * layers of that level itself, by stepping a whole "face" (see the padding note below), so a table with an
 * entry per (level, layer) would make level 1 read level 0's second entry: wrong extents and wrong offsets.
 *
 * The offsets ARE the plain running sum of the levels' byte sizes: vsg's step between the layers of a level
 * is `properties.stride * level_width * level_height`, and the stride the array is constructed with is ONE
 * TEXEL (`bytes_per_texel`, see makeTexelArray). That product is therefore exactly one layer's byte size and
 * the chain is contiguous — which is the only layout the staging buffer can hold: TransferTask sizes it by
 * VALUE COUNT (`bytes_per_texel * valueCount`), so a per-layer step that is larger than a layer runs off the
 * end of it. Declaring the row stride (width * bytes_per_texel) as the element size made the step `width`
 * times too large: the copy regions still stayed inside the IMAGE, so nothing failed loudly, and the single
 * level of a 2-D texture hid it entirely — a 16 MiB minimum staging buffer is what kept the self-test's 8x8
 * faces inside theirs.
 *
 * @param texture Texture whose levels are described.
 * @return One entry per level: texel width, texel height, depth 1, byte offset.
 */
::vsg::ref_ptr<::vsg::MipmapLayout> makeMipmapLayout(const vine::graphics::Texture& texture)
{
    const auto bytes_per_texel = static_cast<std::size_t>(vine::imaging::bytesPerPixel(texture.format()));
    const auto level_count     = static_cast<std::size_t>(texture.mipCount());
    const auto layer_count     = static_cast<std::size_t>(texture.layerCount());
    const auto value_stride    = bytes_per_texel;

    auto layout = ::vsg::MipmapLayout::create(level_count);

    std::size_t offset = 0;
    for (std::size_t level = 0; level < level_count; ++level) {
        const auto width  = levelExtent(texture.width(), level);
        const auto height = levelExtent(texture.height(), level);

        layout->at(level) = ::vsg::uivec4(width, height, 1u, static_cast<std::uint32_t>(offset));
        offset += layer_count * value_stride * static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }

    return layout;
}

/**
 * @brief Builds the vsg image that carries a texture's layers and mip chain.
 *
 * The bytes are staged in MIP-MAJOR order, because that is what vsg's copy regions assume: for one level it
 * reads every layer from consecutive offsets starting at that level's layout entry. The module stores each
 * layer's own chain level after level, so a multi-layer texture has to be interleaved here.
 *
 * Getting that order wrong is SILENT: the total byte count is the same either way, so every copy stays
 * within the image, no validation layer complains, and instead every face samples another face's data. That
 * is the whole reason the cube case is asserted by pixels rather than by "it uploaded without an error".
 *
 * A single layer needs no interleaving at all — the inner loop copies one layer's whole chain — which is
 * what keeps the two-dimensional upload byte-for-byte what it was.
 *
 * @param texture Texture to build the image for.
 * @param format  Vulkan format both the data and the image are declared with.
 * @param reason  Receives why no image could be built (left untouched on success): a texel width no vsg
 *                array type is as wide as (UnsupportedFormat), or pixel data that does not account for
 *                the extent the staging below sizes its slots from (Inconsistent).
 * @return The image, ready to be uploaded by a descriptor bind, or null when the texture cannot be turned
 *         into one (the caller reports @p reason).
 */
::vsg::ref_ptr<::vsg::Image> makeImage(const vine::graphics::Texture& texture, VkFormat format,
                                       TextureReject& reason)
{
    // The staging below sizes each layer's slot from the texture's EXTENT and copies the layer's own bytes
    // into it, so a texture whose data disagrees with that extent writes past the slot (and a chain too
    // large for vsg's 32-bit level offsets wraps them). One rule, shared with classifyTexture so the policy
    // and the guard cannot drift: it is what makes the memcpy below in-bounds by construction.
    if (!textureDataMatchesExtent(texture)) {
        reason = TextureReject::Inconsistent;
        return {};
    }

    const auto width           = static_cast<std::uint32_t>(texture.width());
    const auto height          = static_cast<std::uint32_t>(texture.height());
    const auto mip_levels      = static_cast<std::uint32_t>(texture.mipCount());
    const auto layer_count     = static_cast<std::uint32_t>(texture.layerCount());
    const auto bytes_per_texel = static_cast<std::uint32_t>(vine::imaging::bytesPerPixel(texture.format()));
    const bool is_cube         = (texture.kind() == vine::graphics::Texture::Kind::Cube);

    auto layout = makeMipmapLayout(texture);

    // vsg's step between the layers of one level, which is what the layout's per-level base leaves room for.
    const std::size_t value_stride = bytes_per_texel;
    std::size_t       total        = 0;
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        const auto entry = layout->at(level);
        total = static_cast<std::size_t>(entry.w) + value_stride * static_cast<std::size_t>(entry.x) *
                                                          static_cast<std::size_t>(entry.y) * layer_count;
    }

    auto  storage = ::vsg::ubyteArray::create(static_cast<std::uint32_t>(total));
    auto* staged  = reinterpret_cast<std::uint8_t*>(storage->data());

    // Each layer of each level is copied to the offset vsg will read that layer's copy region from, so the
    // padding cannot drift out of step with what is written.
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        const auto entry      = layout->at(level);
        const auto layer_span = value_stride * static_cast<std::size_t>(entry.x) * static_cast<std::size_t>(entry.y);
        for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
            const auto level_bytes = texture.layer(static_cast<int>(layer))->mipData(static_cast<int>(level));
            std::memcpy(staged + entry.w + static_cast<std::size_t>(layer) * layer_span, level_bytes.data(),
                        level_bytes.size());
        }
    }

    ::vsg::Data::Properties properties;
    properties.format    = format;
    properties.mipLevels = static_cast<std::uint8_t>(mip_levels);
    // properties.stride is not set here: Array2D/Array3D::assign() overwrites it with the stride the array is
    // constructed with, and that IS the `valueSize` vsg's copy regions step by — one TEXEL, so that the step
    // is exactly one layer (see makeMipmapLayout).
    if (is_cube) {
        // The view type is taken from the DATA, not from the ImageView: ImageView's constructors read
        // properties.imageViewType and fall back to a type derived from the image extent whenever it is
        // negative, so assigning to imageView->viewType would simply be overwritten — leaving a six-layer
        // 2D array view that samples nothing.
        properties.imageViewType = VK_IMAGE_VIEW_TYPE_CUBE;
    }

    auto texels = makeTexelArray(storage, layout, width, height, layer_count, bytes_per_texel, properties,
                                 bytes_per_texel);
    if (texels == nullptr) {
        // No vsg array type is as wide as one texel of this layout (see makeTexelArray).
        reason = TextureReject::UnsupportedFormat;
        return {};
    }

    auto image = ::vsg::Image::create();    image->data          = texels;
    image->imageType     = VK_IMAGE_TYPE_2D;
    image->format        = format;
    image->extent        = { width, height, 1u };
    image->mipLevels     = mip_levels;
    image->arrayLayers   = layer_count;
    image->flags         = is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u;
    image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return image;
}

/**
 * @brief Builds the sampler a texture is read through.
 *
 * @param mip_levels      How many levels the image has.
 * @param max_anisotropy  Anisotropy to request, already clamped to what the device allows.
 * @return The sampler for it.
 */
::vsg::ref_ptr<::vsg::Sampler> makeSampler(std::uint32_t mip_levels, float max_anisotropy)
{
    auto sampler = ::vsg::Sampler::create();
    sampler->maxLod = static_cast<float>(mip_levels);
    // A single-level image has nothing to interpolate BETWEEN: leaving the mipmap mode on linear would
    // make the GPU filter across levels that do not exist.
    sampler->mipmapMode = (mip_levels > 1u) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Anisotropy only means anything with a mip chain to choose between, and only where the device enabled
    // the samplerAnisotropy feature — which is why the request comes from the device's own limit rather than
    // from a constant here.
    sampler->anisotropyEnable = (mip_levels > 1u) ? VK_TRUE : VK_FALSE;
    sampler->maxAnisotropy    = (mip_levels > 1u) ? max_anisotropy : 1.0f;
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
    ::vsg::ref_ptr<::vsg::ImageInfo> white_cube;
    // What the device offers (see setMaxAnisotropy); 1 is the safe answer for a cache that was never told.
    float max_anisotropy = 1.0f;
};

VsgTextureCache::VsgTextureCache()
  : d(new Data())
{
}

VsgTextureCache::~VsgTextureCache() = default;

void VsgTextureCache::setMaxAnisotropy(float device_limit) noexcept
{
    d->max_anisotropy = anisotropyFor(device_limit);
}

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

        d->white = ::vsg::ImageInfo::create(makeSampler(1u, d->max_anisotropy), ::vsg::ImageView::create(image),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return d->white;
}

::vsg::ref_ptr<::vsg::ImageInfo> VsgTextureCache::whiteCubeFallback()
{
    if (d->white_cube == nullptr) {
        // Six 1x1 opaque white faces, staged and declared the way makeImage() uploads a real cube: the view
        // type is stated on the DATA (properties.imageViewType — an ImageView's own viewType is overwritten
        // from it), a six-layer Array3D carries the faces, and the mipmap layout names the single level.
        //
        // Built here rather than by driving a CubeMap through makeImage() so that this path cannot fail:
        // every texture the cache accepts is filtered by classifyTexture() first, and a fallback that could
        // itself be refused would leave the cube slot with nothing legal to bind at all.
        constexpr std::uint32_t kFaceCount     = 6u;
        constexpr std::uint32_t kBytesPerTexel = 4u;
        auto                    white_bytes    = ::vsg::ubyteArray::create(kFaceCount * kBytesPerTexel);
        for (std::size_t byte = 0; byte < white_bytes->size(); ++byte) {
            white_bytes->data()[byte] = 0xFFu;
        }

        ::vsg::Data::Properties properties;
        properties.format        = VK_FORMAT_R8G8B8A8_UNORM;
        properties.mipLevels     = 1u;
        properties.imageViewType = VK_IMAGE_VIEW_TYPE_CUBE;

        auto layout    = ::vsg::MipmapLayout::create(1u);
        layout->at(0u) = ::vsg::uivec4(1u, 1u, 1u, 0u);

        // One array element per face: the row stride is one texel, which is what vsg steps the faces of a
        // level by (see the padding note in makeMipmapLayout).
        auto texels = ::vsg::uintArray3D::create(white_bytes, 0u, kBytesPerTexel, 1u, 1u, kFaceCount, properties,
                                                layout.get());

        auto image = ::vsg::Image::create();
        image->data          = texels;
        image->imageType     = VK_IMAGE_TYPE_2D;
        image->format        = VK_FORMAT_R8G8B8A8_UNORM;
        image->extent        = { 1u, 1u, 1u };
        image->mipLevels     = 1u;
        image->arrayLayers   = kFaceCount;
        image->flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        d->white_cube = ::vsg::ImageInfo::create(makeSampler(1u, d->max_anisotropy), ::vsg::ImageView::create(image),
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    return d->white_cube;
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

    const vine::imaging::Image* source = texture->layer(0);
    if (source == nullptr) {
        // classifyTexture() only accepts a complete texture, so this cannot happen for a texture that is
        // not being mutated concurrently; it is checked rather than asserted because a null dereference
        // here would take the whole frame down.
        reason = TextureReject::Incomplete;
        return whiteFallback();
    }

    const VkFormat format    = vkFormatFor(source->format());
    TextureReject  build_reason = reason; // unchanged (Ok) unless makeImage refuses
    auto           vsg_image = makeImage(*texture, format, build_reason);
    if (vsg_image == nullptr) {
        // The two ways an uploadable texture can still fail to become an image, each reported as what it
        // is (see makeImage): a texel width no vsg array type matches, and pixel data that disagrees with
        // the extent the staging sizes its slots from. Uploading a null image would take the frame down,
        // so the fallback is bound instead.
        reason = build_reason;
        return whiteFallback();
    }

    auto info = ::vsg::ImageInfo::create(makeSampler(static_cast<std::uint32_t>(texture->mipCount()),
                                                     d->max_anisotropy),
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
    // This cache is the only retained holder of a texture (it is the session's), so its own
    // entries ARE the retained shares: no other cache waits for it and it waits for none.
    OwnedShareCounts shares;
    for (const auto& entry : d->cache) {
        shares.add(entry.first);
    }
    return eraseAbandoned(d->cache, shares);
}

void VsgTextureCache::clear()
{
    d->cache.clear();
    d->white      = nullptr;
    d->white_cube = nullptr;
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
