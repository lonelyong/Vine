#include <vine/vsg/api/MaterialImages.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vsg/core/Array2D.h>
#include <vsg/core/Array3D.h>
#include <vsg/core/MipmapLayout.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/graphics/Texture.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>

#include <vine/vsg/api/WhiteImage.hpp>

VN_VSG_NS_BEGIN

namespace
{
// The device-free decisions are the backend's own and spelled ONCE (see VsgSceneRules): what a texture has
// to be before it can be uploaded, what one mip level's extent is, which Vulkan format a pixel layout has,
// and what anisotropy a device's limit is worth asking for. A second copy here would be a second truth,
// and it would be the one that drifts.
using detail::anisotropyFor;
using detail::levelExtent;
using detail::TextureReject;
using detail::vkFormatFor;

/** @brief Describes every mip level's extent and where it starts inside the staged bytes.
 *
 * ONE entry per level - not per (level, layer). vsg advances this table once per level and reaches the
 * remaining layers of that level by stepping a whole face, so a table with an entry per layer would make
 * level 1 read level 0's second entry: wrong extents and wrong offsets.
 *
 * The offsets are the running sum of the levels' byte sizes. vsg's step between the layers of one level is
 * `properties.stride * level_width * level_height`, and the stride the array is constructed with is ONE
 * TEXEL (`bytes_per_texel`): that product is exactly one layer's byte size, so the chain is contiguous -
 * the only layout the staging buffer can hold, because the transfer sizes it by VALUE COUNT.
 *
 * @param texture Texture whose levels are described.
 * @return One entry per level: texel width, texel height, depth 1, byte offset.
 */
::vsg::ref_ptr<::vsg::MipmapLayout> makeMipmapLayout(const vn::graphics::Texture& texture)
{
    const auto bytes_per_texel = static_cast<std::size_t>(vn::imaging::bytesPerPixel(texture.format()));
    const auto level_count     = static_cast<std::size_t>(texture.mipCount());
    const auto layer_count     = static_cast<std::size_t>(texture.layerCount());

    auto layout = ::vsg::MipmapLayout::create(level_count);

    std::size_t offset = 0U;
    for (std::size_t level = 0; level < level_count; ++level) {
        const auto width  = levelExtent(texture.width(), level);
        const auto height = levelExtent(texture.height(), level);

        layout->at(level) =
            ::vsg::uivec4(width, height, 1U, static_cast<std::uint32_t>(offset));
        offset += layer_count * bytes_per_texel * static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    }
    return layout;
}

/** @brief Wraps the staged bytes in the vsg array whose ELEMENT is one texel.
 *
 * Two things matter here and neither is the element's type:
 *
 *  * the element WIDTH has to be the texel width. vsg reads `properties.stride` as the size of one element
 *    and multiplies it by texel counts, and over a byte-wide array those products count pixels instead of
 *    bytes;
 *  * the array's DIMENSIONALITY has to match the layer count, because that is where vsg takes its layer
 *    count from: TransferTask derives `arrayLayers` from the image view type AND THE DATA'S DEPTH for a
 *    cube, so a six-layer texture described by a 2D array gets exactly one copy region and leaves every
 *    layer past the first holding whatever the allocation happened to contain - with no validation error,
 *    because the one region it did copy is perfectly legal.
 *
 * The element is never interpreted as a number, only its width is used, so the mapping just has to pick a
 * type of the right size - and a 3D one whenever there is more than a single layer.
 *
 * @param storage Whole staged chain, in the layout @p layout describes.
 * @param layout  Per-level extents and byte offsets (see makeMipmapLayout).
 * @param width   Base level width in pixels.
 * @param height  Base level height in pixels.
 * @param layers  Layer count; also the depth a multi-layer array declares.
 * @param properties Format / mip count / view type the array is declared with.
 * @param bytes_per_texel Texel width in bytes.
 * @return The array over @p storage, or null when no vsg type is that wide.
 */
::vsg::ref_ptr<::vsg::Data> makeTexelArray(::vsg::ref_ptr<::vsg::ubyteArray> storage,
                                           ::vsg::ref_ptr<::vsg::MipmapLayout> layout, std::uint32_t width,
                                           std::uint32_t height, std::uint32_t layers,
                                           const ::vsg::Data::Properties& properties, std::uint32_t bytes_per_texel)
{
    const std::uint32_t stride = bytes_per_texel;

    const auto make2d = [&](auto* typed) -> ::vsg::ref_ptr<::vsg::Data> {
        using Array = std::remove_pointer_t<decltype(typed)>;
        return Array::create(storage, 0U, stride, width, height, properties, layout.get());
    };
    const auto make3d = [&](auto* typed) -> ::vsg::ref_ptr<::vsg::Data> {
        using Array = std::remove_pointer_t<decltype(typed)>;
        return Array::create(storage, 0U, stride, width, height, layers, properties, layout.get());
    };

    switch (bytes_per_texel) {
        case 1U:
            return (layers > 1U) ? make3d(static_cast<::vsg::ubyteArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::ubyteArray2D*>(nullptr));
        case 2U:
            return (layers > 1U) ? make3d(static_cast<::vsg::ushortArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::ushortArray2D*>(nullptr));
        case 4U:
            return (layers > 1U) ? make3d(static_cast<::vsg::uintArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::uintArray2D*>(nullptr));
        case 8U:
            return (layers > 1U) ? make3d(static_cast<::vsg::doubleArray3D*>(nullptr))
                                 : make2d(static_cast<::vsg::doubleArray2D*>(nullptr));
        case 16U:
            return (layers > 1U) ? make3d(static_cast<::vsg::vec4Array3D*>(nullptr))
                                 : make2d(static_cast<::vsg::uivec4Array2D*>(nullptr));
        default:
            return {};
    }
}

/** @brief Builds the vsg image that carries a texture's layers and mip chain.
 *
 * The bytes are staged in MIP-MAJOR order, because that is what vsg's copy regions assume: for one level it
 * reads every layer from consecutive offsets starting at that level's layout entry. The engine stores each
 * layer's own chain level after level, so a multi-layer texture has to be interleaved here. Getting that
 * order wrong is SILENT - the total byte count is the same either way, every copy stays inside the image,
 * and no validation layer complains - and instead every face samples another face's data.
 *
 * @param texture Texture to build the image for.
 * @param format  Vulkan format both the data and the image are declared with.
 * @param reason  Receives why no image could be built (left untouched on success): a texel width no vsg
 *                array type is as wide as (`UnsupportedFormat`), or pixel data that does not account for
 *                the extent the staging below sizes its slots from (`Inconsistent`).
 * @return The image, ready for the transfer step to upload, or null (the caller reports @p reason).
 */
::vsg::ref_ptr<::vsg::Image> makeImage(const vn::graphics::Texture& texture, VkFormat format,
                                       TextureReject& reason)
{
    // The staging below sizes each layer's slot from the texture's EXTENT and copies the layer's own bytes
    // into it, so a texture whose data disagrees with that extent would write past the slot (and a chain
    // too large for vsg's 32-bit level offsets would wrap them). One rule, shared with classifyTexture so
    // the policy and this guard cannot drift: it is what makes the memcpy below in-bounds by construction.
    if (!detail::textureDataMatchesExtent(texture)) {
        reason = TextureReject::Inconsistent;
        return {};
    }

    const auto width           = static_cast<std::uint32_t>(texture.width());
    const auto height          = static_cast<std::uint32_t>(texture.height());
    const auto mip_levels      = static_cast<std::uint32_t>(texture.mipCount());
    const auto layer_count     = static_cast<std::uint32_t>(texture.layerCount());
    const auto bytes_per_texel  = static_cast<std::uint32_t>(vn::imaging::bytesPerPixel(texture.format()));
    const bool is_cube          = texture.kind() == vn::graphics::Texture::Kind::Cube;

    auto layout = makeMipmapLayout(texture);

    std::size_t total = 0U;
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        const auto entry = layout->at(level);
        total = static_cast<std::size_t>(entry.w) +
                static_cast<std::size_t>(bytes_per_texel) * static_cast<std::size_t>(entry.x) *
                    static_cast<std::size_t>(entry.y) * static_cast<std::size_t>(layer_count);
    }

    auto  storage = ::vsg::ubyteArray::create(static_cast<std::uint32_t>(total));
    auto* staged  = storage->data();

    // Each layer of each level is copied to the offset the layout names for that level, so the padding
    // cannot drift out of step with what is written.
    for (std::uint32_t level = 0; level < mip_levels; ++level) {
        const auto entry      = layout->at(level);
        const auto layer_span = static_cast<std::size_t>(bytes_per_texel) * static_cast<std::size_t>(entry.x) *
                                static_cast<std::size_t>(entry.y);
        for (std::uint32_t layer = 0; layer < layer_count; ++layer) {
            const auto level_bytes =
                texture.layer(static_cast<int>(layer))->mipData(static_cast<int>(level));
            std::memcpy(staged + entry.w + static_cast<std::size_t>(layer) * layer_span, level_bytes.data(),
                        level_bytes.size());
        }
    }

    ::vsg::Data::Properties properties;
    properties.format    = format;
    properties.mipLevels = static_cast<std::uint8_t>(mip_levels);
    // properties.stride is NOT set here: Array2D/Array3D::assign() overwrites it with the stride the array
    // is constructed with, and that IS the size the copy regions step by - one texel (see makeTexelArray).
    if (is_cube) {
        // The view type is taken from the DATA, not from the ImageView: ImageView's constructors read
        // properties.imageViewType and derive a type from the image's extent whenever it is negative, so
        // assigning to the view afterwards would simply be overwritten - leaving a six-layer 2D-array
        // view that samples nothing.
        properties.imageViewType = VK_IMAGE_VIEW_TYPE_CUBE;
    }

    auto texels = makeTexelArray(storage, layout, width, height, layer_count, properties, bytes_per_texel);
    if (texels == nullptr) {
        reason = TextureReject::UnsupportedFormat;
        return {};
    }

    auto image = ::vsg::Image::create();
    image->data          = texels;
    image->imageType     = VK_IMAGE_TYPE_2D;
    image->format        = format;
    image->extent        = { width, height, 1U };
    image->mipLevels     = mip_levels;
    image->arrayLayers   = layer_count;
    image->flags         = is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0U;
    image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return image;
}

/** @brief Builds the sampler a texture is read through.
 *
 * @param mip_levels     How many levels the image has.
 * @param max_anisotropy Anisotropy to request, already clamped to what the device allows.
 * @return The sampler for it.
 */
::vsg::ref_ptr<::vsg::Sampler> makeSampler(std::uint32_t mip_levels, float max_anisotropy)
{
    auto sampler       = ::vsg::Sampler::create();
    sampler->maxLod    = static_cast<float>(mip_levels);
    // A single-level image has nothing to interpolate BETWEEN: leaving the mipmap mode on linear would
    // make the GPU filter across levels that do not exist.
    sampler->mipmapMode = (mip_levels > 1U) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Anisotropy only means anything with a mip chain to choose between, and only where the device enabled
    // the samplerAnisotropy feature - which is why the request comes from the device's own limit.
    sampler->anisotropyEnable = (mip_levels > 1U) ? VK_TRUE : VK_FALSE;
    sampler->maxAnisotropy    = (mip_levels > 1U) ? max_anisotropy : 1.0F;
    return sampler;
}

/** @brief Builds the 1x1 white cube a cube slot with no usable texture is sampled through.
 *
 * Six faces, one texel each - built through the SAME path a real texture takes (a data-backed image the
 * transfer uploads), so the shader cannot tell a fallback from a texture. A cube sampler cannot bind the
 * 2D fallback: a 2D view where the text declares `samplerCube` is an invalid descriptor, not an
 * untextured draw.
 *
 * @param max_anisotropy Anisotropy to request, already clamped to what the device allows.
 * @return The image and sampler, or a null view when the objects could not be created.
 */
SamplerImage makeWhiteCube(float max_anisotropy)
{
    constexpr std::uint32_t kFaces        = 6U;
    constexpr std::uint32_t kBytesPerTexel = 4U;

    auto storage = ::vsg::ubyteArray::create(kFaces * kBytesPerTexel);
    for (std::uint32_t byte = 0; byte < kFaces * kBytesPerTexel; ++byte) {
        storage->data()[byte] = 0xFFU;
    }

    ::vsg::Data::Properties properties;
    properties.format         = VK_FORMAT_R8G8B8A8_UNORM;
    properties.mipLevels      = 1U;
    properties.imageViewType  = VK_IMAGE_VIEW_TYPE_CUBE;

    auto layout    = ::vsg::MipmapLayout::create(1U);
    layout->at(0U) = ::vsg::uivec4(1U, 1U, 1U, 0U);

    auto texels = ::vsg::uintArray3D::create(storage, 0U, kBytesPerTexel, 1U, 1U, kFaces, properties,
                                             layout.get());
    if (texels == nullptr) {
        return SamplerImage{};
    }

    auto image = ::vsg::Image::create();
    if (image == nullptr) {
        return SamplerImage{};
    }
    image->data          = texels;
    image->imageType     = VK_IMAGE_TYPE_2D;
    image->format        = VK_FORMAT_R8G8B8A8_UNORM;
    image->extent        = { 1U, 1U, 1U };
    image->mipLevels     = 1U;
    image->arrayLayers   = kFaces;
    image->flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image->usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    SamplerImage white_cube;
    white_cube.view    = ::vsg::ImageView::create(image);
    white_cube.sampler = makeSampler(1U, max_anisotropy);
    return white_cube;
}

}  // namespace

/**
 * @brief The cache's retained state.
 *
 * An entry owns the texture it is keyed by (see the class note) and remembers the revision its images were
 * built from, and `clock` orders insertions so a trim evicts the oldest rather than an arbitrary one.
 */
struct MaterialImages::Data
{
    /** @brief One texture's retained images, plus the content revision they were built from. */
    struct Entry
    {
        vn::intrusive_ptr<const vn::graphics::Texture> owner;     ///< The key object, held (see the note).
        SamplerImage                                       images;    ///< What a declared set binds.
        std::uint64_t                                      revision{0};
        std::uint64_t                                      stamp{0};  ///< Insertion order (see the trim).
    };

    using Map = std::unordered_map<const vn::graphics::Texture*, Entry>;

    Map                            cache;
    std::uint64_t                  clock{0};
    std::shared_ptr<WhiteImage>    white;
    SamplerImage                   white_cube;
    // What the device offers (see setMaxAnisotropy); 1 is the safe answer for a cache that was never told.
    float                          max_anisotropy = 1.0F;
};

MaterialImages::MaterialImages() : d(new Data())
{
}

MaterialImages::~MaterialImages() = default;

std::shared_ptr<MaterialImages> MaterialImages::create()
{
    std::shared_ptr<MaterialImages> cache(new MaterialImages());
    if (cache->d == nullptr) {
        return nullptr;
    }
    return cache;
}

void MaterialImages::setMaxAnisotropy(float device_limit) noexcept
{
    d->max_anisotropy = anisotropyFor(device_limit);
}

float MaterialImages::maxAnisotropy() const noexcept
{
    return d->max_anisotropy;
}

SamplerImage MaterialImages::white() const noexcept
{
    // Lazily, and through the shared WhiteImage: a scene with no textured material pays nothing, and the
    // fallback is the same object every caller binds (see api/WhiteImage for why it is a clear and not an
    // upload).
    if (d->white == nullptr) {
        d->white = WhiteImage::create();
    }
    if (d->white == nullptr) {
        return SamplerImage{};
    }
    return SamplerImage{ d->white->view(), d->white->sampler() };
}

SamplerImage MaterialImages::whiteCube() const noexcept
{
    if (d->white_cube.view == nullptr) {
        d->white_cube = makeWhiteCube(d->max_anisotropy);
    }
    return d->white_cube;
}

SamplerImage MaterialImages::acquire(vn::raw_ptr<const vn::graphics::Texture> texture,
                                     detail::TextureReject& reason)
{
    reason = detail::classifyTexture(texture);
    if (reason != TextureReject::Ok) {
        // The fallback follows the texture's KIND when there is a texture to ask: a cube slot with an
        // unusable cube would otherwise be bound a 2D view, which is an invalid descriptor rather than an
        // untextured draw. With no texture at all the cache cannot know what the slot samples, so the 2D
        // fallback is answered and a caller whose text declares `samplerCube` asks for whiteCube() - it is
        // the caller that knows its program's declaration.
        const bool cube = texture != nullptr && texture->kind() == vn::graphics::Texture::Kind::Cube;
        return cube ? whiteCube() : white();
    }

    // Held before the map is touched: replacing an entry can drop the last other reference to the texture,
    // and the raw pointer this call was handed would then dangle mid-function.
    vn::intrusive_ptr<const vn::graphics::Texture> owner(texture);
    const std::uint64_t                               revision = texture->revision();

    const auto found = d->cache.find(texture);
    if (found != d->cache.end() && found->second.revision == revision) {
        return found->second.images;
    }

    const VkFormat format = vkFormatFor(texture->format());
    if (format == VK_FORMAT_UNDEFINED) {
        // classifyTexture() answers this already; checked rather than assumed, because the image below
        // would otherwise declare a format nothing can sample.
        reason = TextureReject::UnsupportedFormat;
        return white();
    }

    TextureReject build_reason;  // left at Ok unless the build refuses
    auto          image = makeImage(*texture, format, build_reason);
    if (image == nullptr) {
        reason = build_reason;
        return white();
    }

    SamplerImage images;
    images.view    = ::vsg::ImageView::create(image);
    images.sampler = makeSampler(static_cast<std::uint32_t>(texture->mipCount()), d->max_anisotropy);
    if (images.view == nullptr || images.sampler == nullptr) {
        reason = TextureReject::Inconsistent;  // creation refused: the caller reports it, nothing is cached
        return white();
    }

    const std::uint64_t stamp = ++d->clock;
    d->cache.insert_or_assign(texture, Data::Entry{ std::move(owner), images, revision, stamp });

    // A trimmed entry may have been the last owner of its texture AND of the images it referenced; a
    // texture the scene still uses simply rebuilds on its next acquire, which is what bounds the cache.
    while (d->cache.size() > kMaxEntries) {
        const auto oldest =
            std::min_element(d->cache.begin(), d->cache.end(), [](const auto& left, const auto& right) {
                return left.second.stamp < right.second.stamp;
            });
        if (oldest == d->cache.end()) {
            break;
        }
        d->cache.erase(oldest);
    }

    return images;
}

std::size_t MaterialImages::count() const noexcept
{
    return d->cache.size();
}

bool MaterialImages::has(vn::raw_ptr<const vn::graphics::Texture> texture) const noexcept
{
    return d->cache.find(texture) != d->cache.end();
}

std::size_t MaterialImages::releaseAbandoned()
{
    // This cache is the only retained holder of a texture in this path, so its own entries ARE the
    // retained shares: an entry whose texture has no owner left but the entry itself is one nothing can
    // look up again. (The L2 counts shares across its caches because several of them retain the same
    // object; a second retainer of textures here would bring that count with it.)
    std::size_t released = 0;
    for (auto it = d->cache.begin(); it != d->cache.end();) {
        if (it->second.owner == nullptr || it->second.owner->useCount() <= 1UL) {
            it = d->cache.erase(it);
            ++released;
            continue;
        }
        ++it;
    }
    return released;
}

void MaterialImages::clear()
{
    d->cache.clear();
    d->white.reset();
    d->white_cube = SamplerImage{};
}

VN_VSG_NS_END
