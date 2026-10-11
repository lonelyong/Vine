#pragma once
#include "graphics_global.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vine/Color.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

#include "Capabilities.hpp"
#include "Diagnostics.hpp"
#include "FrameDescription.hpp"
#include "FrameStats.hpp"
#include "RenderDiagnostic.hpp"
#include "RenderPass.hpp"
#include "RenderSurfaceBinding.hpp"
#include "ShaderProgram.hpp"
#include "StateNode.hpp"

VN_GRAPHICS_NS_BEGIN

class Camera;
class Light;
class RenderTarget;
class RenderPass;
class ShaderProgram;
struct RenderCommand;

/**
 * @brief Why a readback did or did not happen (see readColorBuffer / readDepthBuffer).
 *
 * A bool alone answered four different questions with one value, so a caller could not tell
 * "this backend cannot read back at all" (expected — gate the feature on it) from "there is
 * nothing to read YET" (retry after the target is rendered) from "the request was wrong" (a
 * caller bug) from "the copy was attempted and failed" (report it). The methods keep their bool
 * return, and a caller that needs the distinction passes a pointer to one of these: the same
 * information an enum return would give, without re-spelling every `if (!read(...))` in a
 * caller (this codebase's other reason-reporting entries use the same out-parameter shape, see
 * VsgTextureCache::makeImage).
 *
 * The diagnostics channel still carries the human-readable sentence; this is the machine answer.
 */
enum class ReadbackResult
{
    Ok,          ///< The pixels / depths were read.
    Unsupported, ///< This backend cannot read this back: no readback support, or a format / usage it does not decode.
    NotReady,    ///< There is nothing to read yet: unknown target, no built attachments, no usable size.
    Invalid,     ///< The request itself was wrong (no target, attachment index out of range).
    Failed,      ///< The copy was attempted and failed (no usable device, submission error).
};

/**
 * @brief Abstract render backend interface.
 *
 * Defines the contract that concrete graphics backends (vsg/Vulkan, OpenGL,
 * null/test) must implement, and what the engine guarantees in return.
 *
 * ONE WIRE: THE FRAME IS A VALUE. A frame reaches a backend in ONE call - executeFrame(FrameDescription) - with
 * every pass of it in the order it runs, each entry carrying the pass' identity, its order, its resolved inputs
 * and everything the pass issues (see FrameDescription / PassDescription / DrawDescription). There is no pass
 * scope, no incremental state setter and no call order to keep: what a frame ran and what it drew is the SHAPE of
 * the value, so the sequence rules this interface used to document cannot be violated - they cannot be expressed.
 * The engine compiles the frame and hands that value over (see RenderEngine::frameDescription); a host that drives
 * one pass itself builds the same shape (RenderPass::execute -> singlePassFrame).
 *
 * A FRAME IS A PARAMETER, NOT AN INTERVAL. executeFrame() does the frame's whole body: it acquires the next
 * presentable image when it has a surface (that is what "this is a frame" means to a windowed backend), records
 * the value, and leaves it ready to be presented. There is no beginFrame() to pair it with, and no endFrame() to
 * close it: "this frame is done" is the call returning.
 *
 * THE WINDOW HALF IS OPTIONAL. A backend that has a surface is also a RenderSurfaceBinding (see surface()): the
 * engine announces the host window and the size through it and presents the frame it just ran through it, while
 * a backend that only draws into render targets answers surface() == nullptr and carries none of that - the four
 * window entries are not empty shells on this interface, they are absent from it.
 *
 * WHAT ELSE A BACKEND IS TOLD, and when - each is a RESOURCE announcement, not a frame call:
 *   * setRenderTarget(target): a target the host will draw into or read back, so the backend can hold it and
 *     build its attachments before any frame names it (null selects the default framebuffer, which needs none);
 *   * releasePass / releaseRenderTarget: the pass / target is going away - free what it owns (see each entry);
 *   * setDefaultContentProgram: the program content with no program of its own is drawn with;
 *   * setDiagnosticsRoute / setDiagnosticSink: where this backend's reports go.
 *
 * WHAT A BACKEND IS ASKED, and what each answer is:
 *   * capabilities(): what it can do, as ONE value (Capabilities) - asked before a frame is driven, because a
 *     capability decides how it is driven;
 *   * frameReport(): how its frames are going, as ONE value (FrameReport) - the frame that just ended, what is
 *     retained, where the clock is; what the backend does not keep stays disengaged rather than reading as zero;
 *   * readColorBuffer / readDepthBuffer: a target's pixels (or depths), or the machine-readable reason there are
 *     none (see ReadbackResult - `Unsupported` is the expected answer for "this backend cannot", which is what a
 *     host gates the feature on);
 *   * diagnosticCount() / diagnosticsClean(): what it had to say, counted on the ONE route the engine handed
 *     over (setDiagnosticsRoute), so the host's sink and this backend's own evidence read the same numbers.
 *
 * BORROWED ARGUMENTS. Every pointer or reference INSIDE a FrameDescription (camera, commands, lights, target,
 * program) is borrowed for the duration of executeFrame() and never owned: the producer keeps them alive while the
 * call runs, and the engine reuses its own storage for them (a pass' resolved input vector, a scene's command
 * list) as soon as the call returns. A backend that needs the data later must copy or upload it and must never
 * keep such a pointer - which includes a backend that retains the whole frame (a render-graph builder, an offline
 * renderer): it copies what it keeps, at the moment it is called.
 *
 * WHAT MAY BE RETAINED. Retained GPU state (a content view, a compiled pipeline, a sampling slot, a texture
 * cache) is expected, but it must be keyed by the lifetime of what it serves (a pass' identity, the target's
 * object), released by the matching release* call, and must not grow with the frame count: a long-running session
 * has to reach a steady state. A backend must never require the host to call a release* method for correctness -
 * the host calls them because a pass or target is gone.
 *
 * THREADING. The engine drives one frame at a time from one thread, so calls
 * into a backend are serialised, never concurrent and never reentrant. A
 * backend may therefore keep its state unsynchronised, but must not assume it
 * stays on the same thread across an initialize()/shutdown() pair. The
 * diagnostic sink is invoked synchronously from inside a backend call, so a
 * sink must record and return, never call back into the backend.
 *
 * FAILURE. A backend reports what it could not do instead of degrading
 * silently (see setDiagnosticSink) and never throws across this interface:
 * initialize() returns false, the readback methods return false, and a request
 * that cannot be served leaves the previous state intact. A false return never
 * means "partially applied". A false initialize() must not leave a half-built
 * session behind: the engine calls shutdown() only after a successful
 * initialize() (see RenderEngine::shutdown), so the backend owns the cleanup
 * of its own partial state.
 *
 * RenderBackend is reference-counted: factories return an intrusive_ptr and
 * RenderEngine keeps its own reference, so ownership and lifetime are
 * explicit.
 */
class VN_GRAPHICS_API RenderBackend : public Object, public RefCounted<RenderBackend> {
    VN_OBJECT_META_DECL;

  public:
    virtual ~RenderBackend() = default;

    /** @brief Initializes the backend: device, surface, pipelines, caches.
     *
     * Called once per session, after the host announced the native window
     * (RenderSurfaceBinding::setWindowHandle) and the default content program (setDefaultContentProgram). A backend
     * that is already initialized tears the previous session down first, so
     * this may be called again on a recreated surface.
     *
     * @return true when the backend is ready to render; false when a required
     *         resource could not be created. On false the backend owns the
     *         cleanup of whatever it built (the engine does not call
     *         shutdown() after a failed initialize()) and reports the reason
     *         on its diagnostics channel.
     */
    virtual bool initialize() = 0;

    /** @brief Releases everything the backend owns for the current session.
     *
     * Safe to call after a failed initialize(), before any initialize(), or
     * twice: when it returns, the backend must be in the state of a freshly
     * constructed one and initializable again. Every window / GPU object the
     * session created must be gone — the engine calls it both when the
     * renderer shuts down and when the rendering surface is recreated.
     */
    virtual void shutdown() = 0;

    /** @brief Executes a whole frame from its value (see FrameDescription).
     *
     * THE FRAME'S BODY, and the one frame entry this interface has: the engine has already compiled the frame
     * (which pass runs when, what its inputs resolved to, what each pass issues), so this is "run THIS", not
     * "here is one call at a time, in an order you have to know". Nothing is announced to the backend before or
     * after this call that is about the frame - the acquisition of the next presentable image (for a backend with
     * a surface) happens inside it, and presenting that image is the surface binding's job (see surface()).
     *
     * WHAT IT MUST DO, in the order a frame means: take the value's passes as they come (an entry with a null
     * pass is nothing to run; an entry whose description is empty is a pass that ran and issued nothing - the
     * backend keys per-pass retained state by its identity, so that is a fact it must not confuse with "the pass
     * was not in the frame"), and make the frame's content reach the target each pass names. How is the backend's
     * business: the vsg backend walks the value into its own collection stage and its session, a retained
     * renderer builds a graph from it, an offline renderer traverses it - the value says what ran, never how.
     *
     * The value is valid for the duration of this call (see the class contract), and a backend that keeps any of
     * it beyond the call copies what it keeps.
     *
     * @param frame The frame to execute, in run order (see FrameDescription).
     */
    virtual void executeFrame(const FrameDescription& frame) = 0;

    /** @brief Gets the window half of this backend, or nullptr when it has none.
     *
     * A backend that presents through a window answers a RenderSurfaceBinding (itself, or a part of itself), and
     * the engine drives the host's window and size announcements and the present through it. A backend that only
     * draws into render targets - an offline baker, a test double, a compute-style consumer - answers nullptr and
     * does nothing else: there is no window to bind, no size to follow and nothing to present, and no empty
     * implementation of those four calls to carry.
     *
     * The answer is a property of the backend, not a state: it does not change over a session and it is asked
     * from outside, never called back into the backend. The binding it returns stays valid for as long as the
     * backend is initialized (see RenderSurfaceBinding for its own contract).
     *
     * @return The surface binding this backend presents through, or nullptr when it presents nothing.
     */
    [[nodiscard]] virtual RenderSurfaceBinding* surface()
    {
        return nullptr;
    }

    /** @brief Sets a render target the host will use.
     *
     * The host's announcement of a target it is about to draw into or read back - a RESOURCE announcement, not a
     * frame call: which pass draws into which target travels inside the frame value (see
     * PassDescription::target), and this is how a host configures a target BEFORE any frame names it, which is
     * what the backend builds the target's GPU attachments from (lazily, and again when its description
     * changes).
     *
     * nullptr selects the default framebuffer, which needs no announcement: a frame that names no target draws
     * into it.
     *
     * @param target Render target the host will use, or nullptr for the default framebuffer.
     */
    virtual void setRenderTarget(RenderTarget* target) = 0;

    /** @brief Gets what this backend can do (see Capabilities).
     *
     * The engine asks this where a capability decides how a frame is driven - before a pass staged through a
     * target is handed over, and once per frame, because a capability that changes under a running session
     * would mean the host planned the last frame against a stale description (see RenderEngine::frame).
     *
     * A backend that can do nothing extra says so by the DEFAULT (every field false, see Capabilities); a
     * backend that can do something overrides this and answers its own description.
     *
     * @return The capabilities of this backend, whole.
     */
    [[nodiscard]] virtual Capabilities capabilities() const noexcept
    {
        return {};
    }

    /** @brief Gets this backend's own report about its frames (see FrameReport).
     *
     * The backend is the only place that knows how a frame actually went (how many pass scopes ran, how many
     * draws they issued, what is retained and what letting go cost, and where the frame clock is), so a host,
     * a HUD and a gate read ITS numbers instead of counting a second time from outside - and the report is
     * ONE answer, so "how is it going" is one question rather than three with three written-or-not flags.
     *
     * What the backend does not keep, it leaves DISENGAGED rather than zero: "no counters" and "a frame that
     * counted nothing" are different answers (see FrameReport).
     *
     * @return The report, with the parts this backend keeps engaged.
     */
    [[nodiscard]] virtual FrameReport frameReport() const noexcept
    {
        return {};
    }

    /** @brief Releases the backend GPU resources a pass owns.
     *
     * Called by the engine just before a removed pass is dropped. The backend
     * owns everything it retained for that pass (its content view / compiled
     * pipelines / per-pass scene-bridge cache, any sampling slot it drew
     * through, and the window layer it presented through), so it must free
     * that state here to keep a closed resource loop; the RenderPass object
     * itself stays a logical description owned by the caller. It is separate
     * from releaseRenderTarget(): a pass may own a render target (freed by
     * both), while the retained per-pass GPU state is only known to the
     * backend and is keyed by the pass identity a frame's entry names (see FrameDescription::Pass).
     *
     * The default no-op lets backends that keep no per-pass GPU state ignore
     * the call.
     *
     * @param pass The pass being removed (borrowed for the call).
     */
    virtual void releasePass(raw_ptr<const RenderPass> pass)
    {
        (void)pass;
    }

    /** @brief Releases backend GPU resources for a removed render target.
     *
     * Called by the engine before a target's owning pass/slot is destroyed.
     * The backend owns the target's GPU attachments (images / views / render
     * passes / framebuffers / a per-target scene bridge) plus any sampling
     * (PiP) state, so it must free them here to keep a closed resource loop.
     * The target object itself stays a logical description owned by the
     * caller. The default no-op lets backends without off-screen targets
     * ignore the call.
     *
     * The call also announces that the caller may destroy @p target now, so a
     * backend holding the target anywhere — the target it built objects for (see
     * setRenderTarget), a pass it is about to run that names it — must drop what it holds rather than keep
     * the pointer. A frame that would still have used it cannot be
     * honoured: what it drew into that target must be skipped and reported (with the reason) instead of
     * being silently redirected to the default framebuffer, which would draw
     * the content where the caller never asked for it.
     *
     * @param target The render target being removed, or nullptr.
     */
    virtual void releaseRenderTarget(vn::graphics::RenderTarget* target)
    {
        (void)target;
    }

    /** @brief Reads back a colour attachment of an off-screen render target.
     *
     * Synchronously copies colour attachment @p attachment of @p target (a
     * target this backend rendered into) into @p outPixels as tightly packed
     * RGBA8 — width * height * 4 bytes, row-major. This is the controlled
     * readback entry point: a logical RenderTarget holds no GPU pixels, so the
     * transfer belongs to the backend that owns the attachments. The default
     * implementation reports the operation as unsupported; a backend that
     * implements readback overrides it (staging buffer, image-to-buffer copy,
     * queue/fence synchronisation and format conversion).
     *
     * A false return is one of several causes — the backend does not support
     * readback, the target was never rendered into / built, the target cannot
     * answer, or the transfer failed. Pass @p why to tell them apart
     * programmatically (ReadbackResult); the backend also reports the reason on
     * its diagnostics channel (see the class contract), and @p outPixels is left
     * untouched either way.
     *
     * @param target     Off-screen target whose colour attachment to read. Borrowed, and CONST: reading
     *                   pixels changes nothing about the target, and a host holding one as const (a
     *                   reader of a target it did not create) can still ask for its pixels.
     * @param attachment Colour attachment index in [0, target->colorCount()).
     * @param outPixels  Receives the packed RGBA8 pixels on success.
     * @param why        Receives why the read did not happen (Ok when it did), or null
     *                   to ignore. A backend that does not implement readback leaves it
     *                   at Unsupported.
     * @return true when the pixels were read; false when the read could not be
     *         performed (see @p why and the diagnostics channel).
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
     * Synchronously copies @p target's depth buffer into @p outDepths as
     * width * height floats in [0, 1], row-major. Ownership and support model
     * match readColorBuffer(): the default implementation reports the
     * operation as unsupported. A target whose depth is borrowed
     * (RenderTarget::shareDepth) is read through the SOURCE target, not the
     * borrower, which reports the request as unsupported.
     *
     * @param target    Off-screen target whose depth buffer to read (borrowed, and const like
     *                  readColorBuffer's).
     * @param outDepths Receives the depth values on success.
     * @param why       Receives why the read did not happen (Ok when it did), or null
     *                  to ignore; see readColorBuffer().
     * @return true when the depth values were read; false when the read could
     *         not be performed (see @p why and the diagnostics channel).
     */
    virtual bool readDepthBuffer(const vn::graphics::RenderTarget* target,
                                 std::vector<float>& outDepths, ReadbackResult* why = nullptr)
    {
        (void)target;
        (void)outDepths;
        if (why != nullptr) {
            *why = ReadbackResult::Unsupported;
        }
        return false;
    }

    /** @brief Selects the DEFAULT program for content: what a drawable naming none of its own gets.
     *
     * A default, not an override: a drawable's own program still wins, and so does a pass' program
     * for the passes that take one. There is no shading-model enum and no fallback: the host names a
     * program (the engine starts with forwardProgram(), see RenderEngine), or content without its own
     * program is NOT drawn and the reason is reported.
     *
     * May be called before initialize(), where it is a session decision baked as the session starts,
     * or on a RUNNING session, where the backend re-bakes the shading side so the next frame draws
     * with @p program. A live switch rebuilds content shading only: the targets keep their
     * attachments, their pass graphs and their depth history, so a host that offers a shading toggle
     * gets a differently shaded picture rather than a restarted session.
     *
     * @param program Program to shade program-less content with, or null for "none" (such content is
     *                then reported and skipped).
     */
    virtual void setDefaultContentProgram(intrusive_ptr<const ShaderProgram> program)
    {
        (void)program;
    }

    /** @brief Installs the route every diagnostic this backend reports goes through.
     *
     * The engine owns the one route (see Diagnostics.hpp) and hands it to the backend it drives, so the
     * engine's own reports and this backend's reports are counted and forwarded in the same place: a
     * host that asks the engine "did anything unexpected happen" gets one answer, not half of one.
     *
     * A backend driven without an engine (a test, a tool) is never handed one and answers every accessor
     * below from the private route it was born with - so such a user sees no change at all.
     *
     * @param route Route to report through; borrowed, and must outlive this backend's reports.
     */
    virtual void setDiagnosticsRoute(Diagnostics& route);

    /** @brief Installs the sink that receives backend diagnostics.
     *
     * A backend reports what it could not serve (a rejected geometry, a
     * dropped channel, a shader that fell back to the built-in one, an
     * off-screen target it could not build) instead of degrading silently, so
     * a host can surface it — log, overlay, telemetry — without the backend
     * having to link a logging framework or write to stderr. Installing no
     * sink keeps the backend's own stderr tracing and still counts every
     * diagnostic (diagnosticCount()).
     *
     * The sink is invoked synchronously from inside the backend call that
     * reported, so it must only record and return — calling back into the
     * backend from a sink is unsupported.
     *
     * The sink belongs to the ACTIVE route, not to this object: once an engine
     * has installed its route, this call replaces the host's sink for the whole
     * engine. A host that drives an engine installs it there instead
     * (RenderEngine::setDiagnosticSink).
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
     * Counted whether or not a sink is installed, so a host can gate on
     * "did anything unexpected happen" without listening. Once an engine has
     * installed its route this is the engine's count as well: there is one
     * route, so there is one number.
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
    /** @brief Gets the route this backend reports through (its own, or the engine's).
     *
     * For a backend that builds sub-objects which report on their own (a session, an executor): they
     * must be handed this route rather than a second one, or their reports would be counted elsewhere
     * and their sentences would never reach the host's sink.
     *
     * @return The active diagnostic route.
     */
    [[nodiscard]] Diagnostics& diagnosticsRoute() noexcept { return *route_; }

    /** @brief Reports one diagnostic to the installed sink and counts it.
     *
     * Backends call this instead of failing silently. The backend keeps its
     * own out-of-band tracing (e.g. stderr) in addition to the sink, so an
     * unmodified host sees today's behaviour plus a programmatic channel.
     *
     * @param severity How bad the situation is.
     * @param category What it is about.
     * @param message  Human-readable detail, with the numbers involved.
     */
    void reportDiagnostic(DiagnosticSeverity severity, DiagnosticCategory category,
                          const String& message);

    RenderBackend() = default;

  private:
    /// The route used when no engine handed one over (see setDiagnosticsRoute).
    Diagnostics  owned_route_;
    /// The active route: this backend's own until an engine installs its.
    Diagnostics* route_ = &owned_route_;
};

VN_GRAPHICS_NS_END
