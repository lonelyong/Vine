#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The SDK-facing backend: the rewrite's session and frame drive behind `vn::graphics::RenderBackend`.
 *
 * WHERE THIS SITS (design §2.2). Every other `api/` type is one piece a frame is made of - the session, the
 * store, the assembly, the executor - and this is the seam the ENGINE talks to: it implements the SDK's
 * `RenderBackend` methods and drives those pieces through them, deciding nothing of its own. That is why it
 * is allowed to be thin: `initialize()` builds the session, a frame is `beginFrame()` -> `endFrame()` ->
 * `swapBuffers()` over the pieces that exist, and every call it cannot serve yet is REPORTED (the SDK's own
 * rule: report what could not be done instead of answering with nothing).
 *
 * WHAT IS SERVED TODAY (the facade's slices, §11.16bd-§11.16bh):
 *   * the session lifecycle - up, down, up again, `shutdown()` idempotent and safe before any
 *     `initialize()`, exactly what the SDK's contract demands of a backend whose surface is recreated;
 *   * the frame protocol - an EMPTY frame is compiled, recorded and presented (the session's own rule:
 *     opening a frame may acquire an image, and the only call that hands it back is the present), and a
 *     frame with passes goes the whole way: the pass protocol below collects the plan, the content world
 *     records it, and the session presents it;
 *   * the PASS PROTOCOL, which is where the drawing half starts: `beginPass()`/`endPass()` open and close
 *     a scope, `setPassOrder()`/`setRenderTarget(nullptr)`/`setViewport()`/`setClearPolicy()`/
 *     `setDepthMode()`/`setLights()` announce its state, and `render()` collects the commands. The calls
 *     are turned into the plan's own spelling (the SDK's clear policy into the plan's, the pass object
 *     into a `core::PassId` through api/PassRegistry) and handed to the collection stage; what the
 *     protocol REFUSES is the recorder's to judge and report, so this layer has no rules of its own.
 *     Calls that arrive OUTSIDE a frame - the engine's pre-frame warm-up, which executes every enabled
 *     non-clearing pass once so a backend can prepare its retained state - are inert by contract (see
 *     the SDK's note) except for the pass identity, which is registered: the warm-up's pass keeps its
 *     number in the frames that follow;
 *   * CONTENT DRAWING: `render()` tracks the commands' own objects (geometry, material, program - and
 *     the default program from `setDefaultContentProgram()`) into api/ContentStore and snapshots the call
 *     into the plan; `swapBuffers()` assembles the frame's content (api/ContentAssembly) and records it
 *     into the passes the plan asked for, with the view block built from the pass' own camera and the
 *     frame's clock - the host's picture, drawn by the pieces that already existed;
 *   * the OFF-SCREEN HALF: `supportsRenderTargets()` answers true, `setRenderTarget()` holds the host's
 *     targets in api/HostTargets (the description is COPIED, the objects are built lazily, and the resize /
 *     rebuild after that is the plan's answer applied by the executor), `setPassInputs()` announces what a
 *     pass reads - and the images of those inputs are what the content recording binds - and
 *     `drawScreenProgram()` records the full-screen call whose source is one of them. `releaseRenderTarget()`
 *     drops everything held for a target the host is about to destroy;
 *   * the READBACK: `readColorBuffer()`/`readDepthBuffer()` stop the device (a counted wait), submit the
 *     target's own copy commands ONCE and fence them, so the pixels / depths the host gets are the ones the
 *     last submitted frame left (api/HostReadback). A refusal answers the SDK's machine-readable result and
 *     says its reason once per episode;
 *   * the surface facts - `setWindowHandle()`/`nativeHandle()` for a host surface, and `resize()`: the
 *     surface OWNS its size (the SDK's authority order), so a live announcement makes the session FOLLOW the
 *     surface it is on (re-read it, rebuild the swapchain when it changed - one COUNTED device stop), while
 *     the announced numbers are what the next `initialize()` creates a window at;
 *   * the diagnostics route - the core's one route feeds the SDK's `reportDiagnostic`, so a host's sink and
 *     `diagnosticCount()` see the session's own reports without this layer re-reporting anything;
 *   * THE SWEEP - a frame's content is retained for as long as the host holds it, and what the host has let go
 *     is let go ONCE PER FRAME, in the one call that owns the order (api/ContentSweep: after the frame's
 *     content has been recorded, before the frame's parks are advanced). Before that unit existed both halves
 *     existed and were called by nothing, so a dropped geometry kept its tables, its halves and its pipelines
 *     alive until the session ended; `releasedContentObjects()` / `releasedTextures()` / `releasedStreams()`
 *     are the counters that
 *     keep the sweep visible from outside.
 *
 * WHAT IS REPORTED AS NOT SERVED YET: two calls that have nowhere to go - a frame asked for with no session,
 * and a draw before the content world is up. Each says so once per episode (core::ReportOnce); a facade that
 * silently dropped them would look like a working backend with a black screen.
 *
 * ONE THREAD, LIKE EVERYTHING ELSE HERE: the engine drives a backend from one thread and never reentrantly
 * (see RenderBackend's class note), so this type keeps no synchronisation.
 */
VN_VSG_NS_BEGIN

namespace detail
{
class BackendContentAccess;
}  // namespace detail

/** @brief The SDK's render backend, driven by the rewrite's session (see the file note). */
class VN_VSG_API VsgBackend : public vn::graphics::RenderBackend
{
  public:
    VsgBackend();
    ~VsgBackend() override;

    VsgBackend(const VsgBackend&)            = delete;
    VsgBackend& operator=(const VsgBackend&) = delete;

  public:
    /** @brief Brings the session up from what was announced (window handle, size, default program).
     *
     * @return true when the session is ready to render; false when it is not, with the reason reported on
     *         the diagnostics channel (the session reports it, and this layer forwards).
     */
    bool initialize() override;

    /** @brief Tears the session down: idempotent, and initializable again (the SDK's contract). */
    void shutdown() override;

    /** @brief Opens the frame on the session; the frame's plan starts empty. */
    void beginFrame() override;

    /** @brief Closes the frame: the plan is closed and compiled against what the frame draws into. */
    void endFrame() override;

    /** @brief Records the frame and commits it through the session: the one present. */
    void swapBuffers() override;

    /** @brief Remembers the host's window handle for the next @ref initialize. */
    void setWindowHandle(void* native_handle) override;

    /** @brief Gets the handle the session adopted, or null while it owns its window (see the file note). */
    [[nodiscard]] void* nativeHandle() const override;

    /** @brief Announces a surface size: applied by the next @ref initialize, followed live (see the file note). */
    void resize(int width, int height) override;

    /** @brief Remembers the default content program, tells the plan, and tracks it (see the file note). */
    void setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program) override;

    /** @brief Answers true: off-screen targets are held, drawn into and sampled (see setRenderTarget). */
    bool supportsRenderTargets() override;

    /** @brief Holds the host's target under its own identity; nullptr selects the default framebuffer. */
    void setRenderTarget(vn::raw_ptr<vn::graphics::RenderTarget> target) override;

    /** @brief Opens the pass scope, under the pass' own number (see the file note). */
    void beginPass(vn::raw_ptr<const vn::graphics::RenderPass> pass) override;

    /** @brief Closes the pass scope (the recorder drops what no draw consumed). */
    void endPass() override;

    /** @brief Announces the pass' stacking order. */
    void setPassOrder(int order) override;

    /** @brief Announces the sub-rectangle for the next drawing call of this scope. */
    void setViewport(int x, int y, int width, int height) override;

    /** @brief Announces the lights for the next drawing call of this scope. */
    void setLights(const std::vector<vn::raw_ptr<const vn::graphics::Light>>& lights) override;

    /** @brief Announces the pass' resolved inputs, in declaration order. */
    void setPassInputs(const std::vector<vn::raw_ptr<vn::graphics::RenderTarget>>& inputs) override;

    /** @brief Announces how this pass' content handles the target's current depth. */
    void setDepthMode(vn::graphics::DepthMode mode) override;

    /** @brief Announces the pass' clear policy (translated into the plan's spelling). */
    void setClearPolicy(const vn::graphics::ClearPolicy& policy) override;

    /** @brief Collects a content drawing call, and tracks the objects it names (see the file note). */
    void render(const std::vector<vn::graphics::RenderCommand>& commands,
                const vn::graphics::Camera*                      camera) override;

    /** @brief Records a full-screen call whose source is one of the pass' declared inputs. */
    void drawScreenProgram(vn::graphics::RenderTarget*                     source,
                           vn::raw_ptr<const vn::graphics::ShaderProgram> program,
                           vn::raw_ptr<const vn::graphics::Camera>        camera) override;

    /** @brief Forgets the pass' identity: the SDK's announcement that the pass is going away. */
    void releasePass(vn::raw_ptr<const vn::graphics::RenderPass> pass) override;

    /** @brief Drops everything held for a target the host is about to destroy. */
    void releaseRenderTarget(vn::graphics::RenderTarget* target) override;

    /** @brief Reads a colour attachment back: the device is stopped (counted), the copy is fenced, bytes out. */
    bool readColorBuffer(const vn::graphics::RenderTarget* target, int attachment,
                         std::vector<std::uint8_t>& outPixels, vn::graphics::ReadbackResult* why = nullptr) override;

    /** @brief Reads a depth attachment back as normalised values (a borrowed depth is the source's to read). */
    bool readDepthBuffer(const vn::graphics::RenderTarget* target, std::vector<float>& outDepths,
                         vn::graphics::ReadbackResult* why = nullptr) override;

  public:
    /** @brief Gets whether the session is up. */
    [[nodiscard]] bool initialized() const noexcept;

    /** @brief Gets how many frames the session has presented. */
    [[nodiscard]] std::uint64_t framesPresented() const noexcept;

    /** @brief Gets how many times this backend has waited the device idle. */
    [[nodiscard]] std::size_t deviceWaits() const noexcept;

    /** @brief Gets how many abandoned content objects the frame's sweeps have let go (see api/ContentSweep).
     *
     * ONE PER OBJECT whose last holder was the content store - a geometry, a program or a material the host
     * dropped. The counter is how "the sweep runs, and it only ever lets go of what nobody holds" stays
     * checkable from outside: a host that keeps everything it draws reads zero however long it runs, and a
     * host that drops something reads it move on the next frame.
     */
    [[nodiscard]] std::size_t releasedContentObjects() const noexcept;

    /** @brief Gets how many abandoned textures the frame's sweeps have let go (see api/ContentSweep). */
    [[nodiscard]] std::size_t releasedTextures() const noexcept;

    /** @brief Gets how many shared streams the frame's steps have let go because no frame named them any more.
     *
     * The observable behind api/StreamUploads's lifetime rule: a geometry the host stops drawing is not
     * released any differently from one it keeps - what ends is its streams being NAMED, and the window is the
     * executor's own (`slots + 1`). A host that keeps drawing everything reads zero; one that drops a mesh
     * reads it move once the window has passed. It is deliberately NOT part of `releasedContentObjects()`:
     * that one counts objects the host let go of, this one counts uploads no frame asks for any more.
     */
    [[nodiscard]] std::size_t releasedStreams() const noexcept;

  private:
    /** @brief Reports one unserved entry point, once per episode (see the file note). */
    void reportUnserved(std::size_t slot) noexcept;

    /** @brief Runs this frame's sweep: what the host let go is let go (see api/ContentSweep).
     *
     * Called once per frame, after the frame's content has been recorded and before the session commits it -
     * the commit advances the retirement queue, and a park made after that advance would be released a frame
     * early (the ordering rule core::RetirementQueue documents).
     */
    void sweepAbandonedContent();

    /** @brief Releases everything that belongs to the SESSION'S DEVICE (the content world and the targets).
     *
     * ONE SPELLING FOR ONE ORDER, and it has to run BEFORE a new session is built: the pieces this drops own
     * GPU objects of the device the session is about to replace, so keeping them alive across a re-initialize
     * means two `vsg::Device`s at once - which vsg refuses by design (VSG_MAX_DEVICES, deliberately 1 in this
     * plugin's CMakeLists: a path that does this must throw instead of quietly working). `shutdown()` and
     * @ref initialize both need it, for the same reason and in the same order.
     */
    void releaseContentWorld() noexcept;

  private:
    friend class detail::BackendContentAccess;

    struct Data;
    std::unique_ptr<Data> d;
};

VN_VSG_NS_END
