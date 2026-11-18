#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <volk.h>

#include <vine/backend_vk/backend_vk_global.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/graphics/Sampler.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The numbers and the two buffer verbs the content path's halves share: the ABI as this backend implements it.
 *
 * @details
 * - ONE HOME FOR THE ABI CONSTANTS. The engine's content ABI is stated by the SDK (`kTextureSlotCount`, the shadow
 *   bindings, the full-screen binding order) and this backend spells the deviceless half of it here, so the cache side
 *   (which packs the layouts) and the frame scratch (which builds the descriptor sets) cannot drift apart: the set the
 *   scratch writes must agree with the layout the caches created, and both name these numbers.
 * - THE HOST-VISIBLE BUFFER PAIR. Streams (cache side) and the uniform ring (scratch side) both upload into the
 *   device's memory pool the same way, so the two verbs are free functions over borrowed device state rather than a
 *   member of either class - one implementation, and neither side owns the pool.
 *
 * @note This is an internal header: it names Vulkan types, which the plugin's shell headers do not (see
 *       `scripts/check_vk_exposure.py`).
 */
namespace detail {

/// @brief The alignment used when the device reports none (Vulkan's minimum for uniform-buffer offsets).
inline constexpr std::uint32_t kDefaultUniformAlignment = 256U;

/// @brief How many descriptor sets the frame scratch's pool may hold before it is exhausted (see FrameScratch::sets).
///
/// A set is a BINDING OF RESOURCES now, not a draw: the per-draw variation is a dynamic offset, so the cache holds
/// one set per (texture set, shadow map) and a scene with hundreds of materials still fits comfortably.
inline constexpr std::uint32_t kMaxCachedSets = 1024U;

/// @brief The ring's first capacity: 256 KiB of constants is ~650 draws of one slice each (see FrameScratch::ring).
inline constexpr VkDeviceSize kInitialRingBytes = 256U * 1024U;

/// @brief Frames a cached set survives unused before it is freed (the same spirit as the reference backend's window).
inline constexpr std::uint64_t kSetRetentionFrames = 600U;

/// @brief How often unused sets are swept (a frame count; sweeping every frame would be pointless work).
inline constexpr std::uint64_t kSetSweepIntervalFrames = 120U;

/// @brief The content ABI's texture-slot count (see `TextureSlot`), i.e. set 0 bindings 1..5.
inline constexpr std::uint32_t kTextureSlots = vn::graphics::kTextureSlotCount;

/// @brief The content set's SHADOW bindings: the engine's forward ABI puts the map at 7 and its block at 8.
///
/// The engine's `forwardFragmentSource()` declares both UNCONDITIONALLY (its `vn_shadow.params.x` is the runtime
/// switch), so a backend that wants its own forward programs must offer the binding even while it casts no shadow.
inline constexpr std::uint32_t kShadowMapBinding   = 7U;
inline constexpr std::uint32_t kShadowBlockBinding = 8U;

/// @brief The full-screen ABI's sampler bindings: bindings 0..4 (four colour attachments and depth).
inline constexpr std::uint32_t kScreenSamplerBindings = 5U;

/**
 * @brief How a shadow map is read: nearest texels, clamped to the edge, no mip interpolation.
 *
 * @details
 * - THE ENGINE'S TERM COMPARES ONE TEXEL (see BuiltinShaders' shadow term): filtering would average a caster's depth
 *   with the surface behind it and move every silhouette, so the map is read exactly. The reference backend states the
 *   same three fields for the same reason (`ContentPipeline::depthSampler`), which is what makes the two backends
 *   sample the same value.
 * - Clamp rather than repeat is the other half: map coordinates are clamped into the rectangle by the shader's own
 *   range test, and a wrapped read would sample the opposite edge if that test ever disagreed with the sampler.
 */
inline constexpr vn::graphics::Sampler kShadowSampler{
    .min_filter   = vn::graphics::Sampler::Filter::Nearest,
    .mag_filter   = vn::graphics::Sampler::Filter::Nearest,
    .mipmap_mode  = vn::graphics::Sampler::MipmapMode::Nearest,
    .address_u    = vn::graphics::Sampler::AddressMode::ClampToEdge,
    .address_v    = vn::graphics::Sampler::AddressMode::ClampToEdge,
    .address_w    = vn::graphics::Sampler::AddressMode::ClampToEdge,
};

/**
 * @brief The 128-byte push range the engine's content programs read (`vine_params.glsl`).
 *
 * @details
 * - `pc.projection == VineViewBlock.proj` (the pass' projection, folded into the device's clip convention) and
 *   `pc.modelView == VineViewBlock.view * VineDrawBlock.model`: the same two values a block-reading program finds in
 *   the view/draw pair, spelled for the range the built-in stages declare.
 * - IT LIVES HERE, WITH THE ABI: the caches size the pipeline layout from it and the recorder fills it, so the two
 *   cannot disagree about how wide the range is.
 * - THE NAME IS THE SDK'S: the range was called `ContentPush` before B17 renamed it `PushRange`, and this block is
 *   that range's layout. `PushConstants` was not used because `::vsg::PushConstants` names a different thing in the
 *   same TUs (`G1`: one concept, one word).
 */
struct PushRangeConstants {
    std::array<float, 16> projection{}; // View -> clip, column-major, in the device's clip convention.
    std::array<float, 16> model_view{}; // World -> view, column-major (the pass' view times the drawable's model).
};
static_assert(sizeof(PushRangeConstants) == 128U, "the push range is two mat4");

/**
 * @brief Rounds @p value up to the next multiple of @p alignment.
 *
 * @param value     Value to round.
 * @param alignment Multiple to round to (0 is treated as 1).
 * @return The rounded value.
 */
[[nodiscard]] inline VkDeviceSize alignedUp(VkDeviceSize value, VkDeviceSize alignment) noexcept
{
    const VkDeviceSize step = alignment != 0U ? alignment : 1U;
    return ((value + step - 1U) / step) * step;
}

/**
 * @brief Gets the vertex format for a channel's component count.
 *
 * @param components Scalars per vertex (1..4).
 * @return The `VkFormat`, or `VK_FORMAT_UNDEFINED` for a count this backend cannot serve.
 */
[[nodiscard]] inline VkFormat vertexFormatOf(std::uint32_t components) noexcept
{
    switch (components) {
    case 1: return VK_FORMAT_R32_SFLOAT;
    case 2: return VK_FORMAT_R32G32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32_SFLOAT;
    case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/**
 * @brief Creates a buffer and binds a host-visible coherent slice of @p allocator to it.
 *
 * @details
 * - THE POOL'S MAPPING IS PERSISTENT (see MemoryAllocator::map): coherent memory needs no flush, so a caller writes
 *   through the address the pool answers instead of mapping and unmapping around one memcpy.
 * - A failure leaves nothing behind: the buffer is destroyed before this answers false.
 *
 * @param table     The device's entry points.
 * @param device    The logical device.
 * @param allocator The memory pool the slice comes from.
 * @param size      Bytes to allocate (must be > 0).
 * @param usage     Buffer usage flags.
 * @param buffer    Receives the buffer.
 * @param memory    Receives the pool range the buffer is bound into.
 * @return true when both exist.
 */
[[nodiscard]] inline bool createHostBuffer(const VolkDeviceTable& table,
                                           const VkDevice         device,
                                           MemoryAllocator&       allocator,
                                           const VkDeviceSize     size,
                                           const VkBufferUsageFlags usage,
                                           VkBuffer&              buffer,
                                           MemorySlice&           memory)
{
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (table.vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    table.vkGetBufferMemoryRequirements(device, buffer, &requirements);

    memory = allocator.allocate(requirements, MemoryUsage::HostVisible, MemoryKind::Linear);
    if (!memory.valid()) {
        table.vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    if (table.vkBindBufferMemory(device, buffer, memory.memory, memory.offset) != VK_SUCCESS) {
        allocator.release(memory);
        table.vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/**
 * @brief Destroys a buffer and gives its slice back to the pool (idempotent).
 *
 * @note IMMEDIATE, ON PURPOSE: this is the TEARDOWN and REPLACEMENT path, both of which run while nothing that could
 *       name the buffer is in flight (a teardown follows the device's own idle; a replacement happens while the
 *       current frame is still being recorded, before it is submitted). A *released* entry goes through the owner's
 *       `releaseLater` instead - and when frames overlap across sessions, a replacement must go the same way (see
 *       `.ai/design/graphics-sessions.md`, S6).
 *
 * @param table     The device's entry points.
 * @param device    The logical device.
 * @param allocator The memory pool the slice came from.
 * @param buffer    Buffer to destroy.
 * @param memory    The slice to release.
 */
inline void destroyBuffer(const VolkDeviceTable& table,
                          const VkDevice         device,
                          MemoryAllocator&       allocator,
                          VkBuffer&              buffer,
                          MemorySlice&           memory) noexcept
{
    if (buffer != VK_NULL_HANDLE) {
        table.vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
    }
    allocator.release(memory);
}

} // namespace detail

VN_BACKEND_VK_NS_END
