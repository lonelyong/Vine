#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/graphics_global.hpp>

/**
 * @brief What a readback request produces, and why it may not produce anything - ONE table.
 *
 * @details
 * - WHY A TABLE AND NOT A PREDICATE PER CALL SITE. A readback has entry points (a colour probe, a depth
 *   probe, the readback node a frame records) and every one of them has to answer the same question: can
 *   this be served at all? Written out per entry point, the answers drift - one call refuses an
 *   unreadable depth format while another hands out a copy that copies nothing, and the caller cannot
 *   tell which of them it is talking to. Here the answer is a function of the FACTS (the target's
 *   extent, its attachment formats, whether a copy was ever handed out for recording) and the REQUEST,
 *   so every entry point gets the same classification from the same place.
 * - WHY THE CLASSIFICATION COMES BEFORE ANY WORK. A refused readback must not stop the device or copy
 *   anything first: a caller asking for pixels that cannot exist gets a category, not a device idle
 *   followed by an empty buffer. The categories are also split by what a caller can DO about them:
 *   - `NotCaptured` - this backend can serve it once a frame records the copy, so a caller may retry
 *     after the next frame;
 *   - `UnreadableFormat` - this backend will never serve it for this target, whatever frames run (a
 *     combined depth/stencil attachment has no plain depth copy, and the colour half has no packing for a float
 *     texel);
 *   - `UnknownAttachment` - the request names something the target does not have.
 * - "Not yet" and "never" are different answers, and a phase that reports the wrong one sends its reader
 *   looking for a bug in the wrong place.
 * - TWO LEVELS, ONE ANSWER EACH: `ReadbackRefusal` is the seam's finer category (which attachment, which format,
 *   whether a copy was handed out); `ReadbackStatus` is what a host acts on, and `readbackStatusOf()` is the single
 *   projection between the two. A host never sees `ReadbackRefusal`; the seam answers "servable" with
 *   `std::nullopt`, one spelling for one fact, rather than a `None` enumerator beside an `ok` flag.
 * - THE ANSWER IS ONE VALUE (see `ReadbackResult`): the classification and the picture travel together, so a refusal
 *   carries no picture at all and no verb has an out-parameter that is only sometimes written. A caller cannot read
 *   bytes a refused call never produced, which the old `(…, out, why)` shape left open.
 * - WHAT IS DELIBERATELY NOT HERE: a "nothing has been laid out yet" category. A target of no size
 *   cannot exist - `create` refuses a zero extent, and before a target exists at all the LIFECYCLE plan
 *   answers `Repair(SizeUnknown)` for it (`planTarget`, which is where the surface's size authority and
 *   the hidden / not-yet-laid-out cases live). A refusal arm nothing can reach reads as a covered case
 *   while covering none.
 * - PRECEDENCE is deliberate and tested: the request's own existence first, then the format (a property
 *   of the target), then the extent (a fact about now), then whether the copy has been recorded. A
 *   caller that asks for an attachment that does not exist gets `UnknownAttachment` even if the target
 *   is also empty - the strongest permanent reason wins, so the answer does not change from frame to
 *   frame for a request that can never be served.
 * - WHAT THIS FILE DOES NOT DO: it does not touch the pixels. Deciding that a readback is servable and
 *   handing out the bytes are different halves (the bytes belong to the device layer); the only
 *   pixel-level thing here is the FORMAT table, because "how many bytes is one texel" and "may this
 *   format be read at all" are the same fact that decides servability.
 */
VN_GRAPHICS_NS_BEGIN

/** @brief Which attachment of a target a readback is about. */
enum class ReadbackKind : std::uint8_t
{
    Color, // A colour attachment (see ReadbackRequest::attachment).
    Depth, // The depth attachment (there is at most one).
};

/** @brief Why a readback cannot be served (see the file note for what each one lets a caller do). */
enum class ReadbackRefusal : std::uint8_t
{
    UnknownAttachment, // The target has no such attachment.
    UnreadableFormat,  // This backend cannot read that attachment's format, ever.
    NotCaptured,       // No copy of that attachment has been handed out for recording (yet).
};

/** @brief What the HOST makes of a readback: the classification every `ReadbackResult` carries.
 *
 * @details
 * - This is the host-facing vocabulary and the only one a caller switches on: five cases, each one a different thing
 *   to DO. `Unsupported` gates a feature off, `NotReady` retries after a frame, `Invalid` is a caller bug, `Failed`
 *   is reported.
 * - WHY `Status` AND NOT `Result` (the spelling this carried while a readback answered with nothing but this code):
 *   the verbs now return `ReadbackResult<T>` - the classification WITH the picture it classified - so the code is one
 *   member of that answer and is named for what it is (`G4`, `G6`; `FrameCallOutcome::Status` is the same rule, and
 *   `SurfaceTargetResult`, which carries no product of its own, keeps `Result`).
 * - It is a FAMILY-LEVEL name, not a member of the template: `ColorReadback` and `DepthReadback` are two
 *   instantiations of one answer, and a status nested in the template would have made them two incompatible types
 *   for one vocabulary.
 * - `ReadbackRefusal` above is the seam's finer view of the same facts (which attachment, which format, whether a
 *   copy was handed out); it maps onto this enum through `readbackStatusOf()` and never crosses to a host.
 */
enum class ReadbackStatus : std::uint8_t
{
    Ok,          // The pixels / depths were read (see ReadbackResult::values).
    Unsupported, // This device cannot read this back: no readback support, or a format / usage it does not decode.
    NotReady,    // There is nothing to read yet: unknown target, no built attachments, no usable size.
    Invalid,     // The request itself was wrong (no target, attachment index out of range).
    Failed,      // The copy was attempted and failed (no usable device, submission error).
};

/** @brief One readback answer: the picture, and the classification that says whether it is one.
 *
 * @details
 * - ONE QUESTION, ONE ANSWER (`G3`, `G4`). The pixels / depths are this value's PRODUCT, not an out-parameter beside
 *   a bool, so a refusal carries no picture at all and a caller cannot read stale bytes a refused call never wrote.
 * - `values` is populated exactly when `status` is #ReadbackStatus::Ok, one element per texel of the source image in
 *   the verb's own packing (see `RenderDevice::readColorBuffer` for the byte family and
 *   `RenderDevice::readDepthBuffer` for the normalised floats), row-major, first row at the top.
 * - `ColorReadback` and `DepthReadback` are the two products the device verbs answer with. `ReadbackResult` is NOT a
 *   general `expected` (`G8`): it has no error alternative, no monadic operations and no other consumer, and the
 *   element type is the only thing the two products do not share.
 * - The boolean conversion is explicit (`G4`: a whole picture is not a flag), so `if (read)` and `read.ok()` read
 *   the same answer and nothing silently turns 8 MB of pixels into `true` in an overload resolution.
 */
template <class T>
struct ReadbackResult {
    /// The classification: #ReadbackStatus::Ok exactly when @ref values holds a picture.
    ReadbackStatus status{ ReadbackStatus::Failed };
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

/** @brief Projects a seam refusal onto the host's classification (the ONE mapping).
 *
 * @details
 * - `UnknownAttachment` is `Invalid` (the request names something that is not there); `UnreadableFormat` is
 *   `Unsupported` (this device never will); `NotCaptured` is `NotReady` (a later frame can serve it).
 * - A backend that classifies with `readbackOf()` reports through this function, so the same seam fact cannot be
 *   told to a host as different answers by different code paths.
 *
 * @param refusal The seam's category.
 * @return The host's classification; never #ReadbackStatus::Ok (a refusal is an answer, not a read).
 */
[[nodiscard]] VN_GRAPHICS_API ReadbackStatus readbackStatusOf(ReadbackRefusal refusal) noexcept;

/** @brief What is asked for. */
struct ReadbackRequest {
    ReadbackKind  kind{ ReadbackKind::Color }; // Which attachment kind.
    std::uint32_t attachment{ 0 };             // Colour attachment index; ignored for Depth.
};

/** @brief What the target currently has, as facts (no device work, no pixels).
 *
 * The extent is not among them on purpose: a live target always has a usable one (see the file note).
 */
struct ReadbackState {
    std::uint32_t                           color_attachments{ 0 }; // How many colour attachments the target has.
    vn::graphics::RenderTarget::ColorFormat color_format{ vn::graphics::RenderTarget::ColorFormat::RGBA8 }; // The format of the requested colour attachment.
    /// The depth attachment's format; empty when the target has no depth at all.
    std::optional<vn::graphics::RenderTarget::DepthFormat> depth_format;
    bool                                                   color_captured{ false }; // A copy-back node for the requested colour attachment was handed out.
    bool                                                   depth_captured{ false }; // A copy-back node for the depth attachment was handed out.
};

/** @brief How one readback format packs its texels, and whether it can be read at all. */
struct VN_GRAPHICS_API ReadbackFormat {
    std::uint32_t bytes_per_texel{ 0 }; // Bytes one texel occupies in the copy; 0 when unreadable.
    bool          readable{ false };    // Whether this backend can read that format.

    /** @brief Compares both halves (one spelling for one fact: no reachable unreadable pair differs). */
    [[nodiscard]] bool operator==(const ReadbackFormat& other) const noexcept;
};

/** @brief Decides whether @p request can be served for @p state (see the file note for the precedence).
 *
 * @details
 * - `std::nullopt` IS the servable answer and a value is the refusal that stops it: one spelling per fact, where the
 *   old `{ ok, ReadbackRefusal::None }` pair could say "served" and "no reason" and still disagree.
 *
 * @param state   The target's facts.
 * @param request What is asked for.
 * @return Nothing when the readback is servable, else the refusal category.
 */
[[nodiscard]] VN_GRAPHICS_API std::optional<ReadbackRefusal> readbackOf(const ReadbackState& state, const ReadbackRequest& request) noexcept;

namespace detail
{

/** @brief Gets a stable, machine-friendly name for @p refusal (for messages and test output).
 *
 * @details
 * - The machine answer a host acts on is `ReadbackStatus`; this name exists for the seam's own messages and for
 *   the test that pins the spelling of each category, so it lives in `detail` (`G8`, `G1`: one vocabulary for the
 *   host, one for the seam).
 *
 * @param refusal The category.
 * @return The name; never null.
 */
[[nodiscard]] VN_GRAPHICS_API std::string_view refusalName(ReadbackRefusal refusal) noexcept;

} // namespace detail

/** @brief Gets how a colour format is read back.
 *
 * @details
 * - THE COLOUR READBACK SERVES THE 8-BIT FAMILY, AND A TEXEL'S WIDTH IS ITS CHANNEL COUNT: one byte per channel, so
 *   `R8` is 1, `RG8` is 2 and `RGBA8` is 4 bytes per texel - the bytes the attachment holds, in channel order. One
 *   rule covers a colour and a mask, because both are a byte per channel.
 * - THE FLOAT FAMILY IS REFUSED (`readable` false, 0 bytes): this table promises "copy the texels, no conversion",
 *   and a byte count alone does not describe a half or a full float to a caller decoding bytes. A float product is
 *   read back when a decoder for it lands - not by handing its bits back under an 8-bit contract.
 *
 * @param format The attachment's colour format.
 * @return Bytes per texel and whether it can be read.
 */
[[nodiscard]] VN_GRAPHICS_API ReadbackFormat colorReadbackOf(vn::graphics::RenderTarget::ColorFormat format) noexcept;

/** @brief Gets how a depth format is read back.
 *
 * @details
 * - The zero is the honest half: a combined depth/stencil image has no plain depth copy layout, and
 *   reading its first bytes as floats produces numbers that look like depths and are not.
 *
 * @param format The attachment's depth format.
 * @return Bytes per texel and whether it can be read.
 */
[[nodiscard]] VN_GRAPHICS_API ReadbackFormat depthReadbackOf(vn::graphics::RenderTarget::DepthFormat format) noexcept;

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
