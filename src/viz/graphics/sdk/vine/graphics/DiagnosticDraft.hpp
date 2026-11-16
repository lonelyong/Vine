#pragma once

#include <optional>

#include <vine/graphics/graphics_global.hpp>
#include <vine/String.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>

/**
 * @brief THE ONE SHAPE a backend's "this call could not be served" takes: the cause's stable code, the
 *        category it belongs to, what it is about, the limit it hit and the sentence.
 *
 * @details
 * - WHY ONE SHAPE. Three spellings of one refusal grew in this tree (the SDK's `RenderDiagnostic`, the vk
 *   backend's `ContentRefusal`, the vsg backend's enum-plus-message pair). They mean the same thing and
 *   differ only in which fields they happen to carry, so every new backend had to pick one and every
 *   reader had to learn three. A draft is written where the cause IS KNOWN - deep in a planner that can
 *   see which stage refused - and handed on as a `RenderDiagnostic` at the report site; a fourth backend
 *   inherits the shape instead of inventing a fourth (G1 / G6).
 * - ONE WRITER, FIRST REASON WINS. `refuse()` is the only way to put anything in a draft, and a second
 *   refusal on the same draft does nothing: the first reason is the one that explains the call, and a
 *   later stage failing as a CONSEQUENCE must not replace the cause. Every planner used to spell that rule
 *   out with `if (why.empty())` by hand.
 * - A CODE IS THE REFUSAL. `refuse()` always states one, so "refused" and "has a code" are the same
 *   question - which is what keeps a caller from reporting a sentence with no matchable cause. The codes
 *   are the reporting module's own vocabulary (`vk.content.*`, `vsg.readback.*`, ...), registered in
 *   `scripts/diagnostic_codes.txt`.
 * - NOT A REPORT. This value is bookkeeping for one call; it holds no severity and no origin, because the
 *   party that hands it to the route knows both (see `diagnostic`).
 *
 * @note Not thread-safe only in the sense every value is: it is written and read by one call's thread.
 */
VN_GRAPHICS_NS_BEGIN

class VN_GRAPHICS_API DiagnosticDraft {
  public:
    /** @brief Builds "nothing was refused": no category, no code, no sentence. */
    DiagnosticDraft();

  public:
    /**
     * @brief Records @p message under @p code as the reason, unless a reason is already recorded.
     *
     * @details
     * - The message is taken even when empty: a caller that has nothing to add still states the code, and
     *   the report then carries the code with no sentence rather than the wrong cause.
     *
     * @param category The coarse class the cause belongs to.
     * @param code     The stable identifier (`vk.content.*`), never null.
     * @param message  The sentence for a human (may be empty).
     */
    void refuse(vn::graphics::DiagnosticCategory category, const char8_t* code, vn::String message);

    /**
     * @brief Records the same reason WITH the object it is about, unless one is already recorded.
     *
     * @param category The coarse class the cause belongs to.
     * @param code     The stable identifier, never null.
     * @param subject  What the refusal is about (a pass, a content object, a target); may be empty.
     * @param message  The sentence for a human (may be empty).
     */
    void refuse(vn::graphics::DiagnosticCategory category,
                const char8_t*               code,
                vn::String                   subject,
                vn::String                   message);

    /**
     * @brief Records the same reason with its object and the two numbers a limit refusal carries.
     *
     * @param category The coarse class the cause belongs to.
     * @param code     The stable identifier, never null.
     * @param subject  What the refusal is about; may be empty.
     * @param limit    What was asked for and what the device offers (`std::nullopt` when not a limit).
     * @param message  The sentence for a human (may be empty).
     */
    void refuse(vn::graphics::DiagnosticCategory               category,
                const char8_t*                                 code,
                vn::String                                     subject,
                std::optional<vn::graphics::DiagnosticLimit>   limit,
                vn::String                                     message);

    /** @brief Forgets any reason, so the value can be handed to another call (a recorder does this on entry). */
    void clear() noexcept;

    /** @brief Answers whether a reason was recorded. */
    [[nodiscard]] bool isRefused() const noexcept;

    /** @brief Gets the category of the recorded reason (ContentSkipped when nothing was refused). */
    [[nodiscard]] vn::graphics::DiagnosticCategory category() const noexcept;

    /** @brief Gets the recorded reason's stable code, or null when nothing was refused. */
    [[nodiscard]] const char8_t* code() const noexcept;

    /** @brief Gets what the refusal is about (empty when nothing was refused, or when it names no object). */
    [[nodiscard]] const vn::String& subject() const noexcept;

    /** @brief Gets the numbers of a limit refusal, or `std::nullopt` (also when nothing was refused). */
    [[nodiscard]] const std::optional<vn::graphics::DiagnosticLimit>& limit() const noexcept;

    /** @brief Gets the recorded sentence (empty when nothing was refused, or when the refusal stated only a code). */
    [[nodiscard]] const vn::String& message() const noexcept;

    /**
     * @brief Turns the draft into the SDK value a route reports, under @p severity.
     *
     * @details
     * - A refusal is a failure to serve a request, so callers pass Warning or Error per their contract (a
     *   skipped draw is a warning, a skipped frame an error); the draft itself carries no severity.
     * - The origin is left at the SDK's default for a backend report (see RenderDiagnostic::origin): a
     *   party that is not the device stamps its own.
     *
     * @param severity How bad the refusal is.
     * @return The diagnostic carrying this draft's category, code, subject, limit and sentence.
     */
    [[nodiscard]] vn::graphics::RenderDiagnostic diagnostic(vn::graphics::DiagnosticSeverity severity) const;

  private:
    vn::graphics::DiagnosticCategory              category_{ vn::graphics::DiagnosticCategory::ContentSkipped };
    const char8_t*                                code_{ nullptr };
    vn::String                                    subject_{};
    std::optional<vn::graphics::DiagnosticLimit>  limit_{};
    vn::String                                    message_{};
};

VN_GRAPHICS_NS_END
