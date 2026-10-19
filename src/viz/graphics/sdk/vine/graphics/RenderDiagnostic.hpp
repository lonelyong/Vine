#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <functional>

#include <vine/String.hpp>

VN_GRAPHICS_NS_BEGIN

/**
 * @brief How bad a backend diagnostic is.
 *
 * @details
 * - A backend that cannot serve a request must say so instead of degrading silently: the severity tells the host
 *   whether content is missing (Error) or was adapted (Warning / Info).
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
 * @details
 * - Deliberately machine-matchable: hosts and tests switch on the category instead of parsing the message, so editing
 *   a message never breaks a caller.
 * - The categories are split by CONSEQUENCE (what the host must know), not by cause, and severity is orthogonal to
 *   them.
 * - Append a new value before Count so existing switches keep their meaning.
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
    PassProtocolViolation, ///< A pipeline statement that cannot be honoured was refused: a presentation the
                           ///< surface cannot serve (a view on another engine, a second surface), a window
                           ///< announced to a backend that has no surface, a target released while a frame
                           ///< still named it, a pass whose sampled inputs do not match the plan. The shape a
                           ///< frame is driven with cannot break this one - a frame is a value (see
                           ///< RenderFrameExecutor::executeFrame) - so what is left here is wiring the host declared.
    UnsupportedRequest,    ///< The host asked for something that is not implemented yet: a documented
                           ///< stand-in was assembled instead, so the picture is not what was asked
                           ///< for.
    SubmissionFailed,      ///< A frame's submission could not be made: the frame was not drawn, and the
                           ///< backend will repair what it recorded before the next one (a device-lost
                           ///< keeps failing, so a host seeing this repeatedly has lost the device).
    ResourcePressure,      ///< The frame is over a bound the engine cannot lift on its own, so work is being
                           ///< repeated rather than dropped: a texture evicted by an internal cache's entry
                           ///< bound is rebuilt and re-uploaded whenever it comes back. The picture is RIGHT,
                           ///< which is why this is not an error - the cost is what is reported, and the host
                           ///< is the one that can decide to hold fewer things at once.

    Count, ///< Number of categories (not a category itself; keeps count arrays sized).
};

/**
 * @brief One backend diagnostic: how bad, about what, and why.
 */
struct VN_GRAPHICS_API RenderDiagnostic
{
    DiagnosticSeverity severity = DiagnosticSeverity::Error;          ///< How bad it is.
    DiagnosticCategory category = DiagnosticCategory::GeometryRejected; ///< What it is about.
    String             message;                                       ///< Human-readable detail, with the numbers
                                                                      ///< involved.
};

/**
 * @brief Callback a host installs to receive backend diagnostics.
 *
 * @details
 * - An empty sink (the default) means "nobody is listening": the backend still counts what happened
 *   (RenderBackend::diagnosticCount()) and keeps its own stderr tracing, so a host that installs no sink loses
 *   nothing but the programmatic channel.
 */
using DiagnosticSink = std::function<void(const RenderDiagnostic&)>;

VN_GRAPHICS_NS_END
