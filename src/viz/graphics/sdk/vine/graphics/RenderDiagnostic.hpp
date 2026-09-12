#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <functional>

#include <vine/String.hpp>

V_GRAPHICS_NS_BEGIN

/**
 * @brief How bad a backend diagnostic is.
 *
 * A backend that cannot serve a request must say so instead of degrading
 * silently: the severity tells the host whether content is missing (Error) or
 * was adapted (Warning / Info).
 */
enum class DiagnosticSeverity : std::uint8_t
{
    Info,    ///< The request was served, adapted (e.g. an extra channel was dropped).
    Warning, ///< The request was partially served (e.g. the built-in shader replaced a user program).
    Error,   ///< The request could not be served: the content is not drawn.
};

/**
 * @brief What a backend diagnostic is about.
 *
 * Deliberately machine-matchable: hosts and tests switch on the category
 * instead of parsing the message, so editing a message never breaks a caller.
 * The categories are split by CONSEQUENCE (what the host must know), not by
 * cause, and severity is orthogonal to them. Append a new value before Count
 * so existing switches keep their meaning.
 */
enum class DiagnosticCategory : std::uint8_t
{
    GeometryRejected,  ///< A geometry's vertex/index data is unusable: it is not drawn at all.
    ChannelIgnored,    ///< One attribute channel was dropped: the mesh is still drawn.
    ShaderFallback,    ///< A user ShaderProgram could not be compiled: the built-in shader is used.
    CompileFailed,     ///< A shader/pipeline compile attempt failed (the backend may retry or give up).
    TargetBuildFailed, ///< An off-screen RenderTarget could not be built: passes using it draw nothing.
    ContentSkipped,    ///< A pass' content could not be prepared: that pass draws nothing this frame.
    InitFailed,        ///< The backend did not come up: nothing can be drawn at all.
    PassProtocolViolation, ///< The host misused the pass protocol: the announced state did not apply
                           ///< (a nested / unpaired beginPass/endPass, or a target released while
                           ///< it was still announced).

    Count, ///< Number of categories (not a category itself; keeps count arrays sized).
};

/**
 * @brief One backend diagnostic: how bad, about what, and why.
 */
struct V_GRAPHICS_API RenderDiagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Error;          ///< How bad it is.
    DiagnosticCategory category = DiagnosticCategory::GeometryRejected; ///< What it is about.
    String             message;                                       ///< Human-readable detail, with the numbers involved.
};

/**
 * @brief Callback a host installs to receive backend diagnostics.
 *
 * An empty sink (the default) means "nobody is listening": the backend still
 * counts what happened (RenderBackend::diagnosticCount()) and keeps its own
 * stderr tracing, so a host that installs no sink loses nothing but the
 * programmatic channel.
 */
using DiagnosticSink = std::function<void(const RenderDiagnostic&)>;

V_GRAPHICS_NS_END
