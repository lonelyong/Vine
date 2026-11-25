#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief The readback vocabulary: one answer, the two byte-width tables, and the depth decoder.
 *
 * @details
 * - WHAT A READBACK IS, IN THREE PARTS. One ANSWER (`ReadbackResult`: the classification with the picture it
 *   classified), one TABLE PER SIDE (`colorBytesPerTexel` / `depthBytesPerTexel`: how many bytes one texel occupies,
 *   and whether this side can be read at all), and one DECODER (`decodeDepth`) that turns copied depth bytes into the
 *   normalised values a probe answers with.
 * - WHY THE ANSWER IS ONE VALUE. A verb returns the classification TOGETHER with the picture, so a refused read
 *   carries no picture and "the vector you already owned" is not a second, stale answer: the old
 *   `(..., std::vector&, ReadbackStatus*)` shape let a caller read last frame's pixels after a refusal, and that is
 *   now unrepresentable rather than documented away.
 * - WHY THE TABLES LIVE HERE (`G6`, `G10`). The same number sizes the device's copy AND indexes the answer a host
 *   receives, so a mask (`R8`), a packed pair (`RG8`) and a colour (`RGBA8`) cannot be counted one way where the copy
 *   is recorded and another way where the bytes are read. A texel's width IS its channel count for the 8-bit family,
 *   and a format with no packing answers `std::nullopt` - one fact, one spelling, with no `{bytes, readable}` pair
 *   that can disagree with itself.
 * - WHY NO CLASSIFICATION TABLE. Whether a request is servable is answered by the DEVICE, from the record of what it
 *   actually built (which image, which format, whether a frame drew it). A second table over guessed facts was one
 *   answer too many: it described a copy-back-node model neither backend has, and its categories never crossed to a
 *   host. The one fact it carried that must not be lost is the PRECEDENCE between answers, which lives on
 *   `ReadbackStatus` now - where the host reads it.
 * - WHAT IS DELIBERATELY NOT HERE: a "nothing has been laid out yet" category. A target of no size
 *   cannot exist - `create` refuses a zero extent, and before a target exists at all the LIFECYCLE plan
 *   answers `Repair(SizeUnknown)` for it (`planTarget`, which is where the surface's size authority and
 *   the hidden / not-yet-laid-out cases live). A refusal arm nothing can reach reads as a covered case
 *   while covering none.
 * - WHAT THIS FILE DOES NOT DO: it does not touch the pixels. Deciding that a readback is servable and
 *   handing out the bytes are different halves (the bytes belong to the device layer); the only
 *   pixel-level thing here is the FORMAT table, because "how many bytes is one texel" and "may this
 *   format be read at all" are the same fact that sizes the copy and validates the decode.
 */
VN_GRAPHICS_NS_BEGIN

/** @brief What a host makes of a readback: the ONE classification every `ReadbackResult` carries.
 *
 * @details
 * - FIVE CASES, EACH A DIFFERENT THING TO DO. `Unsupported` gates a feature off (this device never will),
 *   `NotReady` asks again after a frame (a frame can still supply what is missing), `Invalid` is a caller bug (the
 *   request names something the target does not have), `Failed` is reported (the copy was attempted and did not
 *   complete).
 * - WHICH ANSWER WINS, one rule for both verbs: a request that can NEVER be served answers permanently first
 *   (`Invalid` - the request names nothing; `Unsupported` - this device has no packing for it), and only then the
 *   facts a later frame can change (`NotReady`). The exception runs the other way for one fact, and it is the honest
 *   one: a target this device has not built yet answers `NotReady` before the request is judged, because until a
 *   frame announces it there is no shape to judge against. So an unservable request answers the same way whatever
 *   frames run, and a caller is never told to retry something retrying cannot fix.
 * - WHY `Status` AND NOT `Result` (the spelling this carried while a readback answered with nothing but this code):
 *   the verbs return `ReadbackResult<T>` - the classification WITH the picture it classified - so the code is one
 *   member of that answer and is named for what it is (`G4`, `G6`; `FrameCallOutcome::Status` is the same rule, and
 *   `SurfaceTargetResult`, which carries no product of its own, keeps `Result`).
 * - It is a FAMILY-LEVEL name, not a member of the template: `ColorReadback` and `DepthReadback` are two
 *   instantiations of one answer, and a status nested in the template would have made them two incompatible types
 *   for one vocabulary.
 * - ONE CLASSIFICATION, NOT TWO (`G1`): there is no finer enum beside this one. A host switches on these five cases,
 *   and the device that answers with them is the only place the finer facts (which image, which format, drew or not)
 *   are consulted - they are facts about its own record, not a shared vocabulary.
 */
enum class ReadbackStatus : std::uint8_t
{
    Ok,          // The pixels / depths were read (see ReadbackResult::values).
    Unsupported, // This device cannot read it back: the format has no packing here, or it cannot copy to the host.
    NotReady,    // Nothing to read yet: the target is not built, or no frame has drawn it. Ask again after a frame.
    Invalid,     // The request names something the target does not have (wrong verb, index, no depth, a borrowed depth).
    Failed,      // The copy was attempted and did not complete (no usable device, a submission or mapping error).
};

/** @brief One readback answer: the picture, and the classification that says whether it is one.
 *
 * @details
 * - ONE QUESTION, ONE ANSWER (`G3`, `G4`). The pixels / depths are this value's PRODUCT, not an out-parameter beside
 *   a bool, so a refusal carries no picture at all and a caller cannot read stale bytes a refused call never wrote.
 * - `values` is populated exactly when `status` is #ReadbackStatus::Ok, one element per texel of the source image in
 *   the verb's own packing (see `RenderDevice::readColorImage` for the byte family and `RenderDevice::readDepthImage`
 *   for the normalised floats), row-major, first row at the top.
 * - `ColorReadback` and `DepthReadback` are the two products the device verbs answer with. `ReadbackResult` is NOT a
 *   general `expected` (`G8`): it has no error alternative, no monadic operations and no other consumer, and the
 *   element type is the only thing the two products do not share.
 * - The boolean conversion is explicit (`G4`: a whole picture is not a flag), so `if (read)` and `read.ok()` read
 *   the same answer and nothing silently turns 8 MB of pixels into `true` in an overload resolution.
 */
template <class T>
struct ReadbackResult {
    /**
     * @brief Answers @p status with @p values - the only way to make one, so every answer states its classification.
     *
     * @details
     * - A DEFAULT-CONSTRUCTED answer would have to claim something it did not do (a copy was attempted? this device
     *   cannot?), so there is none: every construction site names the classification it measured. The member order is
     *   the reading order - what happened, then what came of it.
     *
     * @param status What the read answered (see ReadbackStatus).
     * @param values The picture, or an empty vector on every answer but #ReadbackStatus::Ok.
     */
    ReadbackResult(ReadbackStatus status, std::vector<T> values) noexcept
      : status(status)
      , values(std::move(values))
    {}

    /// The classification: #ReadbackStatus::Ok exactly when @ref values holds a picture.
    ReadbackStatus status;
    /// The picture (see the struct's own note); empty on every answer but #ReadbackStatus::Ok.
    std::vector<T> values;

    /**
     * @brief Gets whether the read produced a picture.
     *
     * @return true when @ref status is #ReadbackStatus::Ok.
     */
    [[nodiscard]] bool ok() const noexcept
    {
        return status == ReadbackStatus::Ok;
    }

    /**
     * @brief Answers the readback directly, for a caller that tests the value.
     *
     * @return true when @ref status is #ReadbackStatus::Ok.
     */
    explicit operator bool() const noexcept
    {
        return ok();
    }
};

/// The answer of a colour readback: the attachment's stored texels, one byte per channel value.
using ColorReadback = ReadbackResult<std::uint8_t>;

/// The answer of a depth readback: one normalised depth per texel.
using DepthReadback = ReadbackResult<float>;

/** @brief Gets how many bytes one texel of @p format occupies in a colour readback, or nothing when it cannot be read.
 *
 * @details
 * - THE COLOUR READBACK SERVES THE 8-BIT FAMILY, AND A TEXEL'S WIDTH IS ITS CHANNEL COUNT: one byte per channel, so
 *   `R8` is 1, `RG8` is 2 and `RGBA8` is 4 bytes per texel - the bytes the attachment holds, in channel order. One
 *   rule covers a colour and a mask, because both are a byte per channel.
 * - THE FLOAT FAMILY HAS NO PACKING (`std::nullopt`): this table promises "copy the texels, no conversion", and a byte
 *   count alone does not describe a half or a full float to a caller decoding bytes. A float product is read back when
 *   a decoder for it lands - not by handing its bits back under an 8-bit contract.
 *
 * @param format The attachment's colour format.
 * @return The texel width in bytes, or `std::nullopt` when this format has no readback packing here.
 */
[[nodiscard]] VN_GRAPHICS_API std::optional<std::uint32_t> colorBytesPerTexel(vn::graphics::RenderTarget::ColorFormat format) noexcept;

/** @brief Gets how many bytes one texel of @p format occupies in a depth readback, or nothing when it cannot be read.
 *
 * @details
 * - `std::nullopt` is the honest answer for a combined depth/stencil image: it has no plain depth copy layout, and
 *   reading its first bytes as floats produces numbers that look like depths and are not.
 *
 * @param format The attachment's depth format.
 * @return The texel width in bytes (2 for D16, 4 for D32 / D32F), or `std::nullopt` when it has no plain depth copy.
 */
[[nodiscard]] VN_GRAPHICS_API std::optional<std::uint32_t> depthBytesPerTexel(vn::graphics::RenderTarget::DepthFormat format) noexcept;

/** @brief Turns copied depth bytes into the normalised values a probe answers with.
 *
 * @details
 * - D32 / D32F arrive as their raw float, D16 as an unsigned integer divided by its full scale (65535) -
 *   the conversion the format defines, not an approximation of it. An unreadable format or a buffer that
 *   does not hold a whole number of texels yields an EMPTY result rather than a partial one: a probe
 *   built from half an image would answer "this region is empty", which is a picture claim nobody can
 *   trust.
 *
 * @param format The attachment's depth format.
 * @param bytes  The copied bytes, in the tightly packed order the copy produced.
 * @return One value per texel, or empty when the bytes cannot be read as this format.
 */
[[nodiscard]] VN_GRAPHICS_API std::vector<float> decodeDepth(vn::graphics::RenderTarget::DepthFormat format, std::span<const std::byte> bytes);

VN_GRAPHICS_NS_END
