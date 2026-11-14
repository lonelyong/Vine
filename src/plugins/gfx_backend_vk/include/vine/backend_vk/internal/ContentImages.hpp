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
 * - THE UPLOAD IS RECORDED, NOT SUBMITTED BY THIS CACHE. A texture is copied on a command buffer the CALLER records
 *   the first time a draw samples it (or when its revision moved), because this backend records and submits one frame
 *   synchronously - there is no upload queue to hand it to. Two callers exist, and they are the same copies: the
 *   frame's own command buffer (recorded outside each render pass, see `ContentDraw::preparePass`), and the ONE
 *   transfer-only command buffer a warm-up submits of its own (see `RenderFrameExecutor::prepareFrame`). Staging
 *   buffers therefore stay alive until the command buffer that recorded them has completed: the frame path lets go
 *   through @ref releaseFrame after the queue is idle, the prepare path through @ref releaseUploadStaging right after
 *   its own fence wait. @ref releaseFrame also destroys the images a revision replaced.
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
     * @brief Destroys the staging buffers of every upload recorded SO FAR, leaving the images they fed resident.
     *
     * @details
     * - WHAT IT RELEASES: `staging` (the material textures) and `fallback_staging` (the 1x1 stand-in), and nothing
     *   else - the images stay, because they are what the copies were for.
     * - WHY THE SPLIT FROM @ref releaseFrame: the frame path and the prepare path record the same copies on different
     *   command buffers, so each has to let go at the moment ITS buffer completed. A warm-up calls this right after
     *   its own fence wait; a frame reaches it through @ref releaseFrame, after the queue is idle.
     *
     * @warning
     * - Precondition: the command buffer that recorded those copies has completed. Destroying a staging buffer a
     *   pending copy still reads is a use-after-free the driver cannot catch.
     */
    void releaseUploadStaging() noexcept;

    /**
     * @brief Gets whether a copy has been recorded into a command buffer that has not been waited for yet.
     *
     * @details
     * - THE MECHANISM IS THE STAGING BUFFERS' OWN LIFETIME: they are created when a copy is recorded and destroyed
     *   when that copy completed, so "one is alive" and "an upload is in flight" are the same fact. This is what
     *   lets a caller answer "is there anything to submit?" without opening a command buffer first (see
     *   `RenderFrameExecutor::prepareFrame`: no work means no submit).
     *
     * @return true when a recorded upload has not been released yet.
     */
    [[nodiscard]] bool hasPendingUploads() const noexcept;

    /**
     * @brief What this cache has built, what it let go and what it holds now.
     *
     * @details
     * - CUMULATIVE, EXCEPT THE TWO `live` FIELDS: the frame's own numbers are the difference of two cumulative ones
     *   (see FrameUploads), while residency is a question about NOW (see ResidencyLedger).
     * - A REVISION THE NEW ONE REPLACED IS A REBUILD, NOT A RELEASE: it leaves `live_bytes` and adds to `builds`,
     *   because the entry never stopped being resident - it was replaced. `releases` counts entries a HOST release
     *   let go, which is the same rule the stream table follows.
     */
    struct Progress {
        std::uint64_t builds{ 0 };     ///< Texture images built over this cache's lifetime.
        std::uint64_t bytes{ 0 };      ///< Bytes those images copied over its lifetime.
        std::uint64_t releases{ 0 };   ///< Entries a host release let go (see the class note).
        std::uint32_t live{ 0 };       ///< Texture images resident NOW.
        std::uint64_t live_bytes{ 0 }; ///< GPU bytes those images occupy NOW (the bytes they were uploaded from).
    };

    /** @brief Gets the numbers (see Progress). */
    [[nodiscard]] Progress progress() const noexcept;

    /**
     * @brief Drops @p texture's uploaded image, if there is one, so the next frame that samples it uploads it again.
     *
     * @details
     * - DESTROYED, NOT PARKED: this device records and submits one frame synchronously and waits on its fence, so
     *   between frames nothing is in flight and an image nothing names can go immediately. Call it between frames,
     *   on the frame thread - the same contract the SDK's release verbs state.
     * - The entry keeps nothing: a texture released here is resident again on the first frame that samples it, and
     *   that re-upload is what the release costs.
     *
     * @param texture Texture whose image leaves the cache (borrowed; the host keeps the object).
     * @return true when the cache answered - a texture with no entry is already released, which is the same true.
     */
    bool releaseTexture(const vn::graphics::Texture& texture);

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
