#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/Object.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Capabilities.hpp"
#include "Diagnostics.hpp"
#include "RenderDiagnostic.hpp"
#include "RenderPass.hpp"
#include "WindowPresentTarget.hpp"
#include "ShaderProgram.hpp"

VN_GRAPHICS_NS_BEGIN

class RenderFrameExecutor;
class RenderTarget;

/**
 * @brief Why a readback did or did not happen (see readColorBuffer / readDepthBuffer).
 *
 * @details
 * - A bool alone answered four different questions with one value, so a caller could not tell "this device cannot read back at all" (expected - gate the feature on it) from "there is nothing to read YET" (retry after the target is rendered) from "the request was wrong" (a caller bug) from "the copy was attempted and failed" (report it).
 * - The methods keep their bool return, and a caller that needs the distinction passes a pointer to one of these: the same information an enum return would give, without re-spelling every `if (!read(...))` in a caller (this codebase's other reason-reporting entries use the same out-parameter shape, see VsgTextureCache::makeImage).
 * - The diagnostics channel still carries the human-readable sentence; this is the machine answer.
 */
enum class ReadbackResult
{
    Ok,          ///< The pixels / depths were read.
    Unsupported, ///< This device cannot read this back: no readback support, or a format / usage it does not decode.
    NotReady,    ///< There is nothing to read yet: unknown target, no built attachments, no usable size.
    Invalid,     ///< The request itself was wrong (no target, attachment index out of range).
    Failed,      ///< The copy was attempted and failed (no usable device, submission error).
};

/**
 * @brief Why a surface target was or was not handed out (see RenderDevice::createSurfaceTarget).
 *
 * @details
 * - The three ways a window can be refused are three different situations with three different answers for the host: a device that never presents (plan no window at all), a surface this device cannot serve (the window belongs to this device but not to this surface: a cross-GPU drag, a kind it does not decode - the host either keeps drawing off-screen or brings up a device that can), and a window already occupied (the host asked for a second target on a window that has one - one live swapchain per window is a platform constraint, see WindowPresentTarget).
 * - Like ReadbackResult, the machine answer travels in an out-parameter and the sentence travels on the diagnostics channel; "this device presents nothing" is answered SILENTLY, because a display-less device is a first-class state rather than a failure (see Capabilities::presentation).
 */
enum class SurfaceTargetResult
{
    Ok,                 ///< The target was created.
    NoPresentation,     ///< This device presents nothing at all: it has no window half (never an error - see Capabilities::presentation).
    UnsupportedSurface, ///< This device has a window half but cannot serve THIS surface (a kind it does not decode, or a window it does not own).
    WindowOccupied,     ///< The window already has a live target: one window carries one live swapchain, so the second ask is refused.
};

/**
 * @brief The DEVICE half of a backend: what one GPU has, what it remembers, and the windows it can hand out.
 *
 * @details
 * - WHY A CONTRACT OF ITS OWN. A device is the longest-lived thing in the stack: windows come and go on top of it (WindowPresentTarget), frames are run by it or for it (RenderFrameExecutor), and one host may present through several windows while keeping ONE copy of what it uploaded and compiled. That is what this face answers: what the GPU can do (capabilities), what it retains for the host (targets, passes, content programs, transfers), and where its reports go (diagnostics). It is also the exchange-chain FACTORY - a target is created by the device that will present through it, so a host never has to build a swapchain to get a device (see createSurfaceTarget, and Q1/Q2 of the axis that shaped this: a device is not initialized with a window, but "will anything be shown" is part of what it is).
 * - WHAT A DEVICE IS TOLD, and when - each is a RESOURCE announcement, not a frame call:
 *   - setRenderTarget(target): a target the host will draw into or read back, so the device can hold it and build its attachments before any frame names it (null selects the default framebuffer, which needs none);
 *   - releasePass / releaseRenderTarget: the pass / target is going away - free what it owns (see each entry);
 *   - setDefaultContentProgram: the program content with no program of its own is drawn with;
 *   - setDiagnosticsRoute / setDiagnosticSink: where this device's reports go.
 * - WHAT A DEVICE IS ASKED, and what each answer is:
 *   - capabilities(): what it can do, as ONE value (Capabilities) - asked before a frame is driven, because a capability decides how it is driven;
 *   - createSurfaceTarget(): the window half, one target per window (WindowPresentTarget), or the machine-readable reason it handed none out (see SurfaceTargetResult);
 *   - readColorBuffer / readDepthBuffer: a target's pixels (or depths), or the machine-readable reason there are none (see ReadbackResult - `Unsupported` is the expected answer for "this device cannot", which is what a host gates the feature on);
 *   - diagnosticCount() / diagnosticsClean(): what it had to say, counted on the ONE route the engine handed over (setDiagnosticsRoute), so the host's sink and this device's own evidence read the same numbers.
 * - THREADING. The engine drives one frame at a time from one thread, so calls into a device are serialised, never concurrent and never reentrant. A device may therefore keep its state unsynchronised, but must not assume it stays on the same thread across an initialize()/shutdown() pair. The diagnostic sink is invoked synchronously from inside a device call, so a sink must record and return, never call back into the device.
 * - FAILURE. A device reports what it could not do instead of degrading silently (see setDiagnosticSink) and never throws across this interface: initialize() returns false, the readback methods return false, createSurfaceTarget() answers nullptr with a reason, and a request that cannot be served leaves the previous state intact. A false return never means "partially applied". A false initialize() must not leave a half-built session behind: the engine calls shutdown() only after a successful initialize() (see RenderEngine::shutdown), so the device owns the cleanup of its own partial state.
 * - RenderBackend is the aggregate a device that also executes frames and presents through one window implements today (see its own note); a device is reference-counted through that aggregate, which is what a factory returns.
 */
class VN_GRAPHICS_API RenderDevice
{
  public:
    virtual ~RenderDevice() = default;

    /** @brief Initializes the device: queues, caches, pipelines - and the windows it was asked to serve.
     *
     * @details
     * - Called once per session, after the host asked for what it wants presented (createSurfaceTarget) and announced the default content program (setDefaultContentProgram).
     * - A device that is already initialized tears the previous session down first, so this may be called again on a recreated surface.
     *
     * @return true when the device is ready to render; false when a required resource could not be created. On false the device owns the cleanup of whatever it built (the engine does not call shutdown() after a failed initialize()) and reports the reason on its diagnostics channel.
     */
    virtual bool initialize() = 0;

    /** @brief Releases everything the device owns for the current session.
     *
     * @details
     * - Safe to call after a failed initialize(), before any initialize(), or twice: when it returns, the device must be in the state of a freshly constructed one and initializable again.
     * - Every window / GPU object the session created must be gone — the engine calls it both when the renderer shuts down and when the rendering surface is recreated.
     */
    virtual void shutdown() = 0;

    /** @brief Gets the frame half of this device: how one frame comes out (see RenderFrameExecutor).
     *
     * @details
     * - A device that runs its own frames answers itself; one that separates the two hands out the object that does.
     * - The answer is a property of the device, not a state: it does not change over a session, and the reference stays valid for as long as the device lives.
     *
     * @return The frame executor of this device.
     */
    [[nodiscard]] virtual RenderFrameExecutor& executor() = 0;

    /** @brief Creates a target this device presents a window through (see WindowPresentTarget).
     *
     * @details
     * - THE WINDOW HALF, created where the swapchain will live: one live target per window, because a window carries one live swapchain (a platform constraint, not a choice - see WindowPresentTarget).
     * - The handle is a CREATION input rather than a later announcement, which is what a device needs before it comes up: a Vulkan device is built with the surface it will present to (see the axis' Q1), so a host names the window here and the target is what a recreated window is later re-announced on (WindowPresentTarget::setWindowHandle).
     * - A device that presents nothing answers nullptr with NoPresentation and NO report: a display-less device is a first-class state rather than a failure. The other two refusals are reported once per episode on the diagnostics channel, because a host that asked for a window it cannot have has to hear it.
     *
     * @param native_handle Native window handle (HWND on Windows) to present through, or nullptr when the host names no window and the device may bring up one of its own (or answer NoPresentation when it has no window half at all).
     * @param why           Receives why no target was created (Ok when one was), or null to ignore; see SurfaceTargetResult.
     * @return The target, or nullptr when this device hands none out (see @p why). The target is BORROWED, and the device that made it owns it: it lives until that device is DESTROYED or the area is unbound (see RenderSurface::bindTarget - RenderEngine::shutdown unbinds before it takes the device down). A window closing is the device's own business: it DETACHES the target (the handle is gone, and a resize or a present afterwards is a no-op that answers the truth), it does not hand the area a dangling object.
     */
    [[nodiscard]] virtual raw_ptr<WindowPresentTarget> createSurfaceTarget(void* native_handle,
                                                                         SurfaceTargetResult* why = nullptr)
    {
        (void)native_handle;
        if (why != nullptr) {
            *why = SurfaceTargetResult::NoPresentation;
        }
        return nullptr;
    }

    /** @brief Sets a render target the host will use.
     *
     * @details
     * - The host's announcement of a target it is about to draw into or read back - a RESOURCE announcement, not a frame call: which pass draws into which target travels inside the frame value (see PassDescription::target), and this is how a host configures a target BEFORE any frame names it, which is what the device builds the target's GPU attachments from (lazily, and again when its description changes).
     * - nullptr selects the default framebuffer, which needs no announcement: a frame that names no target draws into it.
     *
     * @param target Render target the host will use, or nullptr for the default framebuffer.
     */
    virtual void setRenderTarget(RenderTarget* target) = 0;

    /** @brief Releases the device GPU resources a pass owns.
     *
     * @details
     * - Called by the engine just before a removed pass is dropped. The device owns everything it retained for that pass (its content view / compiled pipelines / per-pass scene-bridge cache, any sampling slot it drew through, and the window layer it presented through), so it must free that state here to keep a closed resource loop; the RenderPass object itself stays a logical description owned by the caller.
     * - It is separate from releaseRenderTarget(): a pass may own a render target (freed by both), while the retained per-pass GPU state is only known to the device and is keyed by the pass identity a frame's entry names (see FrameDescription::Pass).
     * - The default no-op lets devices that keep no per-pass GPU state ignore the call.
     *
     * @param pass The pass being removed (borrowed for the call).
     */
    virtual void releasePass(raw_ptr<const RenderPass> pass)
    {
        (void)pass;
    }

    /** @brief Releases device GPU resources for a removed render target.
     *
     * @details
     * - Called by the engine before a target's owning pass/slot is dropped: the device frees the attachments it built for that target (and the sampling slots that referenced it).
     * - The RenderTarget object stays a logical description owned by the caller, and the device must not keep a pointer to it.
     *
     * @param target The target being removed (borrowed for the call).
     */
    virtual void releaseRenderTarget(RenderTarget* target)
    {
        (void)target;
    }

    /** @brief Reads back a colour attachment of an off-screen render target.
     *
     * @details
     * - Synchronously copies colour attachment @p attachment of @p target (a target this device rendered into) into @p outPixels as tightly packed RGBA8 — width * height * 4 bytes, row-major. This is the controlled readback entry point: a logical RenderTarget holds no GPU pixels, so the transfer belongs to the device that owns the attachments.
     * - The default implementation reports the operation as unsupported; a device that implements readback overrides it (staging buffer, image-to-buffer copy, queue/fence synchronisation and format conversion).
     * - A false return is one of several causes — the device does not support readback, the target was never rendered into / built, the target cannot answer, or the transfer failed. Pass @p why to tell them apart programmatically (ReadbackResult); the device also reports the reason on its diagnostics channel (see the class contract), and @p outPixels is left untouched either way.
     *
     * @param target     Off-screen target whose colour attachment to read. Borrowed, and CONST: reading pixels changes nothing about the target, and a host holding one as const (a reader of a target it did not create) can still ask for its pixels.
     * @param attachment Colour attachment index in [0, target->colorCount()).
     * @param outPixels  Receives the packed RGBA8 pixels on success.
     * @param why        Receives why the read did not happen (Ok when it did), or null to ignore. A device that does not implement readback leaves it at Unsupported.
     * @return true when the pixels were read; false when the read could not be performed (see @p why and the diagnostics channel).
     */
    virtual bool readColorBuffer(const vn::graphics::RenderTarget* target, int attachment,
                                 std::vector<std::uint8_t>& outPixels, ReadbackResult* why = nullptr)
    {
        (void)target;
        (void)attachment;
        (void)outPixels;
        if (why != nullptr) {
            *why = ReadbackResult::Unsupported;
        }
        return false;
    }

    /** @brief Reads back the depth attachment of an off-screen render target.
     *
     * @details
     * - Synchronously copies @p target's depth buffer into @p outDepths as width * height floats in [0, 1], row-major.
     * - Ownership and support model match readColorBuffer(): the default implementation reports the operation as unsupported.
     * - A target whose depth is borrowed (RenderTarget::shareDepth) is read through the SOURCE target, not the borrower, which reports the request as unsupported.
     *
     * @param target    Off-screen target whose depth buffer to read (borrowed, and const like readColorBuffer's).
     * @param outDepths Receives the depth values on success.
     * @param why       Receives why the read did not happen (Ok when it did), or null to ignore; see readColorBuffer().
     * @return true when the depth values were read; false when the read could not be performed (see @p why and the diagnostics channel).
     */
    virtual bool readDepthBuffer(const RenderTarget* target, std::vector<float>& outDepths,
                                ReadbackResult* why = nullptr)
    {
        (void)target;
        (void)outDepths;
        if (why != nullptr) {
            *why = ReadbackResult::Unsupported;
        }
        return false;
    }

    /** @brief Gets what this device can do (see Capabilities).
     *
     * @details
     * - The engine asks this where a capability decides how a frame is driven - before a pass staged through a target is handed over, and once per frame, because a capability that changes under a running session would mean the host planned the last frame against a stale description (see RenderEngine::frame).
     * - A device that can do nothing extra says so by the DEFAULT (every field false, see Capabilities); a device that can do something overrides this and answers its own description.
     *
     * @return The capabilities of this device, whole.
     */
    [[nodiscard]] virtual Capabilities capabilities() const noexcept
    {
        return {};
    }

    /** @brief Selects the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * @details
     * - A default, not an override: a drawable's own program still wins, and so does a pass' program for the passes that take one.
     * - There is no shading-model enum and no fallback: the host names a program (the engine starts with forwardProgram(), see RenderEngine), or content without its own program is NOT drawn and the reason is reported.
     * - May be called before initialize(), where it is a session decision baked as the session starts, or on a RUNNING session, where the device re-bakes the shading side so the next frame draws with @p program.
     * - A live switch rebuilds content shading only: the targets keep their attachments, their pass graphs and their depth history, so a host that offers a shading toggle gets a differently shaded picture rather than a restarted session.
     *
     * @param program Program to shade program-less content with, or null for "none" (such content is then reported and skipped).
     */
    virtual void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program)
    {
        (void)program;
    }

    /** @brief Installs the route every diagnostic this device reports goes through.
     *
     * @details
     * - The engine owns the one route (see Diagnostics.hpp) and hands it to the device it drives, so the engine's own reports and this device's reports are counted and forwarded in the same place: a host that asks the engine "did anything unexpected happen" gets one answer, not half of one.
     * - A device driven without an engine (a test, a tool) is never handed one and answers every accessor below from the private route it was born with - so such a user sees no change at all.
     *
     * @param route Route to report through; borrowed, and must outlive this device's reports.
     */
    virtual void setDiagnosticsRoute(Diagnostics& route);

    /** @brief Installs the sink that receives device diagnostics.
     *
     * @details
     * - A device reports what it could not serve (a rejected geometry, a dropped channel, a shader that fell back to the built-in one, an off-screen target it could not build) instead of degrading silently, so a host can surface it — log, overlay, telemetry — without the device having to link a logging framework or write to stderr.
     * - Installing no sink keeps the device's own stderr tracing and still counts every diagnostic (diagnosticCount()).
     * - The sink is invoked synchronously from inside the device call that reported, so it must only record and return — calling back into the device from a sink is unsupported.
     * - The sink belongs to the ACTIVE route, not to this object: once an engine has installed its route, this call replaces the host's sink for the whole engine. A host that drives an engine installs it there instead (RenderEngine::setDiagnosticSink).
     *
     * @param sink Callback invoked for every diagnostic, or empty to clear.
     */
    virtual void setDiagnosticSink(DiagnosticSink sink);

    /** @brief Gets the installed diagnostic sink (empty when unset).
     *
     * @return The sink a previous setDiagnosticSink() installed on the active route.
     */
    const DiagnosticSink& diagnosticSink() const;

    /** @brief Gets how many diagnostics were reported through the active route.
     *
     * @details
     * - Counted whether or not a sink is installed, so a host can gate on "did anything unexpected happen" without listening. Once an engine has installed its route this is the engine's count as well: there is one route, so there is one number.
     *
     * @return Total number of diagnostics reported through the active route.
     */
    std::size_t diagnosticCount() const;

    /** @brief Gets how many diagnostics of @p category were reported through the active route.
     *
     * @param category Category to count.
     * @return Number of reported diagnostics in that category.
     */
    std::size_t diagnosticCount(DiagnosticCategory category) const;

  protected:
    /**
     * @brief Gets the route this device reports through (its own, or the engine's).
     *
     * @details
     * - For a device that builds sub-objects which report on their own (a session, an executor): they must be handed this route rather than a second one, or their reports would be counted elsewhere and their sentences would never reach the host's sink.
     *
     * @return The active diagnostic route.
     */
    [[nodiscard]] Diagnostics& diagnosticsRoute() noexcept { return *route_; }

    /**
     * @brief Reports one diagnostic to the installed sink and counts it.
     *
     * @details
     * - Devices call this instead of failing silently. The device keeps its own out-of-band tracing (e.g. stderr) in addition to the sink, so an unmodified host sees today's behaviour plus a programmatic channel.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, with the numbers involved.
     */
    void reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category, const String& message);

    RenderDevice() = default;

  private:
    /// The route used when no engine handed one over (see setDiagnosticsRoute).
    Diagnostics  owned_route_;
    /// The active route: this device's own until an engine installs its.
    Diagnostics* route_ = &owned_route_;
};

VN_GRAPHICS_NS_END
