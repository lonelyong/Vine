#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/graphics/Sampler.hpp>
#include <vine/graphics/Texture.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The content path's sampled images: `Texture` objects uploaded once per revision, and the samplers that read
 *        them.
 *
 * @details
 * - WHAT IT OWNS. A `VkImage` + `VkDeviceMemory` + `VkImageView` per texture REVISION (keyed by the texture's address,
 *   with a strong reference held so that address cannot be recycled by a later object), a `VkSampler` per distinct
 *   `Sampler` value, and the 1x1 white image every absent material slot is bound to.
 * - THE UPLOAD IS RECORDED, NOT SUBMITTED. A texture is copied on the frame's own command buffer the first time a
 *   draw samples it (or when its revision moved), because this backend records and submits one frame synchronously -
 *   there is no upload queue to hand it to. Staging buffers therefore stay alive until @ref releaseFrame, which the
 *   device calls after the queue is idle; the same call destroys the images a revision replaced.
 * - WHAT "ABSENT" MEANS. A slot whose texture is null, empty or incomplete is not uploaded; the caller binds
 *   @ref fallbackView with @ref fallbackSampler and clears the material's slot bit, so the shader never reads a
 *   binding nobody wrote (the slot bits are the shading's own switch - see ShaderAbi.hpp).
 * - A texture that cannot be represented (a depth format, say) is reported once through @p why and treated as absent;
 *   the rest of the draw still happens.
 *
 * @note Not thread-safe: one frame at a time, on the thread that records it (the backend's contract).
 */
class VN_BACKEND_VK_API ContentImages final {
  public:
    /** @brief Builds an empty cache; nothing exists until @ref create. */
    ContentImages();

    /** @brief Releases every Vulkan object this cache owns (see @ref destroy). */
    ~ContentImages();

    ContentImages(const ContentImages&)            = delete;
    ContentImages& operator=(const ContentImages&) = delete;

  public:
    /**
     * @brief Creates the sampler cache and the fallback sampler.
     *
     * @param table  The device's entry points (volk table).
     * @param device The logical device.
     * @param memory The device's memory properties (for the device-local image search).
     * @return true when the fallback sampler exists.
     */
    [[nodiscard]] bool create(const VolkDeviceTable& table, VkDevice device, const VkPhysicalDeviceMemoryProperties& memory);

    /** @brief Destroys every image, view, sampler and staging buffer this cache owns (idempotent). */
    void destroy() noexcept;

    /**
     * @brief Destroys what the PREVIOUS frame left behind: staging buffers and images a revision replaced.
     *
     * @details
     * - Called by the device after the queue has gone idle, which is what makes "the command buffer that copied this
     *   texture has finished" true. Until then the objects stay alive even though nothing caches them any more.
     */
    void releaseFrame() noexcept;

    /**
     * @brief Gets the view of @p texture, recording its upload into @p commands when it is new or changed.
     *
     * @details
     * - The texture must be complete (see `Texture::isComplete`); an incomplete one, or one whose pixel format has no
     *   Vulkan colour spelling, answers null and sets @p why.
     * - The upload is recorded on the frame's command buffer before the draw that samples it; the image is left in
     *   `SHADER_READ_ONLY_OPTIMAL`.
     *
     * @param texture  The texture to sample.
     * @param commands The command buffer the current frame is being recorded into.
     * @param why      Receives the reason when no view can be served, or null to ignore.
     * @return The view, or `VK_NULL_HANDLE`.
     */
    [[nodiscard]] VkImageView viewFor(const vn::graphics::Texture& texture, VkCommandBuffer commands, vn::String* why);

    /**
     * @brief Gets (or creates) the sampler for @p sampler.
     *
     * @param sampler The read state (a value: equal state is one sampler).
     * @return The sampler, or `VK_NULL_HANDLE` when it could not be created.
     */
    [[nodiscard]] VkSampler samplerFor(const vn::graphics::Sampler& sampler);

    /**
     * @brief Creates and uploads the 1x1 white fallback image on first use.
     *
     * @param commands The command buffer the current frame is being recorded into.
     * @param why      Receives the reason when the fallback cannot be built, or null to ignore.
     * @return true when @ref fallbackView serves a view.
     */
    [[nodiscard]] bool ensureFallback(VkCommandBuffer commands, vn::String* why);

    /** @brief Gets the 1x1 white view every absent slot is bound to (null until @ref ensureFallback succeeded). */
    [[nodiscard]] VkImageView fallbackView() const noexcept;

    /** @brief Gets the sampler the fallback and absent slots are read with. */
    [[nodiscard]] VkSampler fallbackSampler() const noexcept;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
