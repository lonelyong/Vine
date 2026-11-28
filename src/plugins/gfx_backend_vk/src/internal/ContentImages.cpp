#include <vine/backend_vk/internal/ContentImages.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <volk.h>

#include <vine/raw_ptr.hpp>
#include <vine/imaging/Image.hpp>
#include <vine/imaging/PixelFormat.hpp>

#include <vine/backend_vk/internal/BarrierPlan.hpp>

VN_BACKEND_VK_NS_BEGIN

namespace
{

/**
 * @brief Gets the Vulkan format a texture's pixel layout uploads as.
 *
 * @details
 * - The sRGB spellings stay sRGB: the sampler decodes to linear on every read, which is the engine's colour-space
 *   contract (see PixelFormat). Declaring an sRGB image UNORM would sample it a gamma too bright.
 * - Depth formats have no sampled-colour use here, so they answer `VK_FORMAT_UNDEFINED`: a depth image is a pass
 *   input (an `ImageRef`), never a material texture.
 *
 * @param format The texture's pixel format.
 * @return The Vulkan format, or `VK_FORMAT_UNDEFINED`.
 */
[[nodiscard]] VkFormat formatOf(vn::imaging::PixelFormat format) noexcept
{
    using vn::imaging::PixelFormat;
    switch (format) {
    case PixelFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
    case PixelFormat::R8Srgb: return VK_FORMAT_R8_SRGB;
    case PixelFormat::Rg8Unorm: return VK_FORMAT_R8G8_UNORM;
    case PixelFormat::Rgb8Unorm: return VK_FORMAT_R8G8B8_UNORM;
    case PixelFormat::Rgb8Srgb: return VK_FORMAT_R8G8B8_SRGB;
    case PixelFormat::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::Rgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case PixelFormat::Bgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
    case PixelFormat::Bgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
    case PixelFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
    case PixelFormat::Rg16Float: return VK_FORMAT_R16G16_SFLOAT;
    case PixelFormat::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
    case PixelFormat::Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
    case PixelFormat::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case PixelFormat::D16Unorm:
    case PixelFormat::D24UnormS8Uint:
    case PixelFormat::D32Float:
    case PixelFormat::Unknown: return VK_FORMAT_UNDEFINED;
    }
    return VK_FORMAT_UNDEFINED;
}

/** @brief The Vulkan filter for an SDK filter. */
[[nodiscard]] VkFilter filterOf(vn::graphics::Sampler::Filter filter) noexcept
{
    return filter == vn::graphics::Sampler::Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

/** @brief The Vulkan mip mode for an SDK mip mode. */
[[nodiscard]] VkSamplerMipmapMode mipmapModeOf(vn::graphics::Sampler::MipmapMode mode) noexcept
{
    return mode == vn::graphics::Sampler::MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

/** @brief The Vulkan address mode for an SDK address mode. */
[[nodiscard]] VkSamplerAddressMode addressModeOf(vn::graphics::Sampler::AddressMode mode) noexcept
{
    using AddressMode = vn::graphics::Sampler::AddressMode;
    switch (mode) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case AddressMode::MirrorClampToEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/** @brief The Vulkan border colour for an SDK border colour. */
[[nodiscard]] VkBorderColor borderColorOf(vn::graphics::Sampler::BorderColor color) noexcept
{
    using BorderColor = vn::graphics::Sampler::BorderColor;
    switch (color) {
    case BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case BorderColor::OpaqueBlack: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case BorderColor::OpaqueWhite: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

/**
 * @brief Rounds @p offset up to the four-byte boundary a `vkCmdCopyBufferToImage` source offset must respect.
 *
 * @details
 * - `VkBufferImageCopy::bufferOffset` must be a multiple of the format's texel block size, which is four for every
 *   uncompressed format and larger only for block-compressed ones; a level that is not a multiple of four bytes (an odd
 *   `R8` mip, say) would otherwise put the next region at an offset the driver is entitled to refuse.
 * - It is a free function because it is a fact about the COPY, not about one upload: both the sizing walk and the
 *   writing walk of `prepareUpload` have to round the same way, or the buffer would be sized for one layout and filled
 *   by another.
 *
 * @param offset The byte offset to round.
 * @return The smallest four-byte multiple that is not below @p offset.
 */
[[nodiscard]] constexpr VkDeviceSize alignedCopyOffset(const VkDeviceSize offset) noexcept
{
    return (offset + 3U) & ~VkDeviceSize{ 3U };
}

} // namespace

struct ContentImages::Impl {
    /// @brief One uploaded texture revision.
    ///
    /// THE ENTRY IS THE SHAREABLE UNIT (see ParkFn): the table owns one, and every frame that resolved its view holds
    /// one too - so a release can erase the table's share while a recording frame keeps reading the image, and the park
    /// destroys them when neither is left and the submission watermark has passed.
    struct Entry {
        vn::intrusive_ptr<const vn::graphics::Texture> texture; // Held so the key (its address) cannot be recycled.
        std::uint64_t                                 revision{ 0 };
        std::uint64_t                                 bytes{ 0 }; // Bytes this image was uploaded from (its residency weight).
        VkImage                                       image{ VK_NULL_HANDLE };
        MemorySlice                                   memory{}; // The pool range the image is bound into.
        VkImageView                                   view{ VK_NULL_HANDLE };
    };

    const VolkDeviceTable* table{ nullptr };
    VkDevice               device{ VK_NULL_HANDLE };
    /// The device's memory pool (borrowed: the device owns it and outlives this cache).
    MemoryAllocator* allocator{ nullptr };

    /// The device's park seam (see ParkFn). Empty destroys immediately (before initialize, during teardown).
    ParkFn park{};

    /// The largest anisotropy factor the device honours (see setMaxAnisotropy): a sampler built above it would be an
    /// invalid create, so the request is clamped here - the same number the capabilities report.
    float max_anisotropy{ 1.0F };

    std::unordered_map<const vn::graphics::Texture*, std::shared_ptr<Entry>> textures;

    /**
     * @brief One driver's upload bookkeeping (see BatchId): what it recorded and has not proven, and what it replaced.
     *
     * @details
     * - `pending` IS THE PUBLICATION LEDGER (see the class note): an entry lives here from "its copy was recorded" until
     *   "the batch's fence was waited", and only @ref batchCompleted moves it into `textures`. A frame head that asks
     *   for the texture meanwhile finds it here for its OWN batch only, so another driver copies it for itself instead
     *   of binding an image nobody filled.
     * - `retired` holds what a promotion replaced (and what a host release removed while it was pending): parked
     *   against the device's watermark, because a submitted frame of another driver may still sample them.
     * - BATCHED BY DRIVER, because "the command buffer that recorded this copy has completed" is a fact about ONE
     *   driver's submission. With two frames in flight, the frame that ends first must not free the staging of the
     *   frame that is still recording.
     */
    struct Batch {
        std::vector<std::shared_ptr<Entry>>           pending;
        std::vector<std::shared_ptr<Entry>>           retired;
        std::vector<std::pair<VkBuffer, MemorySlice>> staging;
    };
    std::unordered_map<std::uint64_t, Batch> batches;

    /**
     * @brief What the PREPARE phase built for one upload: one host-visible buffer holding every layer and mip, the image
     *        range it fills, and the copy regions that read it.
     *
     * @details
     * - ONE BUFFER PER UPLOAD, NOT ONE PER LAYER: the whole upload is one allocation, one mapping and one
     *   `vkCmdCopyBufferToImage` (that call takes the whole region array), so a cube map's six faces cost one object
     *   instead of six - and a failure has one thing to give back rather than the layers built so far (see §2.23).
     * - THE REGIONS ARE BUILT HERE, ONCE: the recording phase states them to the driver and allocates nothing, which is
     *   what lets it answer no failure at all (see the class note: "prepared, then recorded").
     */
    struct StagedUpload
    {
        VkBuffer                       buffer{ VK_NULL_HANDLE }; ///< The staging buffer (handed to the batch on record).
        MemorySlice                    memory{};                 ///< The pool range that buffer is bound into.
        VkImageSubresourceRange        range{};                  ///< Every layer and mip the upload fills.
        std::vector<VkBufferImageCopy> regions;                  ///< One per layer and mip, in buffer order.
        std::uint64_t                  bytes{ 0 };               ///< Bytes copied in (the entry's residency weight).
    };

    std::vector<std::pair<vn::graphics::Sampler, VkSampler>> samplers;

    /// The 1x1 stand-in, as an ENTRY like any other so a frame can keep the view it bound (see TextureView).
    std::shared_ptr<Entry> fallback{};
    VkSampler              fallback_sampler{ VK_NULL_HANDLE };
    /// THE SAME PUBLICATION RULE AS A TEXTURE (see the class note): the batch that recorded the stand-in's copy, or 0
    /// once that copy is proven. Another driver asking for the view before that gets an empty answer and skips its draw.
    std::uint64_t fallback_pending_batch{ 0 };

    /// The numbers a report reads (see Progress): cumulative builds/copies/releases, plus the status counts.
    std::uint64_t built{ 0 };
    std::uint64_t uploaded_bytes{ 0 };
    std::uint64_t released{ 0 };
    std::uint64_t bytes_total{ 0 };

    /**
     * @brief Creates a buffer and binds a host-visible coherent slice of the pool to it.
     *
     * @param size   Bytes to allocate (must be > 0).
     * @param usage  Buffer usage flags.
     * @param buffer Receives the buffer.
     * @param memory Receives the pool range the buffer is bound into.
     * @return true when both exist.
     */
    [[nodiscard]] bool createHostBuffer(const VkDeviceSize size,
                                        const VkBufferUsageFlags usage,
                                        VkBuffer&                buffer,
                                        MemorySlice&             memory) const
    {
        VkBufferCreateInfo info{};
        info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size        = size;
        info.usage       = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (table->vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryRequirements requirements{};
        table->vkGetBufferMemoryRequirements(device, buffer, &requirements);

        memory = allocator->allocate(requirements, MemoryUsage::HostVisible, MemoryKind::Linear);
        if (!memory.valid()) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        if (table->vkBindBufferMemory(device, buffer, memory.memory, memory.offset) != VK_SUCCESS) {
            allocator->release(memory);
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    /** @brief Destroys a buffer and gives its slice back to the pool (idempotent). */
    void destroyBuffer(VkBuffer& buffer, MemorySlice& memory) const noexcept
    {
        if (buffer != VK_NULL_HANDLE) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (allocator != nullptr) {
            allocator->release(memory);
        }
    }

    /**
     * @brief Runs @p destroy now, or parks it with the device until no submitted frame - and no frame still recording -
     *        can still name the entry.
     *
     * @details
     * - The same rule the content recorder states (see ContentDraw::setRetirement): the cache answers "is this image
     *   resident", the queue answers "may this object be destroyed", and without a queue - before initialize, or during
     *   a teardown that already idled the device - the destruction runs at once.
     *
     * @param named   The entry whose objects @p destroy will release (the park's share probe; empty = nobody else can
     *                hold it).
     * @param destroy What to run when the image may go (it keeps the entry alive until then).
     */
    void releaseLater(std::weak_ptr<const void> named, std::function<void()> destroy)
    {
        if (park) {
            park(std::move(named), std::move(destroy));
            return;
        }
        destroy();
    }

    /** @brief Destroys one uploaded image entry (idempotent). */
    void destroyEntry(Entry& entry) const noexcept
    {
        if (entry.view != VK_NULL_HANDLE) {
            table->vkDestroyImageView(device, entry.view, nullptr);
            entry.view = VK_NULL_HANDLE;
        }
        if (entry.image != VK_NULL_HANDLE) {
            table->vkDestroyImage(device, entry.image, nullptr);
            entry.image = VK_NULL_HANDLE;
        }
        if (allocator != nullptr) {
            allocator->release(entry.memory);
        }
    }

    /**
     * @brief Creates the image and view for one texture revision.
     *
     * @param texture The texture whose description (size, format, mip count, layers) the objects are built for.
     * @param entry   Receives the image and view (the caller fills the revision).
     * @param why     Receives the reason on failure.
     * @return true when the objects exist.
     */
    [[nodiscard]] bool createTextureImage(const vn::graphics::Texture& texture, Entry& entry, vn::String* why)
    {
        const VkFormat format = formatOf(texture.format());
        if (format == VK_FORMAT_UNDEFINED) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's pixel format has no sampled Vulkan spelling");
            }
            return false;
        }

        const auto layers = static_cast<std::uint32_t>(texture.layerCount());
        const auto mips   = static_cast<std::uint32_t>(texture.mipCount());

        VkImageCreateInfo info{};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.extent.width  = static_cast<std::uint32_t>(texture.width());
        info.extent.height = static_cast<std::uint32_t>(texture.height());
        info.extent.depth  = 1U;
        info.mipLevels     = mips;
        info.arrayLayers   = layers;
        info.format        = format;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        // A NEW IMAGE IS BORN IN THE TABLE'S Undefined ROW (see BarrierPlan): the record the upload starts from states
        // the same usage, so creation and the first barrier cannot disagree.
        info.initialLayout = stateOf(ImageLayout::Undefined).layout;
        info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.samples       = VK_SAMPLE_COUNT_1_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (layers == 6U) {
            info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }

        if (table->vkCreateImage(device, &info, nullptr, &entry.image) != VK_SUCCESS) {
            entry.image = VK_NULL_HANDLE;
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's image could not be created");
            }
            return false;
        }

        VkMemoryRequirements requirements{};
        table->vkGetImageMemoryRequirements(device, entry.image, &requirements);

        entry.memory = allocator->allocate(requirements, MemoryUsage::DeviceLocal, MemoryKind::Optimal);
        if (!entry.memory.valid()) {
            destroyEntry(entry);
            if (why != nullptr) {
                *why = vn::String(u8"vk: the pool could not serve a material texture");
            }
            return false;
        }
        if (table->vkBindImageMemory(device, entry.image, entry.memory.memory, entry.memory.offset) != VK_SUCCESS) {
            destroyEntry(entry);
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's memory could not be bound");
            }
            return false;
        }

        VkImageViewCreateInfo view{};
        view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image                           = entry.image;
        view.viewType                        = layers == 6U ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        view.format                          = format;
        view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.baseMipLevel   = 0U;
        view.subresourceRange.levelCount     = mips;
        view.subresourceRange.baseArrayLayer = 0U;
        view.subresourceRange.layerCount     = layers;
        if (table->vkCreateImageView(device, &view, nullptr, &entry.view) != VK_SUCCESS) {
            entry.view = VK_NULL_HANDLE;
            destroyEntry(entry);
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's view could not be created");
            }
            return false;
        }
        return true;
    }

    /** @brief Gives a prepared upload back: it was never recorded, so nothing names it and it dies at once. */
    void discardStaged(StagedUpload& staged) const noexcept
    {
        destroyBuffer(staged.buffer, staged.memory);
        staged.regions.clear();
        staged.range = VkImageSubresourceRange{};
        staged.bytes = 0;
    }

    /**
     * @brief Builds the bytes one texture revision is filled from: ONE host-visible buffer with every layer and mip, and
     *        the copy regions that read it.
     *
     * @details
     * - THIS IS THE WHOLE FALLIBLE HALF OF AN UPLOAD (see the class note): it records no command, so a refusal here
     *   destroys what it built and leaves the caller's command buffer exactly as it found it - which is the property
     *   that lets the recording half answer no failure at all (review 19 `Z15`).
     * - ONE ALLOCATION AND ONE MAPPING FOR THE WHOLE UPLOAD: the layers of a cube map are regions of one buffer rather
     *   than six buffers, so a mid-way refusal cannot leave half the faces' staging behind - there is no half.
     * - EVERY LEVEL'S BASE IS ROUNDED to the copy command's four-byte rule, so a mip chain whose levels are not a
     *   multiple of four bytes (an `R8` texture with an odd level) still states a legal `bufferOffset` - the sizing walk
     *   and the writing walk round identically, which is what keeps the buffer and the regions the same layout.
     *
     * @param texture The texture whose CPU images are staged.
     * @param staged  Receives the buffer, its slice, the image range and the regions (populated only on true).
     * @param why     Receives the reason on failure.
     * @return true when every layer was copied into one buffer.
     */
    [[nodiscard]] bool prepareUpload(const vn::graphics::Texture& texture, StagedUpload& staged, vn::String* why)
    {
        // THE TOTAL FIRST, from the same walk the copy below takes: every level of every layer, each starting on a
        // copy-aligned boundary.
        VkDeviceSize total = 0;
        for (int layer = 0; layer < texture.layerCount(); ++layer) {
            const raw_ptr<const vn::imaging::Image> image = texture.layer(layer);
            if (image == nullptr) {
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a complete texture reported an empty layer");
                }
                return false;
            }
            for (int mip = 0; mip < image->mipCount(); ++mip) {
                total = alignedCopyOffset(total) + static_cast<VkDeviceSize>(image->mipByteSize(mip));
            }
        }
        if (total == 0U) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture holds no bytes to copy");
            }
            return false;
        }
        if (!createHostBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staged.buffer, staged.memory)) {
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's staging buffer could not be created");
            }
            return false;
        }
        // THE POOL'S MAPPING IS PERSISTENT (see MemoryAllocator::map): a coherent block needs no flush, so a staging
        // buffer is written and forgotten rather than mapped and unmapped around one memcpy.
        auto* const mapped = static_cast<std::byte*>(allocator->map(staged.memory));
        if (mapped == nullptr) {
            discardStaged(staged);
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's staging buffer could not be mapped");
            }
            return false;
        }

        const auto layers = static_cast<std::uint32_t>(texture.layerCount());
        const auto mips   = static_cast<std::uint32_t>(texture.mipCount());
        VkDeviceSize base = 0;
        for (std::uint32_t layer = 0U; layer < layers; ++layer) {
            const raw_ptr<const vn::imaging::Image> image = texture.layer(static_cast<int>(layer));
            // THE IMAGE'S OWN MIP COUNT, the same walk the sizing above took: a chain that disagreed with the texture's
            // would otherwise state regions for levels it does not hold.
            for (int mip = 0; mip < image->mipCount(); ++mip) {
                base                                      = alignedCopyOffset(base);
                const std::size_t bytes                   = image->mipByteSize(mip);
                std::memcpy(mapped + base, image->mipData(mip).data(), bytes);
                VkBufferImageCopy region{};
                region.bufferOffset                    = base;
                region.bufferRowLength                 = 0U;
                region.bufferImageHeight               = 0U;
                region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel       = static_cast<std::uint32_t>(mip);
                region.imageSubresource.baseArrayLayer = layer;
                region.imageSubresource.layerCount     = 1U;
                region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
                region.imageExtent                     = VkExtent3D{ static_cast<std::uint32_t>(image->mipWidth(mip)),
                                                                     static_cast<std::uint32_t>(image->mipHeight(mip)), 1U };
                staged.regions.push_back(region);
                // RESIDENCY WEIGHT: the texture bytes copied in, which is what a release has to make go down. The
                // alignment padding between levels is the copy's cost, not the image's size.
                staged.bytes += static_cast<std::uint64_t>(bytes);
                base += static_cast<VkDeviceSize>(bytes);
            }
        }

        // THE WHOLE IMAGE, NOT ITS LAYER 0: the copies below fill every layer and mip, and a barrier that names one
        // subresource leaves the rest UNDEFINED - which is what the validation layer reports at submit time for a
        // cube map's faces 1..5 (and what leaves them in TRANSFER_DST for the shader that samples them).
        staged.range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        staged.range.baseMipLevel   = 0U;
        staged.range.levelCount     = mips;
        staged.range.baseArrayLayer = 0U;
        staged.range.layerCount     = layers;
        return true;
    }

    /**
     * @brief Records one PREPARED upload and leaves the image `SHADER_READ_ONLY_OPTIMAL`.
     *
     * @details
     * - IT ANSWERS NOTHING, AND THAT IS THE RULE (see the class note): every object it names was built by the prepare
     *   phase, so there is nothing left to refuse - and a command the driver already holds cannot be taken back, so a
     *   failure here could only be paid for by destroying something the command buffer names.
     * - ONE COPY COMMAND FOR THE WHOLE UPLOAD: the regions are one call's argument, so a cube map's six faces and their
     *   mips are a single `vkCmdCopyBufferToImage` rather than one per face.
     * - The staging buffer becomes the BATCH's here: only this driver's command buffer names it, so it is destroyed when
     *   that batch completes (`batchCompleted`) and parked when it never ran (`batchAbandoned`).
     *
     * @param entry    The image the copy fills.
     * @param staged   What the prepare phase built (its buffer is handed to @p book).
     * @param commands The frame's command buffer, outside any rendering instance.
     * @param book     The recording driver's books.
     * @param tally    Receives the barriers this recorded (see `RecordingTally`), or null.
     */
    void recordUpload(Entry& entry, StagedUpload& staged, VkCommandBuffer commands, Batch& book, RecordingTally* tally)
    {
        const ImageState from = stateOf(ImageLayout::Undefined);
        const ImageState to   = transferWriteState();

        const VkImageMemoryBarrier2 to_transfer = barrierBetween(entry.image, from, to, staged.range);
        recordBarrier(*table, commands, to_transfer, tally);

        // THE COPY NAMES THE ROW THE BARRIER ABOVE MOVED THE IMAGE INTO (`to`), not a second spelling of it (see
        // BarrierPlan).
        table->vkCmdCopyBufferToImage(commands, staged.buffer, entry.image, to.layout,
                                      static_cast<std::uint32_t>(staged.regions.size()), staged.regions.data());

        // THE SAME RANGE THE UPLOAD WRITES (every layer and mip): a single-subresource transition here would leave the
        // faces the descriptor samples in TRANSFER_DST, which is the layout error the app measured.
        const VkImageMemoryBarrier2 to_sampled = barrierBetween(entry.image, to, stateOf(ImageLayout::ShaderReadOnly), staged.range);
        recordBarrier(*table, commands, to_sampled, tally);

        book.staging.emplace_back(staged.buffer, staged.memory);
        staged.buffer = VK_NULL_HANDLE;
        staged.memory = MemorySlice{};
    }
};

ContentImages::ContentImages()
  : impl(std::make_unique<Impl>())
{
}

ContentImages::~ContentImages()
{
    destroy();
}

bool ContentImages::create(const VolkDeviceTable& table, const VkDevice device, MemoryAllocator& allocator)
{
    destroy();
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    impl->table     = &table;
    impl->device    = device;
    impl->allocator = &allocator;
    // The fallback sampler reads the 1x1 white stand-in; it is also what an absent slot is sampled with.
    impl->fallback_sampler = samplerFor(vn::graphics::Sampler{});
    return impl->fallback_sampler != VK_NULL_HANDLE;
}

void ContentImages::setMaxAnisotropy(const float max_anisotropy) noexcept
{
    if (!impl) {
        return;
    }
    // BELOW 2 THERE IS NOTHING TO FILTER ANISOTROPICALLY: the value is clamped up to 1 so a caller cannot build a
    // sampler with a meaningless factor.
    impl->max_anisotropy = max_anisotropy >= 1.0F ? max_anisotropy : 1.0F;
}

void ContentImages::setRetirement(ParkFn park)
{
    if (!impl) {
        return;
    }
    impl->park = std::move(park);
}

void ContentImages::destroy() noexcept
{
    if (!impl) {
        return;
    }
    // A TEARDOWN DESTROYS, IT DOES NOT PARK (see releaseLater): the device has idled its queue before calling this,
    // and a park recorded here would outlive the queue that would have run it.
    impl->park = {};
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        for (auto& [texture, entry] : impl->textures) {
            (void)texture;
            impl->destroyEntry(*entry);
        }
        impl->textures.clear();
        for (auto& [batch, entry] : impl->batches) {
            (void)batch;
            for (std::shared_ptr<Impl::Entry>& pending : entry.pending) {
                impl->destroyEntry(*pending);
            }
            for (std::shared_ptr<Impl::Entry>& retired : entry.retired) {
                impl->destroyEntry(*retired);
            }
            for (auto& [buffer, memory] : entry.staging) {
                impl->destroyBuffer(buffer, memory);
            }
        }
        impl->batches.clear();
        for (auto& [sampler, handle] : impl->samplers) {
            (void)sampler;
            if (handle != VK_NULL_HANDLE) {
                impl->table->vkDestroySampler(impl->device, handle, nullptr);
            }
        }
        impl->samplers.clear();
        if (impl->fallback) {
            impl->destroyEntry(*impl->fallback);
            impl->fallback.reset();
        }
        if (impl->fallback_sampler != VK_NULL_HANDLE) {
            // The fallback sampler is one of `samplers` (created through samplerFor), so it was destroyed with them.
            impl->fallback_sampler = VK_NULL_HANDLE;
        }
    }
    impl->table     = nullptr;
    impl->device    = VK_NULL_HANDLE;
    impl->allocator = nullptr;
    impl->built           = 0;
    impl->uploaded_bytes  = 0;
    impl->released        = 0;
    impl->bytes_total     = 0;
}

ContentImages::Progress ContentImages::progress() const noexcept
{
    Progress out;
    if (impl == nullptr) {
        return out;
    }
    out.builds     = impl->built;
    out.bytes      = impl->uploaded_bytes;
    out.releases   = impl->released;
    out.live       = static_cast<std::uint32_t>(impl->textures.size());
    out.live_bytes = impl->bytes_total;
    // WHAT IS NOT PUBLISHED YET: counted where it is written down, so "resident" and "recorded" cannot be the same
    // number by accident (see Progress).
    for (const auto& [batch, book] : impl->batches) {
        (void)batch;
        for (const std::shared_ptr<Impl::Entry>& pending : book.pending) {
            ++out.pending;
            out.pending_bytes += pending->bytes;
        }
    }
    return out;
}

bool ContentImages::releaseTexture(const vn::graphics::Texture& texture)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return false;
    }
    bool dropped = false;
    const auto found = impl->textures.find(&texture);
    if (found != impl->textures.end()) {
        impl->bytes_total -= found->second->bytes;
        // THE IMAGE LEAVES THE TABLE NOW AND THE OBJECTS FOLLOW LATER (see releaseLater): the next frame uploads the
        // texture again, which is what the release costs, and the Vulkan image dies once the device's watermark has
        // passed the submission that could still sample it AND no frame still names the entry.
        std::shared_ptr<Impl::Entry> doomed = std::move(found->second);
        impl->textures.erase(found);
        const std::weak_ptr<const void> named = doomed;
        impl->releaseLater(named, [target = impl.get(), entry = std::move(doomed)]() mutable { target->destroyEntry(*entry); });
        dropped = true;
    }
    // A COPY THAT IS NOT PUBLISHED YET GOES THE SAME WAY: a promotion must not resurrect a texture the host let go, so
    // the pending entry leaves its batch now - and its objects are parked like any other, because the batch's command
    // buffer may still be recording them.
    for (auto& [batch, book] : impl->batches) {
        (void)batch;
        for (auto pending = book.pending.begin(); pending != book.pending.end();) {
            if ((*pending)->texture.get() != &texture) {
                ++pending;
                continue;
            }
            std::shared_ptr<Impl::Entry> doomed   = std::move(*pending);
            pending                               = book.pending.erase(pending);
            const std::weak_ptr<const void> named = doomed;
            impl->releaseLater(named, [target = impl.get(), entry = std::move(doomed)]() mutable { target->destroyEntry(*entry); });
            dropped = true;
        }
    }
    if (dropped) {
        ++impl->released;
    }
    return true;
}

std::uint32_t ContentImages::sweepUnheldTextures() noexcept
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return 0;
    }
    std::uint32_t withdrawn = 0;
    for (auto entry = impl->textures.begin(); entry != impl->textures.end();) {
        auto next = entry;
        ++next;
        const std::shared_ptr<Impl::Entry>& candidate = entry->second;
        // THE ENTRY'S SHARE IS THE LAST ONE (see the header): a material slot or a pass' imported input holds its own,
        // so a texture still named by content is never withdrawn here.
        const bool unheld = candidate == nullptr || candidate->texture == nullptr || candidate->texture->useCount() <= 1U;
        if (unheld) {
            std::shared_ptr<Impl::Entry> doomed = std::move(entry->second);
            impl->textures.erase(entry);
            if (doomed != nullptr) {
                impl->bytes_total -= doomed->bytes;
                const std::weak_ptr<const void> named = doomed;
                impl->releaseLater(named, [target = impl.get(), entry = std::move(doomed)]() mutable { target->destroyEntry(*entry); });
            }
            ++impl->released;
            ++withdrawn;
        }
        entry = next;
    }
    return withdrawn;
}

void ContentImages::batchCompleted(const BatchId batch) noexcept
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return;
    }
    const auto found = impl->batches.find(batch);
    if (found == impl->batches.end()) {
        return;
    }
    Impl::Batch& book = found->second;
    // PROMOTION: what this batch's command buffer copied is provable now, so the table may serve it (see the class
    // note). A revision the new one replaces leaves `live_bytes` and joins `retired` below - the entry never stopped
    // being resident, it was replaced.
    for (std::shared_ptr<Impl::Entry>& entry : book.pending) {
        const auto* const key      = entry->texture.get();
        const auto        resident = impl->textures.find(key);
        if (resident != impl->textures.end()) {
            impl->bytes_total -= resident->second->bytes;
            book.retired.push_back(std::move(resident->second));
            resident->second = std::move(entry);
        }
        else {
            impl->textures.emplace(key, std::move(entry));
        }
        impl->bytes_total += impl->textures.at(key)->bytes;
    }
    book.pending.clear();
    // THE 1x1 STAND-IN is published by the same evidence (see ensureFallback).
    if (impl->fallback_pending_batch == batch) {
        impl->fallback_pending_batch = 0;
    }
    // THE STAGING BUFFERS MAY GO NOW: only THIS driver's command buffer named them, and its fence has been waited.
    for (auto& [buffer, memory] : book.staging) {
        impl->destroyBuffer(buffer, memory);
    }
    book.staging.clear();
    // THE REPLACED IMAGES ARE PARKED, NOT DESTROYED (see releaseLater): another driver's frame may have resolved the
    // view this driver replaced and still be recording, so only the entry's own share count proves nobody can sample
    // them.
    for (std::shared_ptr<Impl::Entry>& entry : book.retired) {
        std::shared_ptr<Impl::Entry>    doomed = std::move(entry);
        const std::weak_ptr<const void> named  = doomed;
        impl->releaseLater(named, [target = impl.get(), entry = doomed]() { target->destroyEntry(*entry); });
    }
    impl->batches.erase(found);
}

void ContentImages::batchAbandoned(const BatchId batch) noexcept
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return;
    }
    const auto found = impl->batches.find(batch);
    if (found == impl->batches.end()) {
        return;
    }
    Impl::Batch& book = found->second;
    // PARKED, NOT DESTROYED (see the header): a submit that failed may still have executed part of the command buffer,
    // so what the driver could name waits for the device's watermark - and what was never published never becomes
    // resident, which is the whole point (the resident entry a promotion would have replaced stays).
    const auto park   = [this](std::shared_ptr<Impl::Entry>& entry) {
        std::shared_ptr<Impl::Entry>    doomed = std::move(entry);
        const std::weak_ptr<const void> named  = doomed;
        impl->releaseLater(named, [target = impl.get(), entry = doomed]() { target->destroyEntry(*entry); });
    };
    for (std::shared_ptr<Impl::Entry>& entry : book.pending) {
        park(entry);
    }
    book.pending.clear();
    // THE 1x1 STAND-IN GOES WITH THEM: it is one pixel and rebuilt on demand, so a stand-in nobody filled must not
    // outlive the frame that failed to fill it.
    if (impl->fallback_pending_batch == batch && impl->fallback) {
        impl->fallback_pending_batch = 0;
        park(impl->fallback);
        impl->fallback.reset();
    }
    // THE STAGING IS PARKED FOR THE SAME REASON the images are: the copy that read it may have run.
    for (auto& [buffer, memory] : book.staging) {
        impl->releaseLater(std::weak_ptr<const void>{},
                           [target = impl.get(), staged = std::make_pair(buffer, memory)]() mutable { target->destroyBuffer(staged.first, staged.second); });
    }
    book.staging.clear();
    for (std::shared_ptr<Impl::Entry>& entry : book.retired) {
        park(entry);
    }
    impl->batches.erase(found);
}

bool ContentImages::hasPendingUploads(const BatchId batch) const noexcept
{
    if (!impl) {
        return false;
    }
    // THE BATCH'S OWN BOOKS ARE THE LEDGER: a staging buffer exists from "a copy was recorded" until "that copy
    // completed", and a pending entry from the same moment until it is published - so either being non-empty means
    // this batch has recorded work that has not run yet.
    const auto found = impl->batches.find(batch);
    return found != impl->batches.end() && (!found->second.staging.empty() || !found->second.pending.empty());
}

ContentImages::TextureView ContentImages::viewFor(const vn::graphics::Texture& texture, const VkCommandBuffer commands, const BatchId batch, vn::String* why, RecordingTally* tally)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return {};
    }
    if (!texture.isComplete()) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a material texture is still being filled (an incomplete texture is not sampled)");
        }
        return {};
    }

    const auto found = impl->textures.find(&texture);
    if (found != impl->textures.end() && found->second->revision == texture.revision()) {
        return TextureView{ found->second->view, found->second };
    }
    // A COPY THIS BATCH ALREADY RECORDED IS ITS OWN, though it is not yet anyone else's: a frame that samples one
    // texture twice must not copy it twice, and another driver must not see it before the copy is proven (see
    // batchCompleted).
    Impl::Batch& book = impl->batches[batch];
    for (const std::shared_ptr<Impl::Entry>& pending : book.pending) {
        if (pending->texture.get() == &texture && pending->revision == texture.revision()) {
            return TextureView{ pending->view, pending };
        }
    }
    // NOTHING SERVES IT YET, so its copy is recorded here - and a copy may not be recorded inside a rendering instance:
    // the frame head records every upload before the first pass begins (see ContentCaches::resolveTextures).
    if (commands == VK_NULL_HANDLE) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a material texture was not prepared before its rendering instance began");
        }
        return {};
    }

    auto entry      = std::make_shared<Impl::Entry>();
    entry->texture  = vn::intrusive_ptr<const vn::graphics::Texture>(&texture);
    entry->revision = texture.revision();
    // PREPARE, THEN RECORD (see the class note). The image and the bytes it will be filled from are built FIRST, so a
    // refusal releases objects no command names yet - and the recording half, which runs only once everything exists,
    // has nothing left to refuse (review 19 `Z15`: a mid-record failure used to destroy an image the command buffer
    // already held a barrier and copies for, and the frame then submitted that command buffer).
    if (!impl->createTextureImage(texture, *entry, why)) {
        return {};
    }
    Impl::StagedUpload staged;
    if (!impl->prepareUpload(texture, staged, why)) {
        impl->destroyEntry(*entry);
        return {};
    }
    entry->bytes = staged.bytes;
    impl->recordUpload(*entry, staged, commands, book, tally);

    // THE BOOKS: built and copied now, RESIDENT only when THIS BATCH COMPLETES (see batchCompleted). The resident entry
    // this revision will replace stays exactly where it is until then - a frame that never ran must leave the last
    // proven revision in place (see batchAbandoned).
    ++impl->built;
    impl->uploaded_bytes += entry->bytes;
    book.pending.push_back(std::move(entry));
    return TextureView{ book.pending.back()->view, book.pending.back() };
}

VkSampler ContentImages::samplerFor(const vn::graphics::Sampler& sampler)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return VK_NULL_HANDLE;
    }
    for (const auto& [cached, handle] : impl->samplers) {
        if (cached == sampler) {
            return handle;
        }
    }

    VkSamplerCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter               = filterOf(sampler.mag_filter);
    // THE MIPMAP AXIS IS `mipmapMode`'S, NOT THE MIN FILTER'S (Vulkan's composition, unlike OpenGL's single enum):
    // `minFilter` filters WITHIN a level and `mipmapMode` BETWEEN two (the spec's OpenGL-mapping note pairs
    // `VK_FILTER_LINEAR` + `MIPMAP_MODE_NEAREST` with `GL_LINEAR_MIPMAP_NEAREST`), while `minLod` / `maxLod` /
    // `mipLodBias` below choose the level - Vulkan always mip-selects when the view has levels. So the base
    // spelling here is the whole mapping, and a `VK_FILTER_*_MIPMAP_*` value would state the mip half twice.
    // A host pins the base level with `maxLod = 0` (the spec defines that as always-magnification, so the MAG
    // filter is what such a sampler applies). Witness: `VkContentDrawTest.ASamplerReadsTheMipLevelItsLodClampNames`.
    info.minFilter               = filterOf(sampler.min_filter);
    info.mipmapMode              = mipmapModeOf(sampler.mipmap_mode);
    info.addressModeU            = addressModeOf(sampler.address_u);
    info.addressModeV            = addressModeOf(sampler.address_v);
    info.addressModeW            = addressModeOf(sampler.address_w);
    info.borderColor             = borderColorOf(sampler.border_color);
    // THE REQUEST IS CLAMPED, NEVER SILENTLY IGNORED: a factor above the device's maximum is an invalid sampler, so
    // the answer is the device's best - and 1 (feature off) keeps `anisotropyEnable` false, which is what the spec
    // requires beside a maximum of 1.
    const float anisotropy       = std::clamp(sampler.anisotropy, 1.0F, impl->max_anisotropy);
    info.anisotropyEnable        = anisotropy > 1.0F ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy           = anisotropy;
    info.compareEnable           = VK_FALSE;
    info.compareOp               = VK_COMPARE_OP_NEVER;
    info.minLod                  = sampler.min_lod;
    info.maxLod                  = sampler.max_lod;
    info.mipLodBias              = sampler.lod_bias;
    info.unnormalizedCoordinates = VK_FALSE;

    VkSampler handle = VK_NULL_HANDLE;
    if (impl->table->vkCreateSampler(impl->device, &info, nullptr, &handle) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    impl->samplers.emplace_back(sampler, handle);
    return handle;
}

ContentImages::TextureView ContentImages::ensureFallback(const VkCommandBuffer commands, const BatchId batch, vn::String* why, RecordingTally* tally)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return {};
    }
    if (impl->fallback && impl->fallback->view != VK_NULL_HANDLE) {
        // A SHARED CONSTANT (see fallbackView): a frame that finds it mid-fill binds it anyway, because dropping every
        // draw that needs a stand-in is worse than reading one pixel whose copy is one submission away.
        return TextureView{ impl->fallback->view, impl->fallback };
    }
    if (commands == VK_NULL_HANDLE) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image was not prepared before the rendering instance began");
        }
        return {};
    }

    auto              entry = std::make_shared<Impl::Entry>();
    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent.width  = 1U;
    info.extent.height = 1U;
    info.extent.depth  = 1U;
    info.mipLevels     = 1U;
    info.arrayLayers   = 1U;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // THE SAME BIRTH LAYOUT as a texture revision (see createTextureImage and BarrierPlan).
    info.initialLayout = stateOf(ImageLayout::Undefined).layout;
    info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (impl->table->vkCreateImage(impl->device, &info, nullptr, &entry->image) != VK_SUCCESS) {
        entry->image = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image could not be created");
        }
        return {};
    }

    VkMemoryRequirements requirements{};
    impl->table->vkGetImageMemoryRequirements(impl->device, entry->image, &requirements);
    entry->memory = impl->allocator->allocate(requirements, MemoryUsage::DeviceLocal, MemoryKind::Optimal);
    if (!entry->memory.valid()) {
        impl->destroyEntry(*entry);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the pool could not serve the 1x1 fallback image");
        }
        return {};
    }
    if (impl->table->vkBindImageMemory(impl->device, entry->image, entry->memory.memory, entry->memory.offset) != VK_SUCCESS) {
        impl->destroyEntry(*entry);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image's memory could not be bound");
        }
        return {};
    }

    // THE VIEW BEFORE THE COPY, because a view does not need content: creating it here keeps every fallible call in the
    // prepare phase, so a refused `vkCreateImageView` destroys an image no command names yet (review 19 `Z15` - it used
    // to run AFTER the barrier and the copy were recorded).
    VkImageViewCreateInfo view{};
    view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image                           = entry->image;
    view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view.format                          = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount     = 1U;
    view.subresourceRange.layerCount     = 1U;
    if (impl->table->vkCreateImageView(impl->device, &view, nullptr, &entry->view) != VK_SUCCESS) {
        entry->view = VK_NULL_HANDLE;
        impl->destroyEntry(*entry);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image's view could not be created");
        }
        return {};
    }

    // THE BYTES, then the commands: one 4-byte buffer, one region, then the same record verb a texture's upload uses
    // (see Impl::recordUpload) - so the stand-in states no copy path of its own.
    Impl::StagedUpload staged;
    staged.range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    staged.range.levelCount     = 1U;
    staged.range.layerCount     = 1U;
    if (!impl->createHostBuffer(4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staged.buffer, staged.memory)) {
        impl->destroyEntry(*entry);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback's staging buffer could not be created");
        }
        return {};
    }
    const std::uint8_t white[4]{ 255U, 255U, 255U, 255U };
    auto* const mapped = static_cast<std::byte*>(impl->allocator->map(staged.memory));
    if (mapped == nullptr) {
        impl->discardStaged(staged);
        impl->destroyEntry(*entry);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback's staging buffer could not be mapped");
        }
        return {};
    }
    std::memcpy(mapped, white, sizeof(white));
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent                 = VkExtent3D{ 1U, 1U, 1U };
    staged.regions.push_back(region);
    staged.bytes = sizeof(white);
    entry->bytes = staged.bytes;

    impl->recordUpload(*entry, staged, commands, impl->batches[batch], tally);

    // PENDING UNTIL THIS BATCH COMPLETES (see the class note): the copy above is recorded, not proven.
    impl->fallback               = std::move(entry);
    impl->fallback_pending_batch = batch;
    return TextureView{ impl->fallback->view, impl->fallback };
}

VkImageView ContentImages::fallbackView() const noexcept
{
    return impl != nullptr && impl->fallback ? impl->fallback->view : VK_NULL_HANDLE;
}

VkSampler ContentImages::fallbackSampler() const noexcept
{
    return impl != nullptr ? impl->fallback_sampler : VK_NULL_HANDLE;
}

VN_BACKEND_VK_NS_END
