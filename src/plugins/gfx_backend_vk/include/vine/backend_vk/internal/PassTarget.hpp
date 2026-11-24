#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

#include <volk.h>

#include <vine/graphics/DrawState.hpp>
#include <vine/backend_vk/internal/DynamicRendering.hpp>

VN_BACKEND_VK_NS_BEGIN

struct TargetObjects;
class WindowTarget;

/**
 * @brief WHERE ONE PASS DRAWS: its attachment set, as the facts and the verbs a pass needs from it.
 *
 * @details
 * - ONE CONCEPT, TWO OWNERS (`G1` / `G12`). A pass writes either an off-screen target's images or the window's
 *   acquired image, and both answer the same four questions - what extent, which pipeline compatibility, what does it
 *   hold already, open/close it - plus the rewind a frame that never ran owes. Before this value the frame body asked
 *   those questions in four `if (window) A else B` pairs, which is how a fifth question gets answered in one of the two
 *   places only.
 * - WHY IT IS A VALUE AND NOT A BASE CLASS. `TargetObjects` is a deliberately plain record (images, memory, layout
 *   records) and `WindowTarget` already inherits the SDK's `WindowPresentTarget`; making either of them inherit an
 *   attachment-set interface would put a vtable in a value type and force one header to know both. A `PassTarget` is
 *   therefore a pass' handle on one of those two owners, and a default-constructed one names nothing.
 * - THE TWO OWNERS DIFFER IN LIFETIME, AND THE HANDLE SAYS SO (`G4` / `G12`). The window is the DEVICE's object: it
 *   lives for the session, so the handle borrows it. An off-screen set is the target TABLE's, and a host may release
 *   that target at any moment - a release does not wait for the frames in flight (R6) - so the handle KEEPS what it
 *   draws into: the record stays alive until the pass (and with it the frame head) is cleared. The park then dates the
 *   images' destruction against the frames that were SUBMITTED, which is the one window this handle cannot cover.
 *   Before this the handle borrowed both and the record survived a frame only because a frame body never runs beside a
 *   submission - an implicit mechanism no type stated.
 * - THE FACTS ARE A SNAPSHOT (`G4`). Extent and compatibility are read when the frame CLAIMS its passes, which is the
 *   same moment `FrameHead` records what the pass will draw into - so a resize that lands later belongs to the next
 *   frame, exactly as a pipeline resolved into the head does.
 * - `holdsPicture()` IS THE ONE QUESTION THE LOAD RULE ASKS (see `resolveAttachmentOps`): whether an earlier pass has put a
 *   picture in these attachments that a later one may keep. A freshly acquired swapchain image answers false - its
 *   content belongs to the presentation engine, not to the host - and the frame records that it has written the image
 *   itself once a window pass has run (see `FrameRecording::window_written`).
 *
 * @warning @ref begin / @ref end / @ref rewind write the owner's layout records MUTABLY, which is why this is a
 *          handle and not an immutable value. An off-screen handle is also one of the owners of its record: a copy of
 *          it keeps the record alive, so a handle that outlives its frame delays the destruction of images the park has
 *          already decided to drop.
 *
 * @see `DynamicRendering` (what @ref begin records), `TargetObjects::beginPass`, `WindowTarget::beginPass`.
 */
class VN_BACKEND_VK_API PassTarget final {
  public:
    /** @brief Builds a value that names nothing (`valid()` answers false). */
    PassTarget() noexcept = default;

  public:
    /**
     * @brief Builds the target an OFF-SCREEN pass draws into, KEEPING the attachment set alive.
     *
     * @details
     * - The record arrives as the build call's own answer (`TargetTable::ensure`), so "this pass may draw here" and
     *   "this record is alive while it does" are one statement and neither can be made without the other.
     *
     * @param images The attachment set; the handle shares ownership of it for as long as the handle lives.
     * @return The target, with its extent and compatibility read now.
     */
    [[nodiscard]] static PassTarget forTarget(std::shared_ptr<TargetObjects> images) noexcept;

    /**
     * @brief Builds the target a WINDOW pass draws into.
     *
     * @param window The window whose acquired image the pass writes (borrowed; owned by the device).
     * @return The target, with the swapchain's extent and compatibility read now.
     */
    [[nodiscard]] static PassTarget forWindow(WindowTarget& window) noexcept;

  public:
    /** @brief Answers whether this value names an attachment set at all. */
    [[nodiscard]] bool valid() const noexcept
    {
        return images_ != nullptr || window_ != nullptr;
    }

    /** @brief Answers whether the set is the window's acquired image rather than an off-screen target. */
    [[nodiscard]] bool isWindow() const noexcept
    {
        return window_ != nullptr;
    }

    /** @brief Gets the extent the rendering instance covers, in pixels. */
    [[nodiscard]] VkExtent2D extent() const noexcept
    {
        return extent_;
    }

    /** @brief Gets the pipeline compatibility (formats, colour count, samples) a pipeline and its instance are stated from. */
    [[nodiscard]] const vn::graphics::RenderPassCompatibility& compatibility() const noexcept
    {
        return compatibility_;
    }

    /** @brief Gets how many colour attachments the set has (the blend rule and the clear-value count). */
    [[nodiscard]] std::uint32_t colorCount() const noexcept
    {
        return static_cast<std::uint32_t>(compatibility_.device_color_formats.size());
    }

    /**
     * @brief Answers whether the attachments already hold a picture a pass could keep.
     *
     * @details
     * - AN OFF-SCREEN SET ANSWERS `!fresh` (see `TargetObjects::fresh`): a set was drawn into by an earlier frame, so a
     *   pass that asks to preserve an attachment gets the picture instead of a clear.
     * - A WINDOW ANSWERS FALSE: the acquired image's content is the presentation engine's, so the first pass into it
     *   clears what it finds. Once a pass of THIS frame has written it, the frame itself says so
     *   (`FrameRecording::window_written`) and a later pass keeps what the earlier one drew.
     *
     * @return true when a later pass may load what is there.
     */
    [[nodiscard]] bool holdsPicture() const noexcept;

    /**
     * @brief Records the pass' entry transitions and opens the rendering instance (see `DynamicRendering::beginRendering`).
     *
     * @param table    The device's entry points (the off-screen half records barriers through them).
     * @param commands The command buffer being recorded (outside any rendering instance).
     * @param load     The pass' load policy and clear values.
     * @param tally    Receives what the entry dependency cost (see `RecordingTally`), or null.
     */
    void begin(const VolkDeviceTable& table, VkCommandBuffer commands, const PassAttachmentOps& load, RecordingTally* tally = nullptr) const;

    /** @brief Closes the rendering instance @ref begin opened (a no-op when it opened none). */
    void end(const VolkDeviceTable& table, VkCommandBuffer commands) const noexcept;

    /**
     * @brief Puts this set's own layout records back to `Undefined`, because the commands that moved them never ran.
     *
     * @details
     * - THE WINDOW HALF DOES NOTHING: the acquired image's record belongs to the window and is not reset by a failed
     *   frame - the image is simply given back (see `WindowTarget::abandonFrame`), and the next acquire states its own
     *   starting layout.
     * - An off-screen set hands the rewind to `TargetObjects::resetLayouts`, which is where the rule lives.
     */
    void rewind() const noexcept;

  private:
    std::shared_ptr<TargetObjects>        images_{}; ///< The off-screen attachment set this handle keeps alive.
    WindowTarget*                        window_{ nullptr };
    VkExtent2D                           extent_{};
    vn::graphics::RenderPassCompatibility compatibility_{};
};

VN_BACKEND_VK_NS_END
