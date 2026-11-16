#pragma once

#include <optional>
#include <utility>

#include <vine/String.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderPass.hpp>

/**
 * @brief The ENGINE's one way to build a coded report about its own wiring (a private header: no host sees it).
 *
 * @details
 * - A cause is named where it is KNOWN, which is inside the engine: the compile, the frame resolution, a
 *   chain build and an area's refusals each know exactly what they could not honour. Building the value here
 *   (rather than handing three loose arguments to a report call) is what lets each site state its
 *   `engine.*` code and its subject, and it is the shape the diagnostic gate scans for
 *   (scripts/diagnostic_formats.py).
 * - ORIGIN IS ENGINE, SEVERITY IS WARNING: these reports describe a declaration the engine could not honour,
 *   not a failure of the device. A site that really cannot draw at all still reports through this builder -
 *   the picture it describes is the missing part, not the whole frame.
 * - NOT A PUBLIC BUILDER. `RenderEngine::reportEngineProblem(const RenderDiagnostic&)` is the public door;
 *   the values handed to it are built by this header and by `detail::compileProblem` (which differs only in
 *   the defaults it carries for the compile's own call sites).
 */
namespace VN_ROOT_NS
{
namespace graphics
{
namespace detail
{

/**
 * @brief Answers the name a pass is reported under: its own, or a readable fallback when it has none.
 *
 * @details
 * - ONE SPELLING for every report that names a pass (the compile's checks, the engine's capability report): the
 *   subject a host badges and the sentence it reads have to agree, and an unnamed pass is still something a host
 *   can act on.
 *
 * @param pass Pass to name (may be null).
 * @return The pass' name, or `(unnamed)` when it has none.
 */
[[nodiscard]] inline String reportedPassName(raw_ptr<const RenderPass> pass)
{
    if (pass == nullptr || pass->name().empty()) {
        return String(u8"(unnamed)");
    }
    return pass->name();
}

/**
 * @brief Builds a Warning about the engine's own wiring, with origin Engine and the cause's stable code.
 *
 * @param code     Stable identifier of the cause (`engine.*`, see scripts/diagnostic_codes.txt).
 * @param category What it is about.
 * @param message  The sentence (it names its subject too).
 * @param subject  What the report is about (a pass, an area, a target); may be empty.
 * @param limit    The numbers, when the report is about a device limit.
 * @return The diagnostic a report call carries.
 */
[[nodiscard]] inline RenderDiagnostic engineProblem(const char8_t*                 code,
                                                    const DiagnosticCategory      category,
                                                    String                        message,
                                                    String                        subject = {},
                                                    std::optional<DiagnosticLimit> limit = std::nullopt)
{
    RenderDiagnostic diagnostic;
    diagnostic.origin   = DiagnosticOrigin::Engine;
    diagnostic.severity = DiagnosticSeverity::Warning;
    diagnostic.category = category;
    diagnostic.code     = String(code);
    diagnostic.message  = std::move(message);
    diagnostic.subject  = std::move(subject);
    diagnostic.limit    = limit;
    return diagnostic;
}

} // namespace detail
} // namespace graphics
} // namespace VN_ROOT_NS
