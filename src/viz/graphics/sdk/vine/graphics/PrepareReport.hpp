#pragma once
#include "graphics_global.hpp"

#include <cstdint>

#include "EngineNotReadyReason.hpp"

VN_GRAPHICS_NS_BEGIN

/**
 * @brief What one `RenderEngine::prepare()` did: whether a backend prebuilt the frame, and what that cost.
 *
 * @details
 * - WHY IT IS A VALUE AND NOT A BOOL. Preparing a frame is a QUERY with a side effect (the identities are built so
 *   the next frame does not have to), so "did it happen" answers one question and leaves three worth knowing
 *   unanswered: how much of the frame was asked for, what it actually had to build, and what it could not
 *   build at all (a pass whose target has no objects yet, a program the tables cannot answer). A bool would make the
 *   second prepare indistinguishable from "nothing was needed" and hide the first frame's compile cost entirely.
 * - THE COUNTS ARE DELTAS OF THE CALL, not running totals: a second `prepare()` of an unchanged frame answers zeros,
 *   which is exactly the property the call exists for - after it, a recorded frame builds nothing (`R23`). The
 *   backend's own reports (`FrameReport`) keep the totals; this value says what THIS call did.
 * - TWO KINDS OF NUMBER, TWO QUESTIONS. `passes` / `draw_packets` / `draws` are the WORKLOAD OF THE FRAME that was
 *   asked for - the plan's own passes that have an EFFECT (a draw packet, or an announced clear), the packets they
 *   issue, and the draws those packets submit - so they are comparable with what a recorded frame reports and do not
 *   move because a backend could not serve one of them.
 *   `halves` / `sets` / `uploads` / `pipelines` are what had to be BUILT to serve it, and are the evidence of what the
 *   backend actually did.
 * - `draws` IS THE NUMBER A PROFILER CALLS A DRAW CALL, AND `draw_packets` IS NOT: one packet describes one view's
 *   whole list, so a pass that draws fifty drawables through ONE view issues ONE packet and FIFTY draws and builds
 *   one pipeline, while fifty drawables in fifty views ask fifty packets. The draws inside a packet are `DrawItem`s
 *   (see `DrawPacket`'s own note), and none of these three numbers is derivable from the others.
 * - THE COUNTS ARE COMPUTED ONCE, BY THE SDK (`vn::graphics::frameCountsOf`, see FrameCounts): a backend fills these
 *   three fields from that value instead of walking the frame itself. Two backends that count their own way report
 *   different numbers for the same frame, and a clear-only pass or a packet with no camera is enough to make them
 *   disagree.
 * - THE COUNTS DESCRIBE WHAT WAS BUILT, not what a frame did: a pipeline layer, a descriptor set, a stream bind, a
 *   texture image. That is what lets `prepare()` report them without the frame having been recorded or submitted;
 *   a texture image is the one kind whose bytes only land once a transfer batch has run, and it is counted here
 *   because it was THIS call that made it resident (see ADR-2026-11-14).
 *
 * @note
 * - A backend that keeps nothing compiled (an offline renderer, a test double) answers #Outcome::Unsupported rather
 *   than a zero-filled `Prepared`: "there is nothing to prebuild" and "I prebuilt nothing" are different facts, and a
 *   host reading the second one would believe the next frame is free.
 */
struct VN_GRAPHICS_API PrepareReport {
    /** @brief What became of one prepare request. */
    enum class Outcome : std::uint8_t
    {
        Prepared,    // A backend walked the frame and built what it needed, without submitting the FRAME (its own
                     // transfer batch is not a frame; see RenderFrameExecutor::prepareFrame).
        NotReady,    // Nothing was prepared: the engine cannot drive a frame (see reason).
        Refused,     // The backend would not prepare it (a frame is open, or its target is not one it serves).
        Unsupported, // The backend keeps nothing compiled, so there is nothing to prepare.
    };

    Outcome       outcome{ Outcome::NotReady }; // What became of the request (see Outcome).
    EngineNotReadyReason reason{ EngineNotReadyReason::NoDevice }; // Why, when @ref outcome is #Outcome::NotReady.
    std::uint64_t        passes{ 0 };                              // Passes with an EFFECT: a draw packet or an announced clear (see FrameCounts).
    std::uint64_t        draw_packets{ 0 };                        // Draw packets those passes issue - one per view (see DrawPacket).
    std::uint64_t        draws{ 0 };                               // Draw items those packets submit (what a profiler calls draw calls).
    std::uint64_t halves{ 0 };                  // Compiled halves THIS call built (0 = already there).
    std::uint64_t sets{ 0 };                    // Declared sets THIS call built.
    std::uint64_t uploads{ 0 };                 // Uploads THIS call built - streams and textures (the kinds FrameUploads reports).
    std::uint64_t pipelines{ 0 };               // Pipeline identities THIS call compiled.

    /** @brief Gets whether a backend prepared the frame.
     *
     * @return true when @ref outcome is #Outcome::Prepared.
     */
    [[nodiscard]] bool prepared() const noexcept
    {
        return outcome == Outcome::Prepared;
    }

    /** @brief Gets the same answer as @ref prepared, for a caller that tests the report directly.
     *
     * @return true when a backend prepared the frame.
     */
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return prepared();
    }
};

VN_GRAPHICS_NS_END
