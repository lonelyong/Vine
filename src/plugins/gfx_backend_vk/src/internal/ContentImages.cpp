#include <vine/backend_vk/internal/ContentImages.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
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
 * @brief Finds a memory type that serves @p bits and has every property in @p wanted.
 *
 * @param memory The device's memory properties.
 * @param bits   The type bits the allocation requires.
 * @param wanted The property flags the memory must have.
 * @param out    Receives the type index.
 * @return true when a matching type exists.
 */
[[nodiscard]] bool findMemoryType(const VkPhysicalDeviceMemoryProperties& memory,
                                  std::uint32_t                          bits,
                                  VkMemoryPropertyFlags                  wanted,
                                  std::uint32_t&                         out) noexcept
{
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1U << index)) != 0U && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
            out = index;
            return true;
        }
    }
    return false;
}

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

/// @brief The state an upload writes from: `TRANSFER_DST_OPTIMAL`, transfer stage, write access.
[[nodiscard]] ImageState transferWriteState() noexcept
{
    ImageState state{};
    state.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    state.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    state.access = VK_ACCESS_TRANSFER_WRITE_BIT;
    return state;
}

} // namespace

struct ContentImages::Impl {
    /// @brief One uploaded texture revision.
    struct Entry {
        vn::intrusive_ptr<const vn::graphics::Texture> texture; // Held so the key (its address) cannot be recycled.
        std::uint64_t                                 revision{ 0 };
        std::uint64_t                                 bytes{ 0 }; // Bytes this image was uploaded from (its residency weight).
        VkImage                                       image{ VK_NULL_HANDLE };
        VkDeviceMemory                                memory{ VK_NULL_HANDLE };
        VkImageView                                   view{ VK_NULL_HANDLE };
    };

    const VolkDeviceTable*          table{ nullptr };
    VkDevice                        device{ VK_NULL_HANDLE };
    VkPhysicalDeviceMemoryProperties memory{};

    /// The largest anisotropy factor the device honours (see setMaxAnisotropy): a sampler built above it would be an
    /// invalid create, so the request is clamped here - the same number the capabilities report.
    float max_anisotropy{ 1.0F };

    std::unordered_map<const vn::graphics::Texture*, Entry> textures;
    /// Entries a revision replaced (destroyed once the frame that may still name them has finished).
    std::vector<Entry> retired;
    /// Staging buffers an upload recorded (alive until that command buffer has finished).
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging;

    std::vector<std::pair<vn::graphics::Sampler, VkSampler>> samplers;

    VkImage        fallback_image{ VK_NULL_HANDLE };
    VkDeviceMemory fallback_memory{ VK_NULL_HANDLE };
    VkImageView    fallback_view{ VK_NULL_HANDLE };
    VkSampler      fallback_sampler{ VK_NULL_HANDLE };
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> fallback_staging;

    /// The numbers a report reads (see Progress): cumulative builds/copies/releases, plus the live total.
    std::uint64_t built{ 0 };
    std::uint64_t uploaded_bytes{ 0 };
    std::uint64_t released{ 0 };
    std::uint64_t bytes_total{ 0 };

    /**
     * @brief Creates a buffer and binds host-visible coherent memory to it.
     *
     * @param size   Bytes to allocate (must be > 0).
     * @param usage  Buffer usage flags.
     * @param buffer Receives the buffer.
     * @param memory Receives the bound memory.
     * @return true when both exist.
     */
    [[nodiscard]] bool createHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory_handle) const
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

        std::uint32_t memory_type = 0;
        if (!findMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory_type)) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }

        VkMemoryAllocateInfo allocate{};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize  = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (table->vkAllocateMemory(device, &allocate, nullptr, &memory_handle) != VK_SUCCESS) {
            memory_handle = VK_NULL_HANDLE;
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        if (table->vkBindBufferMemory(device, buffer, memory_handle, 0U) != VK_SUCCESS) {
            table->vkFreeMemory(device, memory_handle, nullptr);
            memory_handle = VK_NULL_HANDLE;
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    /** @brief Destroys a buffer and its memory (idempotent). */
    void destroyBuffer(VkBuffer& buffer, VkDeviceMemory& memory_handle) const noexcept
    {
        if (buffer != VK_NULL_HANDLE) {
            table->vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
        }
        if (memory_handle != VK_NULL_HANDLE) {
            table->vkFreeMemory(device, memory_handle, nullptr);
            memory_handle = VK_NULL_HANDLE;
        }
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
        if (entry.memory != VK_NULL_HANDLE) {
            table->vkFreeMemory(device, entry.memory, nullptr);
            entry.memory = VK_NULL_HANDLE;
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
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
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

        std::uint32_t memory_type = 0;
        if (!findMemoryType(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type)) {
            destroyEntry(entry);
            if (why != nullptr) {
                *why = vn::String(u8"vk: no device-local memory type serves a material texture");
            }
            return false;
        }

        VkMemoryAllocateInfo allocate{};
        allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize  = requirements.size;
        allocate.memoryTypeIndex = memory_type;
        if (table->vkAllocateMemory(device, &allocate, nullptr, &entry.memory) != VK_SUCCESS) {
            entry.memory = VK_NULL_HANDLE;
            destroyEntry(entry);
            if (why != nullptr) {
                *why = vn::String(u8"vk: a material texture's memory could not be allocated");
            }
            return false;
        }
        if (table->vkBindImageMemory(device, entry.image, entry.memory, 0U) != VK_SUCCESS) {
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

    /**
     * @brief Records the copy of every layer and mip into @p entry, leaving the image `SHADER_READ_ONLY_OPTIMAL`.
     *
     * @param texture  The texture whose CPU images are copied.
     * @param entry    The entry whose image receives them.
     * @param commands The frame's command buffer.
     * @param why      Receives the reason on failure.
     * @return true when every layer was copied.
     */
    [[nodiscard]] bool recordUpload(const vn::graphics::Texture& texture, Entry& entry, VkCommandBuffer commands, vn::String* why)
    {
        const auto aspect       = static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT);
        const ImageState from   = stateOf(vn::graphics::backend::ImageLayout::Undefined);
        const ImageState to     = transferWriteState();

        // THE WHOLE IMAGE, NOT ITS LAYER 0: the loop below copies every layer and mip, and a barrier that names one
        // subresource leaves the rest UNDEFINED - which is what the validation layer reports at submit time for a
        // cube map's faces 1..5 (and what leaves them in TRANSFER_DST for the shader that samples them).
        VkImageSubresourceRange whole{};
        whole.aspectMask     = aspect;
        whole.baseMipLevel   = 0U;
        whole.levelCount     = static_cast<std::uint32_t>(texture.mipCount());
        whole.baseArrayLayer = 0U;
        whole.layerCount     = static_cast<std::uint32_t>(texture.layerCount());

        const VkImageMemoryBarrier to_transfer = barrierBetween(entry.image, from, to, whole);
        table->vkCmdPipelineBarrier(commands,
                                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    0U,
                                    0U,
                                    nullptr,
                                    0U,
                                    nullptr,
                                    1U,
                                    &to_transfer);

        for (int layer = 0; layer < texture.layerCount(); ++layer) {
            const raw_ptr<const vn::imaging::Image> image = texture.layer(layer);
            if (image == nullptr) {
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a complete texture reported an empty layer");
                }
                return false;
            }

            VkBuffer       staging_buffer{ VK_NULL_HANDLE };
            VkDeviceMemory staging_memory{ VK_NULL_HANDLE };
            const std::size_t bytes = image->totalByteSize();
            if (bytes == 0U || !createHostBuffer(static_cast<VkDeviceSize>(bytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer, staging_memory)) {
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a material texture's staging buffer could not be created");
                }
                return false;
            }

            void* mapped = nullptr;
            if (table->vkMapMemory(device, staging_memory, 0U, static_cast<VkDeviceSize>(bytes), 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
                destroyBuffer(staging_buffer, staging_memory);
                if (why != nullptr) {
                    *why = vn::String(u8"vk: a material texture's staging buffer could not be mapped");
                }
                return false;
            }
            std::memcpy(mapped, image->mipData(0).data(), bytes);
            table->vkUnmapMemory(device, staging_memory);
            // RESIDENCY WEIGHT: the bytes copied in, which is what a release has to make go down.
            entry.bytes += static_cast<std::uint64_t>(bytes);

            std::size_t offset = 0U;
            for (int mip = 0; mip < image->mipCount(); ++mip) {
                VkBufferImageCopy region{};
                region.bufferOffset                    = offset;
                region.bufferRowLength                 = 0U;
                region.bufferImageHeight               = 0U;
                region.imageSubresource.aspectMask     = aspect;
                region.imageSubresource.mipLevel       = static_cast<std::uint32_t>(mip);
                region.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(layer);
                region.imageSubresource.layerCount     = 1U;
                region.imageOffset                     = VkOffset3D{ 0, 0, 0 };
                region.imageExtent                     = VkExtent3D{ static_cast<std::uint32_t>(image->mipWidth(mip)),
                                                                     static_cast<std::uint32_t>(image->mipHeight(mip)), 1U };
                table->vkCmdCopyBufferToImage(commands, staging_buffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &region);
                offset += image->mipByteSize(mip);
            }
            staging.emplace_back(staging_buffer, staging_memory);
        }
        return true;
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

bool ContentImages::create(const VolkDeviceTable& table, const VkDevice device, const VkPhysicalDeviceMemoryProperties& memory)
{
    destroy();
    if (device == VK_NULL_HANDLE) {
        return false;
    }
    impl->table  = &table;
    impl->device = device;
    impl->memory = memory;
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

void ContentImages::destroy() noexcept
{
    if (!impl) {
        return;
    }
    if (impl->device != VK_NULL_HANDLE && impl->table != nullptr) {
        for (auto& [texture, entry] : impl->textures) {
            (void)texture;
            impl->destroyEntry(entry);
        }
        impl->textures.clear();
        for (Impl::Entry& entry : impl->retired) {
            impl->destroyEntry(entry);
        }
        impl->retired.clear();
        for (auto& [buffer, memory] : impl->staging) {
            impl->destroyBuffer(buffer, memory);
        }
        impl->staging.clear();
        for (auto& [buffer, memory] : impl->fallback_staging) {
            impl->destroyBuffer(buffer, memory);
        }
        impl->fallback_staging.clear();
        for (auto& [sampler, handle] : impl->samplers) {
            (void)sampler;
            if (handle != VK_NULL_HANDLE) {
                impl->table->vkDestroySampler(impl->device, handle, nullptr);
            }
        }
        impl->samplers.clear();
        if (impl->fallback_view != VK_NULL_HANDLE) {
            impl->table->vkDestroyImageView(impl->device, impl->fallback_view, nullptr);
            impl->fallback_view = VK_NULL_HANDLE;
        }
        if (impl->fallback_image != VK_NULL_HANDLE) {
            impl->table->vkDestroyImage(impl->device, impl->fallback_image, nullptr);
            impl->fallback_image = VK_NULL_HANDLE;
        }
        if (impl->fallback_memory != VK_NULL_HANDLE) {
            impl->table->vkFreeMemory(impl->device, impl->fallback_memory, nullptr);
            impl->fallback_memory = VK_NULL_HANDLE;
        }
        if (impl->fallback_sampler != VK_NULL_HANDLE) {
            // The fallback sampler is one of `samplers` (created through samplerFor), so it was destroyed with them.
            impl->fallback_sampler = VK_NULL_HANDLE;
        }
    }
    impl->table  = nullptr;
    impl->device = VK_NULL_HANDLE;
    impl->memory = VkPhysicalDeviceMemoryProperties{};
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
    return out;
}

bool ContentImages::releaseTexture(const vn::graphics::Texture& texture)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return false;
    }
    const auto found = impl->textures.find(&texture);
    if (found == impl->textures.end()) {
        return true; // nothing was resident: already released
    }
    impl->bytes_total -= found->second.bytes;
    // NOT INTO `retired`: that list exists for images a frame replaced, whose copies may still be in flight. This
    // call runs between frames, where the device has waited on its fence, so nothing names the image any more.
    impl->destroyEntry(found->second);
    impl->textures.erase(found);
    ++impl->released;
    return true;
}

void ContentImages::releaseFrame() noexcept
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return;
    }
    for (Impl::Entry& entry : impl->retired) {
        impl->destroyEntry(entry);
    }
    impl->retired.clear();
    // The frame's command buffer has completed (the device waited on its fence), so the copies it recorded are done
    // and the buffers they read may go - the same release the prepare path performs right after ITS fence wait.
    releaseUploadStaging();
}

void ContentImages::releaseUploadStaging() noexcept
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return;
    }
    for (auto& [buffer, memory] : impl->staging) {
        impl->destroyBuffer(buffer, memory);
    }
    impl->staging.clear();
    for (auto& [buffer, memory] : impl->fallback_staging) {
        impl->destroyBuffer(buffer, memory);
    }
    impl->fallback_staging.clear();
}

bool ContentImages::hasPendingUploads() const noexcept
{
    if (!impl) {
        return false;
    }
    // THE STAGING BUFFERS ARE THE LEDGER: they exist from "a copy was recorded" until "that copy completed".
    return !impl->staging.empty() || !impl->fallback_staging.empty();
}

VkImageView ContentImages::viewFor(const vn::graphics::Texture& texture, const VkCommandBuffer commands, vn::String* why)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return VK_NULL_HANDLE;
    }
    if (!texture.isComplete()) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a material texture is still being filled (an incomplete texture is not sampled)");
        }
        return VK_NULL_HANDLE;
    }

    const auto found = impl->textures.find(&texture);
    if (found != impl->textures.end() && found->second.revision == texture.revision()) {
        return found->second.view;
    }
    // A NEW revision needs a copy, and a copy may not be recorded inside a render pass: the device prepared the pass'
    // textures first (preparePass). Reaching here without a command buffer means that step was skipped.
    if (commands == VK_NULL_HANDLE) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: a material texture was not prepared before its render pass began");
        }
        return VK_NULL_HANDLE;
    }

    Impl::Entry entry;
    entry.texture  = vn::intrusive_ptr<const vn::graphics::Texture>(&texture);
    entry.revision = texture.revision();
    if (!impl->createTextureImage(texture, entry, why)) {
        return VK_NULL_HANDLE;
    }

    // The image starts UNDEFINED; a fresh entry is uploaded whole, while a REVISION replacement is a new image (the
    // old one is retired below), so both take the same path.
    if (!impl->recordUpload(texture, entry, commands, why)) {
        impl->destroyEntry(entry);
        return VK_NULL_HANDLE;
    }

    const ImageState transfer = transferWriteState();
    const ImageState sampled  = stateOf(vn::graphics::backend::ImageLayout::ShaderReadOnly);
    // THE SAME RANGE THE UPLOAD WRITES (every layer and mip): a single-subresource transition here would leave the
    // faces the descriptor samples in TRANSFER_DST, which is the layout error the app measured.
    VkImageSubresourceRange whole{};
    whole.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    whole.baseMipLevel   = 0U;
    whole.levelCount     = static_cast<std::uint32_t>(texture.mipCount());
    whole.baseArrayLayer = 0U;
    whole.layerCount     = static_cast<std::uint32_t>(texture.layerCount());
    const VkImageMemoryBarrier to_sampled = barrierBetween(entry.image, transfer, sampled, whole);
    impl->table->vkCmdPipelineBarrier(commands,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      0U,
                                      0U,
                                      nullptr,
                                      0U,
                                      nullptr,
                                      1U,
                                      &to_sampled);

    // THE BOOKS: a REPLACED revision is a rebuild (its bytes leave the total, the entry never stopped being
    // resident), a fresh upload is a build.
    ++impl->built;
    impl->uploaded_bytes += entry.bytes;
    impl->bytes_total += entry.bytes;
    if (found != impl->textures.end()) {
        impl->bytes_total -= found->second.bytes;
        impl->retired.push_back(std::move(found->second));
        found->second = std::move(entry);
    }
    else {
        impl->textures.emplace(&texture, std::move(entry));
    }
    return impl->textures.at(&texture).view;
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

bool ContentImages::ensureFallback(const VkCommandBuffer commands, vn::String* why)
{
    if (!impl || impl->device == VK_NULL_HANDLE || impl->table == nullptr) {
        return false;
    }
    if (impl->fallback_view != VK_NULL_HANDLE) {
        return true;
    }
    if (commands == VK_NULL_HANDLE) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image was not prepared before the render pass began");
        }
        return false;
    }

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
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    if (impl->table->vkCreateImage(impl->device, &info, nullptr, &impl->fallback_image) != VK_SUCCESS) {
        impl->fallback_image = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image could not be created");
        }
        return false;
    }

    VkMemoryRequirements requirements{};
    impl->table->vkGetImageMemoryRequirements(impl->device, impl->fallback_image, &requirements);
    std::uint32_t memory_type = 0;
    if (!findMemoryType(impl->memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory_type)) {
        impl->table->vkDestroyImage(impl->device, impl->fallback_image, nullptr);
        impl->fallback_image = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: no device-local memory type serves the 1x1 fallback image");
        }
        return false;
    }

    VkMemoryAllocateInfo allocate{};
    allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize  = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    if (impl->table->vkAllocateMemory(impl->device, &allocate, nullptr, &impl->fallback_memory) != VK_SUCCESS ||
        impl->table->vkBindImageMemory(impl->device, impl->fallback_image, impl->fallback_memory, 0U) != VK_SUCCESS) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image's memory could not be prepared");
        }
        return false;
    }

    VkBuffer       staging_buffer{ VK_NULL_HANDLE };
    VkDeviceMemory staging_memory{ VK_NULL_HANDLE };
    if (!impl->createHostBuffer(4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging_buffer, staging_memory)) {
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback's staging buffer could not be created");
        }
        return false;
    }
    void* mapped = nullptr;
    if (impl->table->vkMapMemory(impl->device, staging_memory, 0U, 4U, 0U, &mapped) != VK_SUCCESS || mapped == nullptr) {
        impl->destroyBuffer(staging_buffer, staging_memory);
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback's staging buffer could not be mapped");
        }
        return false;
    }
    const std::uint8_t white[4]{ 255U, 255U, 255U, 255U };
    std::memcpy(mapped, white, sizeof(white));
    impl->table->vkUnmapMemory(impl->device, staging_memory);

    const ImageState transfer = transferWriteState();
    const VkImageMemoryBarrier to_transfer =
        barrierBetween(impl->fallback_image, stateOf(vn::graphics::backend::ImageLayout::Undefined), transfer, VK_IMAGE_ASPECT_COLOR_BIT);
    impl->table->vkCmdPipelineBarrier(commands,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      0U,
                                      0U,
                                      nullptr,
                                      0U,
                                      nullptr,
                                      1U,
                                      &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1U;
    region.imageExtent                 = VkExtent3D{ 1U, 1U, 1U };
    impl->table->vkCmdCopyBufferToImage(commands,
                                        staging_buffer,
                                        impl->fallback_image,
                                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        1U,
                                        &region);

    const ImageState sampled = stateOf(vn::graphics::backend::ImageLayout::ShaderReadOnly);
    const VkImageMemoryBarrier to_sampled = barrierBetween(impl->fallback_image, transfer, sampled, VK_IMAGE_ASPECT_COLOR_BIT);
    impl->table->vkCmdPipelineBarrier(commands,
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                      0U,
                                      0U,
                                      nullptr,
                                      0U,
                                      nullptr,
                                      1U,
                                      &to_sampled);
    impl->fallback_staging.emplace_back(staging_buffer, staging_memory);

    VkImageViewCreateInfo view{};
    view.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image                           = impl->fallback_image;
    view.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    view.format                          = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount     = 1U;
    view.subresourceRange.layerCount     = 1U;
    if (impl->table->vkCreateImageView(impl->device, &view, nullptr, &impl->fallback_view) != VK_SUCCESS) {
        impl->fallback_view = VK_NULL_HANDLE;
        if (why != nullptr) {
            *why = vn::String(u8"vk: the 1x1 fallback image's view could not be created");
        }
        return false;
    }
    return true;
}

VkImageView ContentImages::fallbackView() const noexcept
{
    return impl != nullptr ? impl->fallback_view : VK_NULL_HANDLE;
}

VkSampler ContentImages::fallbackSampler() const noexcept
{
    return impl != nullptr ? impl->fallback_sampler : VK_NULL_HANDLE;
}

VN_BACKEND_VK_NS_END
