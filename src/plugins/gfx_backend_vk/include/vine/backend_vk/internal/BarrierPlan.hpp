#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

#include <volk.h>

#include <vine/graphics/DrawState.hpp>

#include "ImageLayout.hpp"

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief THE ONE PLACE where a neutral "what is this image being used for" becomes a Vulkan layout, stage and access
 *        mask.
 *
 * @details
 * - WHY A TABLE. This backend's `ImageLayout` states what an image is FOR (a colour attachment, a depth
 *   attachment, something sampled, something presented) and Vulkan wants three separate spellings for the same
 *   fact - the layout, the stages that must wait, and the access mask that must be visible. Writing those three
 *   inline wherever a barrier is needed is how a backend ends up with two answers for one pass; one function per
 *   direction keeps the answer single.
 * - WHY THE INPUT IS THIS MODULE'S AND NOT THE SDK'S. The SDK states only what every family can answer
 *   (`LoadOp` / `StoreOp`, and what a pass declares); a LAYOUT is one family's spelling of "this image is being
 *   written as colour", so it lives here with the table that consumes it (backlog T15, review 8 §4.7/§4.8).
 *   A second API family replaces THIS TABLE and this input, not the seam.
 * - SYNCHRONISATION 2, NOT THE VULKAN-1.0 FORM. The floor is Vulkan 1.3, so a barrier is a
 *   `VkImageMemoryBarrier2` inside a `VkDependencyInfo` and is recorded by `vkCmdPipelineBarrier2`: the stage masks live
 *   ON the barrier (they are the two rows of this table), which is why no call site passes a stage pair of its own any
 *   more. `recordBarrier` is the one place that names the 1.3 command.
 * - THE TABLE IS PURE. No device, no allocation, no state: the table answers the same thing on a machine with no Vulkan
 *   loader, which is what makes it testable before anything can be drawn. `recordBarrier` takes the device table
 *   because a recording call needs one; it keeps no state of its own.
 *
 * @note Not thread-safe only in the sense that every function here is stateless; they may be called from any thread.
 */
struct ImageState {
    VkImageLayout         layout{ VK_IMAGE_LAYOUT_UNDEFINED };         // The layout that usage requires.
    VkPipelineStageFlags2 stages{ VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT }; // The stages a barrier must synchronise.
    VkAccessFlags2        access{ 0 };                                 // The memory a barrier must make visible.
};

/**
 * @brief The stage a PASS' clock boundary is written at: `ALL_COMMANDS`, because a timestamp answers "when did
 *        everything before it finish".
 *
 * @details
 * - WHAT IT REPLACES. The boundary used to be written at `TOP_OF_PIPE`, a stage that imposes no wait - so a pass' span
 *   measured when the command processor ARRIVED at the boundary, not when the pass' work was done. On a device with a
 *   command front-end that runs ahead of execution that number is queue latency; on a device that executes commands in
 *   order (a software rasteriser) it happens to look right, which is why the meaning is pinned HERE, as a value, and
 *   not only by a timing case: `VkPassTimingTest.AHeavyPassReportsMoreTimeThanACheapOne` states the property, this
 *   constant states the stage, and only the second is observable on every device.
 * - IT IS THE SAME RULE THE BARRIERS FOLLOW: a synchronisation point says what it WAITS for, and "everything" is what
 *   the end of a pass means (see `stateOf`).
 */
inline constexpr VkPipelineStageFlags2 kPassClockBoundaryStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

/** @brief The stage a frame's clock STARTS at: no wait, because nothing before it is part of the frame. */
inline constexpr VkPipelineStageFlags2 kFrameClockStartStage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;

/**
 * @brief What one recording pass asked the queue to wait for: dependencies (the driver calls) and the barriers inside
 *        them.
 *
 * @details
 * - THE WITNESS FOR "A PASS' ENTRY IS ONE DEPENDENCY" (see `BarrierBatch` and `beginRendering`). Vulkan states the
 *   ordering of a pass' attachments as ONE `VkDependencyInfo` holding N image barriers; a recorder that issues one
 *   `vkCmdPipelineBarrier2` per image says the same thing N times, and the number of calls is what a host pays for.
 *   Counting both halves is also what makes a regression visible: an implementation that stops batching grows
 *   `dependencies` without changing `barriers`.
 * - CUMULATIVE, because it is a driver's lifetime counter (see `Device::recordingNumbers`): a test takes the delta
 *   around a frame, exactly as it does for the ring and the set cache.
 */
struct RecordingTally {
    std::uint64_t dependencies{ 0 }; ///< `vkCmdPipelineBarrier2` calls recorded.
    std::uint64_t barriers{ 0 };     ///< Image barriers those calls carried.
};

/**
 * @brief Gets the Vulkan state that serves @p usage.
 *
 * @details
 * - `Undefined` is the "contents do not matter" state (a fresh image, or one whose contents a pass will overwrite), which
 *   is why it carries no access mask and only the top-of-pipe stage.
 * - `Present` is the only value whose layout belongs to the platform rather than to a queue operation, so it is the
 *   only one that names an extension-provided layout.
 * - AN ATTACHMENT ROW STATES BOTH HALVES OF ITS ACCESS, read and write: under dynamic rendering there is no subpass
 *   dependency to order a pass' LOAD (or its blend read) after the previous write, so a barrier that named only the
 *   write would leave the read unordered (see `DynamicRendering`).
 *
 * @param usage What the image is used for (see `ImageLayout`).
 * @return The layout, stages and access mask for @p usage.
 */
[[nodiscard]] ImageState stateOf(ImageLayout usage) noexcept;

/**
 * @brief Answers whether moving from @p from to @p to also TRANSITIONS the layout, i.e. whether the two usages differ.
 *
 * @details
 * - WHAT IT ANSWERS IS THE LAYOUT HALF, NEVER THE DEPENDENCY HALF. Two different usages need a barrier that changes the
 *   layout; two IDENTICAL usages may still need one, because something has to be ordered against an earlier access - and
 *   under dynamic rendering that is the common case: `DynamicRendering::beginRendering` records a barrier between two
 *   identical states on purpose, because it IS the execution dependency between two passes into one image. A caller that
 *   skips a barrier because this answered false has therefore ordered nothing.
 * - Same usage means the same layout, stage and access, which is what makes the LAYOUT half skippable at all - it is not
 *   a statement that the barrier has no consequences.
 *
 * @param from Usage the image is in now.
 * @param to   Usage the image is about to be in.
 * @return true when the two usages differ (the barrier also transitions the layout); false when they are identical.
 */
[[nodiscard]] bool requiresTransition(ImageLayout from, ImageLayout to) noexcept;

/**
 * @brief Builds the barrier that moves @p image from one usage to another.
 *
 * @details
 * - `oldLayout` / `newLayout` come from @p from and @p to, and the access masks from the same table, so a caller cannot
 *   pair a layout with the wrong access mask.
 * - Queue-family ownership is ignored (the backend records everything on one queue family).
 *
 * @param image  Image to transition (may be `VK_NULL_HANDLE`: the value is still well formed).
 * @param from   Usage the image is in now.
 * @param to     Usage the image is about to be in.
 * @param aspect Aspect mask (colour or depth).
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierFor(VkImage           image,
                                              ImageLayout from,
                                              ImageLayout to,
                                               VkImageAspectFlags aspect) noexcept;

/**
 * @brief Gets the state a copy WRITES to: `TRANSFER_DST_OPTIMAL` with the transfer stage and write access.
 *
 * @details
 * - THE TWIN OF `transferReadState`, and the reason both live HERE: a device verb's two ends are the same table's
 *   statement as a pass' states are, so the stage and access a copy writes with cannot drift into a second file. Before
 *   this, the write end was a private helper inside `ContentImages.cpp` - one table in name, two in fact (`G1`).
 *
 * @return The transfer-destination state.
 */
[[nodiscard]] ImageState transferWriteState() noexcept;

/**
 * @brief Gets the state a copy READS from: `TRANSFER_SRC_OPTIMAL` with the transfer stage and read access.
 *
 * @details
 * - `ImageLayout` states what a PASS uses an image for; a readback is a device verb, not a pass, so this end of
 *   a barrier has no SDK spelling. It is kept here (next to the table) rather than in the SDK so the seam does not grow a
 *   Vulkan-shaped member for one backend's readback (C6).
 *
 * @return The transfer-source state.
 */
[[nodiscard]] ImageState transferReadState() noexcept;

/**
 * @brief Builds a barrier between two states, each of which may come from either table.
 *
 * @details
 * - THE TWO QUEUE-FAMILY INDICES ARE `VK_QUEUE_FAMILY_IGNORED`, which is a statement about this backend's model rather
 *   than an omission: every command it records goes on ONE family (the graphics queue that also presents), so there is
 *   no ownership to transfer - a swapchain image included, because the family that draws it is the family that presents
 *   it. An explicit release/acquire pair would describe a transfer that never happens; a second family would be the
 *   feature that makes this line change.
 * - The same `oldLayout` / `newLayout` + stage/access pair is used for every caller, so a caller cannot pair a layout
 *   with another row's stages or access mask (see `stateOf`).
 *
 * @param image  Image to transition.
 * @param from   State the image is in.
 * @param to     State the image is about to be in.
 * @param aspect Aspect mask (colour or depth).
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierBetween(VkImage            image,
                                                  const ImageState&  from,
                                                  const ImageState&  to,
                                                  VkImageAspectFlags aspect) noexcept;

/**
 * @brief Builds the barrier that moves a RANGE of @p image's subresources from one usage to another.
 *
 * @details
 * - A texture image is not one subresource: a mip chain and an array/cube face set are several, and a barrier that
 *   names only layer 0 leaves the rest in the layout they had - the copies that follow then write UNDEFINED
 *   subresources (measured in the app: a cube map's faces 1..5 at submit time). The aspect-only overload above is the
 *   single-subresource case, which is what every attachment barrier wants.
 *
 * @param image  Image to transition (may be `VK_NULL_HANDLE`: the value is still well formed).
 * @param from   Usage the image is in now.
 * @param to     Usage the image is about to be in.
 * @param range  The subresources the transition covers.
 * @return The barrier, ready to be recorded (see recordBarrier).
 */
[[nodiscard]] VkImageMemoryBarrier2 barrierBetween(VkImage                        image,
                                                  const ImageState&              from,
                                                  const ImageState&              to,
                                                  const VkImageSubresourceRange& range) noexcept;

/**
 * @brief Records ONE dependency carrying @p barriers, counting it in @p tally when one is given.
 *
 * @details
 * - THE BATCH IS THE POINT, NOT AN OPTIMISATION OF THE SAME STATEMENT. Vulkan describes the ordering a pass' entry
 *   needs as one `VkDependencyInfo` with N image barriers inside it; recording N dependencies that each carry one
 *   barrier is a different, larger command stream that says the same thing. Every engine that states a pass' entry
 *   transitions in one call does so for that reason, and the count is observable (`RecordingTally`).
 * - AN EMPTY BATCH RECORDS NOTHING: a pass with no image to move owes the queue no dependency.
 * - ONE PLACE names `vkCmdPipelineBarrier2`, still (see @ref recordBarrier).
 *
 * @param table    The device's entry points.
 * @param commands The command buffer being recorded.
 * @param barriers The barriers this dependency carries (borrowed for the call).
 * @param tally    Receives the dependency and its barrier count, or null when the caller keeps no number.
 */
void recordBarriers(const VolkDeviceTable&                 table,
                    VkCommandBuffer                        commands,
                    std::span<const VkImageMemoryBarrier2> barriers,
                    RecordingTally*                        tally = nullptr) noexcept;

/**
 * @brief Records one image barrier through synchronisation 2.
 *
 * @details
 * - THE ONE PLACE the 1.3 command is named: every barrier this backend records goes through here (or through
 *   @ref recordBarriers, which it uses), so there is no second spelling of "submit a dependency" to drift. Nothing else
 *   is set - the barrier already carries both stage masks and both access masks (see @ref ImageState).
 * - A barrier that names no image is still legal to record (see @ref barrierBetween); the caller decides whether it is
 *   worth recording at all - and a barrier between two IDENTICAL states is worth recording whenever something has to be
 *   ordered (see @ref requiresTransition).
 *
 * @param table    The device table to record through.
 * @param commands The command buffer being recorded.
 * @param barrier  The barrier to record.
 * @param tally    Receives the dependency and its barrier count, or null when the caller keeps no number.
 */
void recordBarrier(const VolkDeviceTable& table,
                   VkCommandBuffer        commands,
                   const VkImageMemoryBarrier2& barrier,
                   RecordingTally*        tally = nullptr) noexcept;

/**
 * @brief Gets the attachment load operation for @p op.
 *
 * @param op The pass' neutral load operation (see `PassLoad`).
 * @return The Vulkan attachment load operation.
 */
[[nodiscard]] VkAttachmentLoadOp loadOpOf(vn::graphics::LoadOp op) noexcept;

/**
 * @brief Gets the attachment store operation for @p op.
 *
 * @param op The pass' neutral store operation (see `PassLoad`).
 * @return The Vulkan attachment store operation.
 */
[[nodiscard]] VkAttachmentStoreOp storeOpOf(vn::graphics::StoreOp op) noexcept;

/**
 * @brief MOVES @p image to @p to, recording the barrier AND writing the record that says where it now is.
 *
 * @details
 * - THE ONE SHAPE EVERY MOVER USES (`G1` / `G4` / `G7`). "The command that moves an image writes its record" is the
 *   invariant this backend's layouts rest on, and it was previously assembled by hand at every site - a barrier built
 *   from one value and an assignment of another. Two measured VUIDs (09588 for a depth, 09592 + 09600 for a colour)
 *   were exactly that pair drifting apart, so the pair is now one call: a caller cannot move an image and forget (or
 *   mis-state) its record.
 * - NOTHING HAPPENS WHEN NOTHING MOVES: @p record already stating @p to answers false and records no barrier. A
 *   caller that needs the DEPENDENCY without a layout change (two passes into one attachment) is not this verb - that
 *   barrier belongs to `DynamicRendering::beginRendering`, which records it deliberately.
 * - The barrier itself is `barrierBetween`, so the barriers of `to` and `from` are the table's two rows and cannot be
 *   paired with another usage's stages or accesses.
 *
 * @param table    The device's entry points (volk table).
 * @param commands The command buffer being recorded.
 * @param image    The image to move (may be `VK_NULL_HANDLE`: nothing is recorded then).
 * @param record   The layout record that describes @p image; it is read AND written.
 * @param to       The usage the image is about to be in.
 * @param aspect   The aspect mask (colour, or depth for a depth image).
 * @return true when a barrier was recorded (i.e. @p record differed from @p to).
 */
bool transitionImage(const VolkDeviceTable& table,
                     VkCommandBuffer        commands,
                     VkImage                image,
                     ImageLayout&           record,
                     ImageLayout            to,
                     VkImageAspectFlags     aspect,
                     RecordingTally*        tally = nullptr) noexcept;

/**
 * @brief ONE PASS' MOVES, appended and then recorded as ONE dependency: each move writes the record it moved.
 *
 * @details
 * - WHY A BATCH AND NOT N MOVERS. A pass that samples four pictures would otherwise record four dependencies, and the
 *   pass' entry (see `DynamicRendering::beginRendering`) would add five more - one statement, nine commands. The batch
 *   keeps the invariant that made `transitionImage` worth having (THE COMMAND THAT MOVES AN IMAGE WRITES ITS RECORD)
 *   while saying the ordering once.
 * - THE STORAGE IS THE CALLER'S. A fixed array on the recording path (the bound is the roles a pass states), so this
 *   type allocates nothing and cannot fail for want of memory; it answers `false` from @ref move when a caller sized
 *   it wrong, which is a defect in the caller rather than a device answer.
 * - AN ALREADY-CORRECT RECORD MOVES NOTHING: `move` answers false and appends no barrier, and the caller should not
 *   log a rewind for it.
 * - @ref flush is explicit (no destructor): recording a command from a destructor hides the moment a dependency reaches
 *   the command buffer, and the one caller here has exactly one place to state it.
 *
 * @warning Not thread-safe: one frame at a time (the SDK's per-driver contract).
 */
class BarrierBatch final {
  public:
    /**
     * @brief Opens a batch over @p storage, recording into @p commands when it is flushed.
     *
     * @param table    The device's entry points.
     * @param commands The command buffer being recorded.
     * @param storage  The caller's fixed array (one entry per move the caller may state).
     * @param tally    Receives what {@ref flush} recorded, or null.
     */
    BarrierBatch(const VolkDeviceTable& table, VkCommandBuffer commands, std::span<VkImageMemoryBarrier2> storage, RecordingTally* tally = nullptr) noexcept;

    BarrierBatch(const BarrierBatch&)            = delete;
    BarrierBatch& operator=(const BarrierBatch&) = delete;

  public:
    /**
     * @brief Moves @p image to @p to: appends the barrier and writes @p record.
     *
     * @param image  The image to move (null records nothing).
     * @param record The layout record that describes @p image; it is read AND written.
     * @param to     The usage the image is about to be in.
     * @param aspect The aspect mask (colour, or depth for a depth image).
     * @return true when a barrier was appended (i.e. @p record differed from @p to and there was room).
     */
    bool move(VkImage image, ImageLayout& record, ImageLayout to, VkImageAspectFlags aspect) noexcept;

    /** @brief Records everything appended so far as ONE dependency, then empties the batch (idempotent). */
    void flush() noexcept;

    /** @brief Gets how many barriers are waiting for the next @ref flush. */
    [[nodiscard]] std::size_t size() const noexcept
    {
        return used_;
    }

  private:
    const VolkDeviceTable*         table_{ nullptr };
    VkCommandBuffer                commands_{ VK_NULL_HANDLE };
    std::span<VkImageMemoryBarrier2> storage_{};
    RecordingTally*                tally_{ nullptr };
    std::size_t                    used_{ 0U };
};

VN_BACKEND_VK_NS_END
