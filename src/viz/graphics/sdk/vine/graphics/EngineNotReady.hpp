#pragma once
#include "graphics_global.hpp"

#include <cstdint>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief Why an engine could not drive a request at all: the ONE pair of reasons its verbs share.
 *
 * @details
 * - A PRECONDITION, not an outcome: no device was ever handed to the engine (#NoDevice), or the device it was handed
 *   was never brought up / has been detached (#NotInitialized). Nothing was compiled, submitted or prepared, so there
 *   is no frame result and no report to read - the reason is the whole answer.
 * - The two spellings are shared on purpose (`G1`): `RenderEngine::frame()` and `RenderEngine::prepare()` used to
 *   answer the same two facts twice, once as a frame outcome and once as a prepare outcome, and a host had to learn
 *   both to answer "is this engine ready".
 *
 * @note
 * - Readiness is not a frame fact: a frame that ran and issued no draw is a #FrameReport question, and a picture that
 *   did not reach its surface is #PresentOutcome's.
 */
enum class EngineNotReady : std::uint8_t
{
    NoDevice,       // setDevice() was never called, so there is nothing to hand a frame to.
    NotInitialized, // initialize() has not succeeded: never called, refused, or shutdown() ran since.
};

VN_GRAPHICS_NS_END
