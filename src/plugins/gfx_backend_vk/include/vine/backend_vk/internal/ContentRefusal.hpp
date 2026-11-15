#pragma once

#include <vine/backend_vk/backend_vk_global.hpp>

#include <vine/String.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief Why a recording path drew nothing: the cause's STABLE CODE, the category it belongs to and the sentence.
 *
 * @details
 * - THE CAUSE IS NAMED WHERE IT IS KNOWN. A planner sees which stage refused (the program, the streams, the pipeline)
 *   and the callers above it do not, so the code is decided here and travels with the sentence. The device's report
 *   then forwards all three - which is what keeps `RenderDiagnostic::code` matchable instead of prose.
 * - ONE VALUE, NOT TWO OUT-PARAMETERS. The message and its category used to be separate `why` / `category` arguments,
 *   so a new refusal could fill one and forget the other; this type has no way to hold a message without a code
 *   (`refuse` is the only writer, and it is the only place the "first reason wins" rule lives).
 * - THE FIRST REASON WINS: a second `refuse` on the same value does nothing, because the first one is what explains
 *   the draw - the same rule the planner had when it tested `why.empty()` by hand.
 *
 * @note The codes are the `vk.content.*` family in `scripts/diagnostic_codes.txt`; a value that is not refused carries
 *       no code at all (`code()` answers null), so a caller must ask `isRefused()` first.
 */
class VN_BACKEND_VK_API ContentRefusal final
{
  public:
    /** @brief Builds "nothing was refused": no category, no code, no sentence. */
    ContentRefusal();

    /**
     * @brief Records @p text as the reason, unless a reason is already recorded.
     *
     * @details
     * - The message is taken even when empty: a caller that has nothing to add still states the code, and the report
     *   then carries the code with no sentence rather than the wrong cause.
     *
     * @param category The coarse class the cause belongs to.
     * @param code     The stable identifier (`vk.content.*`), never null.
     * @param text     The sentence for a human (may be empty).
     */
    void refuse(vn::graphics::DiagnosticCategory category, const char8_t* code, vn::String text);

    /** @brief Forgets any reason, so the value can be handed to another call (what `record` does on entry). */
    void clear() noexcept;

    /** @brief Answers whether a reason was recorded. */
    [[nodiscard]] bool isRefused() const noexcept;

    /** @brief Gets the category of the recorded reason (ContentSkipped when nothing was refused). */
    [[nodiscard]] vn::graphics::DiagnosticCategory category() const noexcept;

    /** @brief Gets the recorded reason's stable code, or null when nothing was refused. */
    [[nodiscard]] const char8_t* code() const noexcept;

    /** @brief Gets the recorded sentence (empty when nothing was refused, or when the refusal stated only a code). */
    [[nodiscard]] const vn::String& message() const noexcept;

  private:
    vn::graphics::DiagnosticCategory category_{ vn::graphics::DiagnosticCategory::ContentSkipped };
    const char8_t*                   code_{ nullptr };
    vn::String                       message_{};
};

VN_BACKEND_VK_NS_END
