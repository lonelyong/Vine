#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/graphics/FrameTimeline.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief The completion evidence of a DEVICE: one submission sequence, and the watermark every submission's proof folds into.
 *
 * @details
 * - WHY IT EXISTS. A resource is shared: a buffer, an image or a pipeline a host released may still be named by work
 *   submitted from ANY place on the device, so "may this be destroyed" is a DEVICE question. The witness is the QUEUE -
 *   every submission is numbered, and destroying an object asks only "is every submission up to the one that could
 *   name it complete?"
 * - THE RULE. A backend takes a number when it hands a command buffer to the queue (`noteSubmission`, inside the
 *   queue's own external-synchronization lock, so the numbers follow the order the queue sees) and reports that
 *   number's proof when the fence says its work is done (`noteCompletion`). The watermark is the highest number such
 *   that EVERY submission up to it is complete: the oldest unproven submission minus one, or every number handed out
 *   when nothing is unproven. That is exact regardless of the order completions arrive in, and it never moves back.
 * - WHAT IT IS NOT (and what the previous shape got wrong). It is NOT a minimum over the drivers' own evidence. A
 *   driver that stops framing keeps its last frame forever, so that fold stalls every release on the device as soon as
 *   one engine idles - a defect that no amount of care in the driver could fix, because a driver is not a witness of
 *   work it never submitted. Completion belongs to the submission (`G1`); the driver registry below exists so a backend
 *   can key its own tables by `FrameDriverId`, and for nothing else.
 * - WHOSE NUMBER IS MINTED WHEN. A frame's number (`FrameToken`) is minted when its body STARTS; a submission's number
 *   is minted when its command buffer is HANDED TO THE QUEUE. Two drivers may open frames before either submits, which
 *   is why the two sequences are separate and why a park is dated against the second one.
 * - This is the device-level equivalent of what D3D12's `GetCompletedValue`, WebGPU's submission index, Metal's
 *   completed handlers and Filament's `Driver::destroyDeferred` do: ONE submission watermark per device, not one per
 *   surface. See `.ai/design/graphics-sessions.md` 2.6 and `ADR-2026-11-24-completion-belongs-to-the-submission.md`.
 *
 * NOT thread-safe: the device guards it with its clock lock (see `Impl::clock_mutex` in the backend).
 */
VN_GRAPHICS_NS_BEGIN

/**
 * @brief The identity of ONE frame driver on a device: what `DeviceClock` mints and what every id-carrying seam type
 *        takes.
 *
 * @details
 * - MINTED BY THE CLOCK, never by the caller: `DeviceClock::addDriver()` answers one, and it stays valid until that
 *   clock removes it.
 * - NEVER REUSED within one clock: a removed driver's id is not handed out again, so an id that outlived its driver
 *   answers "unknown" rather than naming another driver's tables.
 * - ONE SPELLING: `DeviceClock`'s verbs take this type, and so does every seam a backend keys by driver - a backend's
 *   own tables (which driver may build a key, which driver holds a target for a frame) answer with THIS id rather than
 *   minting their own. It used to be spelled `WindowId` / `SessionId`, in three widths (`D-01`).
 * - WHY u32: a device hands out a handful of drivers in its lifetime, so the width is a namespace, not a count.
 *
 * @note An id means something WITHIN the clock that minted it: two devices both start at 0, so it is not a
 *       process-wide key.
 */
using FrameDriverId = std::uint32_t;

/**
 * @brief One device's submission clock: the frame numbers, the driver ids, and the completion watermark (see the file
 *        note).
 */
class VN_GRAPHICS_API DeviceClock {
  public:
    /** @brief Registers a frame driver and answers its id.
     *
     * @details
     * - Registration is IDENTITY, not evidence: it keys the tables a backend keeps for itself (which driver may build a
     *   key, which driver holds a target). The completion watermark does not consult it, which is what keeps an idle
     *   driver from holding the device back.
     *
     * @return The driver's id, for every call below that names a driver.
     */
    [[nodiscard]] FrameDriverId addDriver() noexcept;

    /** @brief Unregisters a driver: its id stops naming a live driver (the number is never handed out again).
     *
     * @param driver Id returned by @ref addDriver (an unknown or already removed id changes nothing).
     */
    void removeDriver(FrameDriverId driver) noexcept;

    /** @brief Gets how many drivers are registered. */
    [[nodiscard]] std::size_t drivers() const noexcept;

    /** @brief Opens a frame on the device's clock (see FrameTimeline::begin). */
    [[nodiscard]] FrameToken begin() noexcept;

    /** @brief Spends the frame @p token names as submitted (see FrameTimeline::submitted).
     *
     * @param token Token the device's clock returned from @ref begin.
     */
    void submitted(FrameToken token) noexcept;

    /** @brief Marks the open frame as over without a submission (see FrameTimeline::abandoned).
     *
     * @param token Token the device's clock returned from @ref begin.
     */
    void abandoned(FrameToken token) noexcept;

    /** @brief Gets the device's frame clock (what a frame body mints its token from). */
    [[nodiscard]] FrameTimeline& timeline() noexcept;

    /** @brief Gets the device's frame clock (const). */
    [[nodiscard]] const FrameTimeline& timeline() const noexcept;

    /** @brief Numbers a submission: something was just handed to the queue, and this is its sequence number.
     *
     * @details
     * - CALL IT INSIDE THE QUEUE'S EXTERNAL-SYNCHRONIZATION LOCK, immediately before the submit: the numbers must
     *   follow the order the queue receives the submissions, or "every submission up to N" would not be the set the
     *   queue ran.
     * - EVERY NUMBER IT HANDS OUT IS EVENTUALLY PROVEN (`noteCompletion`) or abandoned with the device: a submission
     *   that the queue refused is proven at once (nothing it named will ever execute), and one whose fence never
     *   signalled can only be a lost device, whose teardown releases everything at once.
     *
     * @return The submission's sequence number (1-based).
     */
    [[nodiscard]] std::uint64_t noteSubmission() noexcept;

    /** @brief Reports that submission @p submission is complete: the queue proved its work is done.
     *
     * @details
     * - Monotonic in effect and exact: the watermark is the oldest unproven submission minus one, so a later
     *   submission completing while an earlier one is still in flight does NOT claim the earlier one.
     *
     * @param submission Number @ref noteSubmission answered (0, or one already proven, changes nothing).
     */
    void noteCompletion(std::uint64_t submission) noexcept;

    /** @brief Gets how many submissions have been handed to the queue (the newest sequence number). */
    [[nodiscard]] std::uint64_t submissions() const noexcept;

    /** @brief Gets the submission every earlier one of which is proven complete (see the file note). */
    [[nodiscard]] std::uint64_t completedSubmission() const noexcept;

  private:
    /// Recomputes the watermark from the unproven submissions, and moves it forward only.
    void updateWatermark() noexcept;

    FrameTimeline              timeline_;
    std::uint32_t              next_driver_{ 0 }; // The next id addDriver hands out (never reused).
    std::vector<bool>          drivers_{};        // live flag per id.
    std::uint64_t              submissions_{ 0 }; // Submissions handed to the queue.
    std::uint64_t              completed_{ 0 };   // The watermark (see completedSubmission).
    std::vector<std::uint64_t> unproven_{};       // Submitted, not yet proven; ascending (the oldest is front).
};

VN_GRAPHICS_NS_END
