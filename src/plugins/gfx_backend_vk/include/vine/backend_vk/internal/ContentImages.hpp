#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstdint>
#include <functional>
#include <memory>

#include <volk.h>

#include <vine/String.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/graphics/Sampler.hpp>
#include <vine/graphics/Texture.hpp>

VN_BACKEND_VK_NS_BEGIN

struct RecordingTally;

/**
 * @brief The content path's sampled images: `Texture` objects uploaded once per revision, and the samplers that read
 *        them.
 *
 * @details
 * - WHAT IT OWNS. A `VkImage`, the `MemorySlice` it is bound into and a `VkImageView` per texture REVISION (keyed by the
 *   texture's address, with a strong reference held so that address cannot be recycled by a later object), a
 *   `VkSampler` per distinct `Sampler` value, and the 1x1 white image every absent material slot is bound to. The
 *   memory itself is the device pool's (see MemoryAllocator), not this cache's.
 * - THE UPLOAD IS RECORDED, NOT SUBMITTED BY THIS CACHE. A texture is copied on a command buffer the CALLER records
 *   the first time a draw samples it (or when its revision moved), because this backend records and submits one frame
 *   synchronously - there is no upload queue to hand it to. Two callers exist, and they are the same copies: the
 *   frame's own command buffer (recorded outside each rendering instance, at the frame head), and the ONE transfer-only
 *   command buffer a warm-up submits of its own (see `RenderFrameExecutor::prepareFrame`). Staging buffers therefore
 *   stay alive until the command buffer that recorded them has completed - and each driver's are grouped in its own
 *   batch (@ref BatchId), so the frame that ends first never frees the staging of the frame that is still recording.
 * - PREPARED, THEN RECORDED (`G3` / `G4` / `G10`; review 19 `Z15`). An upload has two halves with different contracts.
 *   The PREPARE half builds every object the copy names - the image, its memory, its view and ONE host-visible buffer
 *   holding every layer and mip with the regions that read it - and may fail: it records no command, so a refusal
 *   destroys what it built and leaves the caller's command buffer exactly as it found it. The RECORD half states the
 *   two transitions and the copy, hands the staging buffer to the batch, and ANSWERS NOTHING - the signature is the
 *   rule (`G4`), because a command the driver already holds cannot be taken back.
 *   What that forbids is the one destroy Vulkan cannot forgive: an object destroyed while a RECORDED command still
 *   names it. Every destroy this cache performs is either a prepare-phase refusal (no command names anything) or a
 *   release through the batch's books (@ref batchAbandoned / `ParkFn`).
 * - ONE STAGING BUFFER PER UPLOAD, NOT ONE PER LAYER: one allocation, one mapping and one `vkCmdCopyBufferToImage` for
 *   a cube map's six faces, and a refusal that costs one object instead of "the faces built so far".
 * - AN IMAGE BECOMES RESIDENT WHEN ITS COPY IS PROVEN, NOT WHEN ITS COPY IS RECORDED (`G6` / `G10`). The two kinds of
 *   content this backend fills from GPU work follow ONE rule: a geometry publishes on the frame's fence
 *   (`ContentCaches::FrameBuilds`), and a texture is PENDING in the batch that recorded it until that batch completes
 *   (@ref batchCompleted). Recording is not evidence - a frame whose submit failed would otherwise leave a resident
 *   entry whose image is still `UNDEFINED` and never re-uploaded (`T66`), and another driver's frame head would bind a
 *   view whose copy had not even been submitted. A competitor that wants the texture meanwhile copies it for itself,
 *   exactly as it does for a geometry (`ContentCaches::FrameBuilds`).
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
    /**
     * @brief What the device does with an object that a frame may still name: park it, or destroy it now.
     *
     * @details
     * - THE DEVICE DECIDES, THIS CACHE REPORTS (`G4`): whether an image may be destroyed is a question about the
     *   device's completion watermark AND about who still names the entry, and the answer needs the clock and the park
     *   queue - both of which belong to the device, not to a cache. The seam is a function so this header keeps naming
     *   neither.
     * - `named` IS THE SECOND WINDOW: a weak view of the entry the act is about to destroy. The park keeps the act
     *   alive (the act holds the entry), asks `use_count` when its turn comes, and re-parks while a frame still holds a
     *   share it got from `viewFor` - so a release announced while another driver records neither waits nor destroys.
     */
    using ParkFn = std::function<void(std::weak_ptr<const void> named, std::function<void()> destroy)>;

    /**
     * @brief One served view, and the share that keeps the image behind it alive while a frame names it.
     *
     * @details
     * - THE SHARE IS THE RESIDENCY-BORROWING RULE (ADR-2026-11-24): a release ends residency, not the objects, so a
     *   frame that resolved a view keeps the entry until the frame ends. The frame stores `owner` in its own book
     *   (`ContentCaches::FrameBuilds::borrow`) and records the raw handle it copied - so the park's `use_count` probe
     *   answers "is any frame still naming this?"
     */
    struct TextureView {
        VkImageView                 view{ VK_NULL_HANDLE }; ///< The image's view, or the 1x1 stand-in.
        std::shared_ptr<const void> owner{};                ///< The entry to keep; empty when no view was served.
    };

    /**
     * @brief One driver's upload bookkeeping: the staging buffers it recorded and the images its revisions replaced.
     *
     * @details
     * - A BATCH IS A FRAME DRIVER (`C2`), and a frame and its warm-up are the same driver: the staging buffer of one
     *   driver's uploads may be destroyed the moment THAT driver's submission completed, and no sooner - releasing it
     *   with another driver's frame end is the use-after-free two concurrent frames would otherwise hand the driver.
     * - 0 IS "NOBODY": a driver's id comes from the device (see `Device::FrameDriver::uploadBatch`), so a real batch is
     *   never 0.
     */
    using BatchId = std::uint64_t;

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
     * @param table     The device's entry points (volk table).
     * @param device    The logical device.
     * @param allocator The device's memory pool (every image and staging buffer asks it for a slice).
     * @return true when the fallback sampler exists.
     */
    [[nodiscard]] bool create(const VolkDeviceTable& table, VkDevice device, MemoryAllocator& allocator);

    /** @brief Destroys every image, view, sampler and staging buffer this cache owns (idempotent). */
    void destroy() noexcept;

    /**
     * @brief Sets the largest anisotropy factor the device honours (1 = filtering is unavailable).
     *
     * @details
     * - `Sampler::anisotropy` is a request and this is the limit it is clamped to, which is also what the
     *   capabilities report as `max_anisotropy`: a sampler built above it is an invalid create, and answering a
     *   quality setting with silence is worse than answering it with the device's best.
     *
     * @param max_anisotropy The device's `maxSamplerAnisotropy`, or 1 when the feature is not enabled.
     */
    void setMaxAnisotropy(float max_anisotropy) noexcept;

    /**
     * @brief Hands this cache the device's park queue and completion clock, so a released image can be deferred.
     *
     * @details
     * - THE SAME SEAM `ContentCaches::setRetirement` STATES, for the images that cache owns: a released texture leaves the
     *   table at once (the next frame uploads it again), and the `VkImage` behind it is destroyed once the device's
     *   watermark has passed every frame that could still sample it.
     *
     * @param park What to do with an object that a frame may still name (empty destroys immediately).
     */
    void setRetirement(ParkFn park);

    /**
     * @brief Marks one driver's upload batch COMPLETE: every copy its command buffer recorded becomes resident.
     *
     * @details
     * - THE ONE PUBLICATION POINT (see the class note), and it is the driver's own fence that earns it: called after
     *   the submission was waited for, so "the command buffer that copied this texture has finished" is true.
     * - WHAT IT DOES, in one pass over the batch: promotes its PENDING entries into the table, parks the resident entry
     *   each promotion replaces (a revision replacement), destroys the staging buffers the copies read, and parks the
     *   images an earlier replacement retired. Another driver's batch is untouched - its command buffer may still be
     *   recording.
     * - A SECOND DRIVER THAT COPIED THE SAME REVISION WHILE THIS ONE WAS IN FLIGHT promotes its own copy later, and the
     *   first one is parked then: two copies, one survivor, which is the cost the alternative (waiting for another
     *   driver's frame) would charge every time.
     *
     * @param batch The driver whose submission completed (see @ref BatchId).
     */
    void batchCompleted(BatchId batch) noexcept;

    /**
     * @brief Marks one driver's upload batch ABANDONED: nothing it recorded is published.
     *
     * @details
     * - A FRAME THAT NEVER REACHED THE QUEUE FILLED NOTHING, so its pending entries must not become resident: each
     *   one's image is still in `UNDEFINED` and no copy will ever write it. This is the fix for the class of defect
     *   `T66` records - a texture that read as resident while it was black, and never re-uploaded because its revision
     *   matched.
     * - THE RESIDENT ENTRY IT WOULD HAVE REPLACED STAYS: that revision is the last one proven complete, so the next
     *   frame that samples the texture uses it.
     * - THE OBJECTS ARE PARKED, NOT DESTROYED: a submit that failed may still have executed part of the command buffer
     *   (the safe reading `P17` takes for layout records too), so what the driver could still name waits for the
     *   device's watermark.
     *
     * @param batch The driver whose submission did not complete (see @ref BatchId).
     */
    void batchAbandoned(BatchId batch) noexcept;

    /**
     * @brief Gets whether a copy has been recorded into a command buffer that has not been waited for yet.
     *
     * @details
     * - THE MECHANISM IS THE STAGING BUFFERS' OWN LIFETIME: they are created when a copy is recorded and destroyed
     *   when that copy completed, so "one is alive" and "an upload is in flight" are the same fact. This is what
     *   lets a caller answer "is there anything to submit?" without opening a command buffer first (see
     *   `RenderFrameExecutor::prepareFrame`: no work means no submit).
     * - A BATCH'S PENDING ENTRIES ARE THE SAME FACT SPELLED AS "what is not published yet", which is why this asks
     *   them too: a promotion leaves both empty in the same call.
     *
     * @return true when a recorded upload has not been published yet.
     *
     * @param batch The driver to ask about (see @ref BatchId).
     */
    [[nodiscard]] bool hasPendingUploads(BatchId batch) const noexcept;

    /**
     * @brief What this cache has built, what it let go and what it holds now.
     *
     * @details
     * - CUMULATIVE, EXCEPT THE `live` AND `pending` PAIRS: the frame's own numbers are the difference of two
     *   cumulative ones (see FrameUploads), while residency is a question about NOW (see ResidencyLedger).
     * - A REVISION THE NEW ONE REPLACED IS A REBUILD, NOT A RELEASE: it leaves `live_bytes` and adds to `builds`,
     *   because the entry never stopped being resident - it was replaced. `releases` counts entries a HOST release
     *   let go, which is the same rule the stream table follows.
     * - `PENDING` IS THE OTHER HALF OF THE PUBLICATION RULE and it is deliberately NOT in `live`: a copy recorded by a
     *   frame whose fence has not been waited for is not resident yet, and a residency report that counted it would be
     *   the number `T66` was about.
     */
    struct Progress {
        std::uint64_t builds{ 0 };        ///< Texture images built over this cache's lifetime.
        std::uint64_t bytes{ 0 };         ///< Bytes those images copied over its lifetime.
        std::uint64_t releases{ 0 };      ///< Entries a host release or a replaced revision let go.
        std::uint32_t live{ 0 };          ///< Texture images RESIDENT now (their copy is proven complete).
        std::uint64_t live_bytes{ 0 };    ///< GPU bytes those images occupy now (the bytes they were uploaded from).
        std::uint32_t pending{ 0 };       ///< Copies recorded and NOT proven yet (see @ref batchCompleted).
        std::uint64_t pending_bytes{ 0 }; ///< The bytes those pending images were uploaded from.
    };

    /** @brief Gets the numbers (see Progress). */
    [[nodiscard]] Progress progress() const noexcept;

    /**
     * @brief Drops @p texture's uploaded image, if there is one, so the next frame that samples it uploads it again.
     *
     * @details
     * - THE ENTRY LEAVES THE TABLE NOW; THE OBJECTS FOLLOW WHEN THE DEVICE SAYS THEY MAY: the next frame that samples
     *   the texture uploads it again (that re-upload is what the release costs), while a frame that already resolved
     *   the view keeps its share, so the image is destroyed only once no frame names it and the submission watermark
     *   has passed it (see `ParkFn`).
     *
     * @param texture Texture whose image leaves the cache (borrowed; the host keeps the object).
     * @return true when the cache answered - a texture with no entry is already released, which is the same true.
     */
    bool releaseTexture(const vn::graphics::Texture& texture);

    /**
     * @brief Drops every image whose TEXTURE nobody but this cache holds any more: the texture half of the frame-end
     *        ownership sweep.
     *
     * @details
     * - THE PROBE IS THE ENTRY'S OWN SHARE (see `Entry::texture`): the entry holds the texture so its address cannot be
     *   recycled under the key, which also makes it an owner - so `useCount() == 1` means this entry is the last holder
     *   and no host can ever ask for that image again. A material that names the texture holds a share of its own
     *   (`TextureSlotBinding::texture`), and a pass' imported input does too (`ImageInput::byTexture`), so live content
     *   keeps its images without this sweep having to know which kind of content it is.
     * - WHY IT MUST EXIST. `releaseTexture` names the object, so a host that drops the last reference to a texture (a
     *   document closing, a thumbnail replaced) has no way to name it any more - and without this arm the image, its
     *   device memory and the host's own pixel data stay resident for the session.
     * - WHERE: the frame's end, once, beside the stream / geometry / program sweeps (see `ContentCaches::endFrame`).
     *   O(#resident textures) - one share count each.
     *
     * @return How many images were withdrawn.
     */
    std::uint32_t sweepUnheldTextures() noexcept;

    /**
     * @brief Gets the view of @p texture, recording its upload into @p commands when it is new or changed.
     *
     * @details
     * - The texture must be complete (see `Texture::isComplete`); an incomplete one, or one whose pixel format has no
     *   Vulkan colour spelling, answers no view and sets @p why.
     * - The upload is recorded on the frame's command buffer before the draw that samples it; the image is left in
     *   `SHADER_READ_ONLY_OPTIMAL` BY THAT COMMAND BUFFER. The entry is PENDING in @p batch until it completes (see
     *   @ref batchCompleted): the view serves the frame that recorded it, and no other driver until the copy is
     *   proven - a competitor records its own copy, which is the cost of never binding an image nobody filled.
     * - THE ANSWER CARRIES THE SHARE (see `TextureView`): the caller keeps it for the frame's life, which is what lets
     *   a host release the texture while this frame is still recording.
     * - THE UPLOAD IS PREPARED BEFORE IT IS RECORDED (see the class note): an image that cannot be built, or bytes that
     *   cannot be staged, answer no view and leave @p commands untouched - so the caller's frame submits a command
     *   buffer that names nothing this cache then destroyed.
     *
     * @param texture  The texture to sample.
     * @param commands The command buffer the current frame is being recorded into.
     * @param batch    The driver recording the copy (see @ref BatchId).
     * @param why      Receives the reason when no view can be served, or null to ignore.
     * @param tally    Receives the barriers the upload recorded (see `RecordingTally`), or null.
     * @return The view and its share (`view == VK_NULL_HANDLE` when nothing can serve it).
     */
    [[nodiscard]] TextureView viewFor(const vn::graphics::Texture& texture, VkCommandBuffer commands, BatchId batch, vn::String* why, RecordingTally* tally = nullptr);

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
     * @param batch    The driver recording the copy (see @ref BatchId).
     * @param why      Receives the reason when the fallback cannot be built, or null to ignore.
     * @param tally    Receives the barriers the upload recorded (see `RecordingTally`), or null.
     * @return The stand-in's view and its share (see `TextureView`); `view == VK_NULL_HANDLE` when it cannot be built.
     */
    [[nodiscard]] TextureView ensureFallback(VkCommandBuffer commands, BatchId batch, vn::String* why, RecordingTally* tally = nullptr);

    /**
     * @brief Gets the 1x1 white view every absent slot is bound to (null until @ref ensureFallback succeeded).
     *
     * @note
     * - THE STAND-IN IS A SHARED CONSTANT, NOT AN ASSET: it is the same one pixel for every pass of every driver, so a
     *   frame that finds it mid-fill binds it anyway rather than dropping every draw that needs it. The one thing that
     *   is NOT shared is its failure: a batch that never ran drops the stand-in so the next frame makes a fresh one
     *   (see @ref batchAbandoned), which is what keeps a black stand-in from outliving the frame that failed to fill it.
     */
    [[nodiscard]] VkImageView fallbackView() const noexcept;

    /** @brief Gets the sampler the fallback and absent slots are read with. */
    [[nodiscard]] VkSampler fallbackSampler() const noexcept;

  private:
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
