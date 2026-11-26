#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Why an engine could not drive a request at all: the reasons `frame()` and `prepare()` share.
 *
 * @details
 * - A PRECONDITION, not an outcome: no device was ever handed to the engine (#NoDevice), the device it was handed
 *   was never brought up / has been detached (#NotInitialized), or the interval a frame is built in is already held -
 *   by this thread (#Reentrant: a hook or a diagnostic sink driving the same engine from inside a frame) or by
 *   ANOTHER thread (#Concurrent: two host threads driving one engine). Nothing was compiled, submitted or prepared,
 *   so there is no frame result and no report to read - the reason is the whole answer.
 * - TWO GROUPS, ONE SPELLING (`G1` / `G4`): the first two say "this engine cannot drive anything yet"; the last two
 *   say "it can, and the interval is taken right now". A host that must wait or defer needs the first group; a host
 *   with a bug (a frame asked from a hook, two threads driving one engine) needs the second - and both arrive as
 *   values, because the engine never waits for the thread that already holds the interval
 *   (`graphics-threading.md` 2.17).
 * - The spellings are shared on purpose (`G1`): `RenderEngine::frame()` and `RenderEngine::prepare()` used to
 *   answer the same two facts twice, once as a frame outcome and once as a prepare outcome, and a host had to learn
 *   both to answer "is this engine ready".
 *
 * @note
 * - Readiness is not a frame fact: a frame that ran and issued no draw is a #FrameReport question, and a picture that
 *   did not reach its surface is #PresentOutcome's.
 */
enum class EngineNotReadyReason : std::uint8_t
{
    NoDevice,       // setDevice() was never called, so there is nothing to hand a frame to.
    NotInitialized, // initialize() has not succeeded: never called, refused, or shutdown() ran since.
    Reentrant,      // asked from the thread that is already building or answering this engine's frame.
    Concurrent,     // ANOTHER host thread is building or answering one: one host thread drives the frames.
};

VN_GRAPHICS_NS_END
