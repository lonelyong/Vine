#pragma once

#include <cstdint>
#include <vector>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/core/Readback.hpp>
#include <vine/vsg/vsg_global.hpp>

namespace vsg
{
class Device;
}

/**
 * @brief The host's SYNCHRONOUS readback: one attachment of an off-screen target, as the last frame left it.
 *
 * TWO SOURCES, ONE PICTURE. The executor already appends every frame's targets' copy-back nodes AFTER every
 * pass ("a probe reads the frame's final picture") - for a colour target that is attachment 0, and a
 * depth-only target copies its depth. So a readback of what a frame already copied needs NO submission at
 * all: it stops the device (the caller counts that wait, see SessionContentAccess::waitDeviceIdle) and reads
 * the mapped buffer. What a frame did NOT copy - the depth of a colour target, a second colour attachment -
 * is copied HERE, once, by submitting the target's own copy commands on their own command buffer and waiting
 * on a fence: the same commands the frame would have recorded, and a target that has been DRAWN INTO is in a
 * defined layout, so the copy is valid. A target no frame has drawn into is `NotRecorded` (the SDK's
 * NotReady) - a copy of it would read UNDEFINED memory and call it a picture.
 *
 * THE ORDER IS THE CONTRACT, and each step is there for a reason:
 *   1. the classification answers FIRST (and needs no device): an unknown attachment, an unreadable format,
 *      a target nothing has been recorded into - a request that cannot be served costs nothing;
 *   2. the caller stops the device: the frame whose copy filled the buffer may still be in flight;
 *   3. only if the frame did not copy this attachment, it is copied here - after the stop, so the two
 *      submissions cannot overlap;
 *   4. the probe reads the mapped buffer, and its bytes/values are handed on.
 *
 * NOT thread-safe: it is used from the frame's own thread, like the rest of the backend.
 */
VN_VSG_NS_BEGIN

/** @brief Why a synchronous readback did not happen (the caller reports the sentence and maps the answer). */
enum class HostReadbackRefusal : std::uint8_t
{
    None,              ///< The values were read.
    NoTarget,          ///< The caller gave no target at all (a request that cannot be made).
    UnknownTarget,     ///< This backend does not hold the target (never announced, or released).
    NotBuilt,          ///< It is held but has no built attachments yet.
    NotRecorded,       ///< No frame has drawn into it (yet): there is nothing to read back.
    UnknownAttachment, ///< The target has no such attachment.
    UnreadableFormat,  ///< This backend cannot read that attachment's format (see core::colorReadbackOf).
    BorrowedDepth,     ///< The depth is the LENDER's image: the source target is the one to read (SDK's rule).
    NoDevice,          ///< There is no device to copy with or whose work produced the values.
    TransferFailed,    ///< The copy was submitted/fenced and did not produce a readable picture.
};

/** @brief Classifies a request WITHOUT reading anything (and without stopping the device).
 *
 * The caller asks this first, so a request that cannot be served costs nothing - not even the device wait a
 * readback pays. `None` means a read WILL be attempted: either a frame already copied this attachment (the
 * buffer is current), or the target has been drawn into and the copy can be made here.
 *
 * @param target     The target to classify against (its objects must be built).
 * @param kind       Which attachment kind.
 * @param attachment Colour attachment index (ignored for Depth).
 * @return `None` when a readback can be served, the refusal otherwise.
 */
[[nodiscard]] HostReadbackRefusal classifyReadback(OffscreenTarget& target, core::ReadbackKind kind,
                                                   std::uint32_t attachment) noexcept;

/** @brief Reads colour attachment @p attachment of @p target back as tightly packed RGBA8 rows.
 *
 * The device must have been stopped by the caller (see the file note).
 *
 * @param target     The target to read (its objects must be built and something must have been recorded).
 * @param attachment Colour attachment index, in [0, colorAttachmentCount()).
 * @param device     Device a missing copy is submitted on, or null when only a frame's own copy can serve.
 * @param outPixels  Receives `width * height * 4` bytes on success; untouched otherwise.
 * @return `None` when the pixels were read, the refusal otherwise.
 */
[[nodiscard]] HostReadbackRefusal readColorAttachment(OffscreenTarget& target, std::uint32_t attachment,
                                                      ::vsg::Device* device,
                                                      std::vector<std::uint8_t>& outPixels);

/** @brief Reads @p target's depth attachment back as normalised values in [0, 1], row-major.
 *
 * @param target    The target to read (it must own a depth attachment; a BORROWED depth is the caller's to
 *                  refuse - the map belongs to the lender, see the SDK's readDepthBuffer note).
 * @param device    Device a missing copy is submitted on, or null when only a frame's own copy can serve.
 * @param outDepths Receives `width * height` values on success; untouched otherwise.
 * @return `None` when the values were read, the refusal otherwise.
 */
[[nodiscard]] HostReadbackRefusal readDepthAttachment(OffscreenTarget& target, ::vsg::Device* device,
                                                      std::vector<float>& outDepths);

/** @brief Translates a refusal into the SDK's machine-readable result.
 *
 * One table: a caller that gates on "this backend cannot do it" must not be told a different story by a new
 * refusal case - and an unmapped one falls back to `Failed` (report it), never to `Ok`.
 *
 * @param refusal Why the readback did not happen.
 * @return The SDK result the refusal means.
 */
[[nodiscard]] vn::graphics::ReadbackResult readbackResultOf(HostReadbackRefusal refusal) noexcept;

/** @brief Gets the sentence for one refusal (the caller owns the diagnostic stream).
 *
 * @param refusal Why the readback did not happen.
 * @param what    Entry point that refused ("readColorBuffer()" / "readDepthBuffer()").
 * @return The message to report.
 */
[[nodiscard]] vn::String readbackRefusalMessage(HostReadbackRefusal refusal, const char* what);

VN_VSG_NS_END
