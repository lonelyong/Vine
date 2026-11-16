#pragma once
#include "graphics_global.hpp"

#include <cstdint>
#include <functional>
#include <optional>

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
    Info,    // The request was served, adapted (e.g. an extra channel was dropped).
    Warning, // The request was partially served (e.g. the built-in shader replaced a user program).
    Error,   // The request could not be served: the content is not drawn.
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
    GeometryRejected,      // A geometry's vertex/index data is unusable: it is not drawn at all.
    ChannelIgnored,        // One attribute channel was dropped: the mesh is still drawn.
    ShaderFallback,        // A user ShaderProgram could not be compiled: the built-in shader is used.
    CompileFailed,         // A shader/pipeline compile attempt failed (the backend may retry or give up).
    TargetBuildFailed,     // An off-screen RenderTarget could not be built: passes using it draw nothing.
    ContentSkipped,        // A pass' content could not be prepared: that pass draws nothing this frame.
    InitFailed,            // The backend did not come up: nothing can be drawn at all.
    PassProtocolViolation, // A pipeline statement that cannot be honoured was refused: a presentation the
                           // surface cannot serve (a view on another engine, a second surface), a window
                           // announced to a backend that has no surface, a target released while a frame
                           // still named it, a pass whose sampled inputs do not match the plan. The shape a
                           // frame is driven with cannot break this one - a frame is a value (see
                           // RenderFrameExecutor::executeFrame) - so what is left here is wiring the host declared.
    UnsupportedRequest,    // The host asked for something that is not implemented yet: a documented
                           // stand-in was assembled instead, so the picture is not what was asked
                           // for.
    SubmissionFailed,      // A frame's submission could not be made: the frame was not drawn, and the
                           // backend will repair what it recorded before the next one (a device-lost
                           // keeps failing, so a host seeing this repeatedly has lost the device).
    ResourcePressure,      // The frame hit a capacity the backend cannot lift THIS frame (currently a full
                           // material arena): the affected draws are skipped and the storage grows before the
                           // next frame. The picture is right except for those skipped draws, which is why this
                           // is a warning rather than an error - and the host can release content to avoid it.
    MaterialSlot,          // A material's texture slot does not match what the slot declares (a cube map in a
                           // data slot, a map in the wrong colour space): reported once per material and slot,
                           // and the picture is drawn with what the slot can bind (graphics-material.md F11).
                           // A slot the material does not have at all is not this - that is a legal material.

    Count, // Number of categories (not a category itself; keeps count arrays sized).
};

/**
 * @brief The two numbers a report about a LIMIT carries (see RenderDiagnostic::limit).
 *
 * @details
 * - A HOST ACTS ON THE PAIR, not on the sentence: "asks for 8 samples, this device offers 4" tells it to ask for 4,
 *   while the same fact in prose tells it nothing it can use programmatically.
 * - `supported == 0` means "this device offers none of it", which is a different answer from "the limit is 0".
 */
struct VN_GRAPHICS_API DiagnosticLimit
{
    std::uint64_t requested{ 0 }; ///< What was asked for (samples, bytes, a count).
    std::uint64_t supported{ 0 }; ///< What the device offers.
};

/**
 * @brief Who reported a diagnostic: the engine, the device it drives, or the layer above them.
 *
 * @details
 * - WHY IT EXISTS. The route counts what it was told, and both halves tell it things: the engine about its
 *   own wiring, a device about what it could not serve. A host may also inject a report of its own through
 *   the engine's public entry, and once that is counted the same way, "how many problems did the engine
 *   find" stops being decidable - a host bug reads exactly like a wiring bug (`G4` / `G11`).
 * - THE COUNTS THAT SAY "the engine" (RenderEngine::diagnosticCount, Diagnostics::total) cover Engine +
 *   Device and nothing else; a party that wants the host's number asks for it (A-06 / CAP-87).
 * - Append a new value before Count so existing tables keep their meaning.
 */
enum class DiagnosticOrigin : std::uint8_t
{
    Engine, // The render engine (or a helper acting for it) found it while compiling or attaching a frame.
    Device, // The render backend found it while serving a request - the type's default.
    Host,   // The layer above injected it through the engine's report entry; it is not the engine's finding.

    Count, // Number of origins (not an origin itself; keeps count tables sized).
};

/**
 * @brief One backend diagnostic: how bad, about what, why - and, when it applies, the object and the limit.
 *
 * @details
 * - FIVE FIELDS A HOST MATCHES ON, one it shows, one it acts on:
 *   - @ref category is the COARSE class (the twelve consequences the SDK defines);
 *   - @ref code is the STABLE identifier of the cause inside that class (`u8"vk.memory.out_of_memory"`): never
 *     reworded, never localised, so a host or a test matches on it instead of parsing @ref message (`R28`);
 *   - @ref subject names WHAT the report is about (a pass, a content object, a target), so a host can badge that
 *     object rather than a log line;
 *   - @ref limit carries the two numbers when the report is about a device limit;
 *   - @ref message is the human sentence, and it is the only field whose wording may change.
 * - THE CODE IS A STRING, NOT AN ENUM, because the vocabulary belongs to the MODULE that reports (this SDK knows no
 *   `Vk*` and no `vsg::` - see `C3` in the graphics design docs): each backend and the engine name their own codes
 *   under their own prefix, and `scripts/diagnostic_codes.txt` is the registry that keeps them from drifting
 *   (`check_diagnostic_formats.py` checks both directions).
 * - AN EMPTY CODE IS NOT A CODE. A report that names no finer cause than its category leaves @ref code empty; that is
 *   the shape the migration is ratcheting away from, and the gate refuses NEW reports of that shape.
 */
struct VN_GRAPHICS_API RenderDiagnostic {
    DiagnosticOrigin               origin   = DiagnosticOrigin::Device;             ///< Who reported it (see DiagnosticOrigin).
    DiagnosticSeverity severity = DiagnosticSeverity::Error;            // How bad it is.
    DiagnosticCategory category = DiagnosticCategory::GeometryRejected; // What it is about.
    String             code;     // Stable identifier of the cause; empty means "the category is all there is".
    String             message;  // Human-readable detail, with the numbers involved.
    String             subject;  // What it is about (pass / content / target name); empty when nothing.
    std::optional<DiagnosticLimit> limit; // The numbers, when the report is about a device limit.
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
