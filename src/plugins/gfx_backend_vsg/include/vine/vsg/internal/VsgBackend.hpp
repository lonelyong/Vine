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
 * is allowed to be thin: `initialize()` builds the session, a frame is `executeFrame(value, target)` ->
 * `present()` over the pieces that exist, and every call it cannot serve yet is REPORTED (the SDK's own
 * rule: report what could not be done instead of answering with nothing).
 *
 * WHAT IS SERVED TODAY (the facade's slices, §11.16bd-§11.16bh):
 *   * the session lifecycle - up, down, up again, `shutdown()` idempotent and safe before any
 *     `initialize()`, exactly what the SDK's contract demands of a backend whose surface is recreated;
 *   * THE FRAME, RUN FROM ITS VALUE - the one frame entry the SDK has (RenderFrameExecutor::executeFrame): the frame's
 *     books are opened (the storage a frame needs is brought up to what it takes, the session mints the token and
 *     acquires the image, the collection stage opens the plan for that token), every pass of the value is walked
 *     in the order it states - identity through api/PassRegistry, then the facts and the draws it carries, each
 *     turned into the plan's own spelling (the SDK's clear policy into the plan's) - and the books are closed
 *     (the plan compiles against the frame's target facts). An EMPTY frame is still a frame: it is compiled,
 *     recorded and presented (the session's own rule: opening a frame may acquire an image, and the only call
 *     that hands it back is the present), and the surface's `present()` presents what the frame left;
 *   * CONTENT DRAWING: the value's drawing calls track the commands' own objects (geometry, material, program -
 *     and the default program from `setDefaultContentProgram()`) into api/ContentStore and snapshot the call
 *     into the plan; `present()` assembles the frame's content (api/ContentAssembly) and records it
 *     into the passes the plan asked for, with the view block built from the pass' own camera and the
 *     frame's clock - the host's picture, drawn by the pieces that already existed;
 *   * the OFF-SCREEN HALF: `capabilities()` answers `render_targets = true`, `setRenderTarget()` holds the host's
 *     targets in api/HostTargets (the description is COPIED, the objects are built lazily, and the resize /
 *     rebuild after that is the plan's answer applied by the executor), a pass' declared inputs (the value's
 *     `inputs`) announce what it reads - and the images of those inputs are what the content recording binds -
 *     and a draw of `Kind::Screen` is the full-screen call whose source is one of them. `releaseRenderTarget()`
 *     drops everything held for a target the host is about to destroy;
 *   * the READBACK: `readColorBuffer()`/`readDepthBuffer()` stop the device (a counted wait), submit the
 *     target's own copy commands ONCE and fence them, so the pixels / depths the host gets are the ones the
 *     last submitted frame left (api/HostReadback). A refusal answers the SDK's machine-readable result and
 *     says its reason once per episode;
 *   * the surface facts - `setWindowHandle()`/`nativeHandle()` for a host surface, `resize()` and the present:
 *     THE DEVICE IS NOT A WINDOW (see `.ai/design/device-is-not-a-window.md`), so what this backend hands out from
 *     `createSurfaceTarget()` is one target OBJECT per window, the first included - never itself (a device that draws
 *     only into render targets answers none at all, see RenderDevice::createSurfaceTarget). The same window asked for
 *     twice is refused with `WindowOccupied` and said once, because one live swapchain per window is a platform
 *     constraint, while a different window is served. The surface OWNS its size (the SDK's authority order), so a
 *     live announcement makes the session FOLLOW the surface it is on (re-read it, rebuild the swapchain when it
 *     changed - one COUNTED device stop), while the announced numbers are what the next `initialize()` creates a
 *     window at;
 *   * THE OFF-SCREEN FRAME (no window at all): `executeFrame(frame, nullptr)` is the off-screen form, not "window
 *     0" - it runs through api::OffscreenSession, which has no surface, acquires nothing and presents nothing, and
 *     the frame is submitted by the time the call returns (there is nobody to hand its tail to);
 *   * the diagnostics route - this layer's own route feeds the SDK's ACTIVE one (`reportDiagnostic`), so a
 *     host's sink and its single `diagnosticCount()` see the session's own reports without this layer
 *     re-reporting anything;
 *   * THE SWEEP - a frame's content is retained for as long as the host holds it, and what the host has let go
 *     is let go ONCE PER FRAME, in the one call that owns the order (api/ContentSweep: after the frame's
 *     content has been recorded, before the frame's parks are advanced). Before that unit existed both halves
 *     existed and were called by nothing, so a dropped geometry kept its tables, its halves and its pipelines
 *     alive until the session ended; `releasedContentObjects()` / `releasedTextures()` / `releasedStreams()`
 *     are the counters that
 *     keep the sweep visible from outside.
 *
 * WHAT IS REPORTED AS NOT SERVED YET: two calls that have nowhere to go - a frame asked for with no session,
 * and a draw before the content world is up. Each says so once per episode (vn::graphics::ReportOnce); a facade that
 * silently dropped them would look like a working backend with a black screen.
 *
 * ONE THREAD, LIKE EVERYTHING ELSE HERE: the engine drives a backend from one thread and never reentrantly
 * (see RenderBackend's class note), so this type keeps no synchronisation.
 */
VN_VSG_NS_BEGIN

class BlockStorage;

namespace api
{
class OffscreenSession;
class Session;
}  // namespace api

namespace detail
{
class BackendContentAccess;
}  // namespace detail

/** @brief The SDK's render device, driven by the rewrite's session (see the file note). */
class VN_VSG_API VsgBackend : public vn::graphics::RenderBackend
{
  public:
    /** @brief Creates the device @p desc asks for (see DeviceDesc and RenderDevice::capabilities).
     *
     * @details
     * - `Probe` (the default) is the window-owning shape: the device brings up a window of its own when the host
     *   names none, and hands out one target per window the host asks for.
     * - `None` is a device that presents nothing: `initialize()` creates its device through the WINDOWLESS seam (no
     *   window is ever created), `capabilities().presentation` answers false, and `createSurfaceTarget` answers
     *   NoPresentation silently. Its frames are the ones that name no target.
     * - `Handle` is the same device made for the host's window, named as the creation input.
     *
     * @param desc How this device reaches the display (see vn::graphics::DeviceDesc).
     */
    explicit VsgBackend(const vn::graphics::DeviceDesc& desc = {});
    ~VsgBackend() override;

    VsgBackend(const VsgBackend&)            = delete;
    VsgBackend& operator=(const VsgBackend&) = delete;

  public:
    /** @brief Brings the session up from what was announced (window handle, size, default program).
     *
     * @details
     * - A device created with `DevicePresentation::None` creates its DEVICE here and no window: the windowless seam
     *   (`vine/vsg/internal/Device.hpp`) is the door that needs no surface, so such a device never owns a window.
     * - A device created with `DevicePresentation::Handle` without a handle is refused here with one InitFailed
     *   report rather than quietly becoming a window of its own.
     *
     * @return true when the session is ready to render; false when it is not, with the reason reported on
     *         the diagnostics channel (the session reports it, and this layer forwards).
     */
    bool initialize() override;

    /** @brief Tears the session down: idempotent, and initializable again (the SDK's contract). */
    void shutdown() override;

    /** @brief Answers this object: this device's frame half is its own (see RenderDevice::executor). */
    [[nodiscard]] vn::graphics::RenderFrameExecutor& executor() override;

    /** @brief Hands out the target of ONE WINDOW this device will serve (see WindowPresentTarget).
     *
     * ONE DEVICE, SEVERAL WINDOWS: a window carries one live swapchain (a platform constraint), so a window that
     * is already being served is refused with WindowOccupied and said once - and a NEW window gets its own target,
     * because the device is the thing that is shared, not the window. EVERY window is answered by its own small
     * target object, the first included: the backend is the device and the frame half, and it is never a window
     * (see `.ai/design/device-is-not-a-window.md`).
     *
     * What a window brings is a SURFACE and a SWAPCHAIN; the device, its compiled programs and the buffers they
     * read are shared with the windows already served - which is the whole point of asking ONE device for them.
     *
     * A device created to present nothing (`DevicePresentation::None`) answers nullptr with NoPresentation and NO
     * report: it has no window half at all, and that is a state rather than a failure.
     *
     * @param native_handle Host window to present through, or nullptr when the host names none (this device then
     *                      brings up a window of its own - it owns at most ONE of those, because a window of ours
     *                      has no other reason to exist).
     * @param why           Receives why no target was handed out (Ok when one was), or null to ignore.
     * @return The target of that window, or nullptr (see @p why).
     */
    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget>
    createSurfaceTarget(void* native_handle, vn::graphics::SurfaceTargetResult* why = nullptr) override;

    /** @brief Gets how many windows this device serves (0 before the first ask). */
    [[nodiscard]] std::size_t windowCount() const noexcept;

    /** @brief Presents window @p index's last frame: what a target object this device handed out calls.
     *
     * @param index Window index (see createSurfaceTarget); an out-of-range one does nothing.
     */
    void presentWindow(std::size_t index);

    /** @brief Announces window @p index's surface size: what a target object calls (see PresentTarget::resize).
     *
     * @param index  Window index.
     * @param width  Announced width in pixels.
     * @param height Announced height in pixels.
     */
    void resizeWindow(std::size_t index, int width, int height);

    /** @brief Gets the handle window @p index is attached to, or nullptr (see WindowPresentTarget::nativeHandle). */
    [[nodiscard]] void* windowHandleAt(std::size_t index) const noexcept;

    /** @brief Re-announces window @p index's host handle (see WindowPresentTarget::setWindowHandle). */
    void setWindowHandleAt(std::size_t index, void* native_handle);

    /** @brief Runs a frame from its value: the plugin's own translation of the frame the engine compiled.
     *
     * The engine hands over the whole frame (see vn::graphics::FrameDescription), so the plugin no longer learns
     * the frame from a call sequence it has to keep in order - it walks the value and applies each entry to its own
     * pieces (see the file note): the frame's books open (storage, token, acquisition, the plan's frame), every
     * pass is collected in the order the value states, and the books close (the plan compiles against what the
     * frame draws into). The frame's TAIL is here too: with a target the executor presents through it (which in this
     * backend commits and shows the frame), so a host that drove the frame does not present it again.
     *
     * The TARGET is what the frame's "default framebuffer" row resolves to (see
     * vn::graphics::RenderFrameExecutor::executeFrame): this backend's own window half is the only one it can
     * serve, so a frame that names another target is refused and reported ONCE (nothing was recorded), while
     * nullptr is a frame with NO WINDOW - the off-screen form, which runs through this device's off-screen session,
     * submits what its passes recorded into their own targets, and presents nothing.
     *
     * A frame with no session is reported once per episode and runs nothing (see the file note).
     *
     * @param frame      The frame to run, in run order.
     * @param present_to Target the frame's default framebuffer resolves to, or nullptr to present nowhere.
     */
    virtual void executeFrame(const vn::graphics::FrameDescription& frame,
                              vn::graphics::PresentTarget*           present_to = nullptr) override;

    /** @brief Answers what this backend can do: off-screen targets, and presentation unless created to present nothing.
     *
     * @details
     * - `render_targets` is always true: the held-target and readback half needs no window.
     * - `presentation` answers the CREATION INPUT (see DeviceDesc): false for a device built with
     *   `DevicePresentation::None`, which has no window half for its whole life.
     *
     * @return The capability value the engine reads (see `RenderDevice::capabilities`).
     */
    [[nodiscard]] vn::graphics::Capabilities capabilities() const noexcept override;

    /** @brief Answers the ONE report: frame counters, retention, and the session's frame clock (see the file note). */
    [[nodiscard]] vn::graphics::FrameReport frameReport() const noexcept override;

    /** @brief Remembers the default content program, tells the plan, and tracks it (see the file note). */
    void setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program) override;

    /** @brief Holds the host's target and builds what its description can make; nullptr needs no announcement.
     *
     * Which pass draws into which target travels inside the frame value, so this is the host's own announcement:
     * the target it is about to draw into or read back, configured before any frame names it (see the file note).
     */
    void setRenderTarget(vn::raw_ptr<vn::graphics::RenderTarget> target) override;

    /** @brief Forgets the pass' identity: the SDK's announcement that the pass is going away. */
    void releasePass(vn::raw_ptr<const vn::graphics::RenderPass> pass) override;

    /** @brief Drops everything held for a target the host is about to destroy. */
    void releaseRenderTarget(vn::graphics::RenderTarget* target) override;

    /** @brief Releases the GPU residency of one content object the host still owns. */
    bool releaseContent(vn::raw_ptr<const vn::Object> content) override;

    /** @brief Reads a colour attachment back: the device is stopped (counted), the copy is fenced, bytes out. */
    bool readColorBuffer(const vn::graphics::RenderTarget* target, int attachment,
                         std::vector<std::uint8_t>& outPixels, vn::graphics::ReadbackResult* why = nullptr) override;

    /** @brief Reads a depth attachment back as normalised values (a borrowed depth is the source's to read). */
    bool readDepthBuffer(const vn::graphics::RenderTarget* target, std::vector<float>& outDepths,
                         vn::graphics::ReadbackResult* why = nullptr) override;

  public:
    /** @brief Gets whether the session is up: the device contract's own answer (see RenderDevice::initialized).
     *
     * @details
     * - A device created to present nothing (`DevicePresentation::None`) has no session to ask, so its answer is "the
     *   device came up" - which is the only thing a window would have added there.
     *
     * @return true when any window can draw, or - for a display-less device - when its device exists.
     */
    [[nodiscard]] bool initialized() const noexcept override;

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
    /** @brief Opens the frame's books: storage, token, acquisition, and the plan's own frame.
     *
     * The first half of what used to be `beginFrame()` (the SDK's frame is one call now - see
     * RenderFrameExecutor::executeFrame): the per-frame storage is brought up to what this frame needs HERE, between
     * frames, where no recording is in progress and the previous frame's bytes can be left behind safely, and
     * then the frame's token is minted and the session's clock advanced - the recorder opens the plan for
     * the same token, so the description and the session's frame cannot disagree about which frame it is.
     *
     * WHICH session opens it is the frame's own answer: the window a target names, or the off-screen session when the
     * frame names none (see executeFrame).
     */
    void openFrameBooks();

    /** @brief Closes the frame's books: the plan is closed and compiled against what the frame draws into.
     *
     * The second half of the old `endFrame()`: the collection stage's frame closes and swaps its buffers, and the
     * plan is compiled against the facts the frame is answered for - the window as the default framebuffer (from
     * the target itself) plus one row per host target this backend holds, built or not, because the plan has to be
     * able to say "this one has no objects yet" and because the compiler resolves a pass' declared INPUTS from
     * that same table (see HostTargets::facts).
     */
    void closeFrameBooks();

    /** @brief Records and commits the frame whose books are closed, through the ONE session that owns it.
     *
     * @details
     * - The window path and the off-screen path differ in three facts and nothing else: which session mints the
     *   frame (a window's, or the off-screen one), which target the frame's default-framebuffer row resolves to
     *   (the window's, or none at all), and whether the commit presents (a window's does, an off-screen one never).
     *   Everything between - applying the plan's target answers, recording the content, the sweep, handing the graph
     *   to the session, the commit - is one sequence, so it is one function.
     * - A frame with no passes still commits: a window's opens an image that has to be handed back, and the
     *   off-screen session's graph is a valid empty submission.
     *
     * @param window    The window's session that owns the frame, or null when the frame names no window.
     * @param offscreen The off-screen session that owns the frame, or null when the frame belongs to a window
     *                  (exactly one of the two is non-null).
     */
    void commitFrameThrough(api::Session* window, api::OffscreenSession* offscreen);

    /** @brief Opens a pass' scope under the pass' own number (see api/PassRegistry). */
    void openPassScope(vn::raw_ptr<const vn::graphics::RenderPass> pass);

    /** @brief Closes the pass scope (the recorder drops what no draw consumed). */
    void closePassScope();

    /** @brief Announces the pass' stacking order (see FrameDescription::Pass::order). */
    void announcePassOrder(int order);

    /** @brief Announces the pass' resolved inputs, in declaration order (see FrameDescription::Pass::inputs). */
    void announcePassInputs(const std::vector<vn::graphics::ImageSource>& inputs);

    /** @brief Announces the target a pass draws into, and reports it when it cannot be built at all.
     *
     * null means the default framebuffer, which the plan spells the same way. A non-null target goes through the
     * host-target half (held, built as far as its description allows) and, INSIDE a frame where a pass draws into
     * it, a description that cannot become objects is REPORTED once per episode - a picture drawn where the host
     * never asked for it is worse than one that did not arrive (see HostTargets).
     */
    void announceTarget(vn::raw_ptr<vn::graphics::RenderTarget> target);

    /** @brief Announces the pass' sub-rectangle (see PassDraw::viewport). */
    void announceViewport(const vn::graphics::Viewport& viewport);

    /** @brief Announces the pass' clear policy, translated into the plan's spelling (see PassDraw::clear). */
    void announceClear(const vn::graphics::ClearPolicy& policy);

    /** @brief Announces the pass' own state for the content it draws (see PassDraw::state). */
    void announcePassState(const vn::graphics::PassRenderState& state);

    /** @brief Announces the lights of one drawing call (see DrawDescription::lights). */
    void announceLights(const std::vector<vn::raw_ptr<const vn::graphics::Light>>& lights);

    /** @brief Collects one content drawing call, and tracks the objects it names (see the file note).
     *
     * @param commands         The pass' collected commands (shared and immutable - never rewritten here).
     * @param camera           The camera the draw is projected with.
     * @param program_override The pass' program override, applied to every command when the draw is recorded
     *                         (see PassDraw::program_override), or null for each command's own program.
     */
    void collectContentDraw(const std::vector<vn::graphics::RenderCommand>& commands,
                            const vn::graphics::Camera*                      camera,
                            const vn::graphics::ShaderProgram*               program_override);

    /** @brief Creates the slot of window @p index when it does not exist yet (indices are dense and never reused).
     *
     * @param index Window index (0 is the window the device answers for itself).
     */
    void ensureWindowAt(std::size_t index);

    /** @brief Gets window @p index's session, creating the window first when it does not exist yet.
     *
     * @param index Window index.
     * @return The session (never null; see core::DeviceClock for what it shares when it is not alone).
     */
    [[nodiscard]] api::Session& sessionAt(std::size_t index);

    /** @brief Brings up (lazily) the session a frame naming NO window runs through, or answers null.
     *
     * THE OFF-SCREEN SESSION IS THE DEVICE'S, not a window's: it borrows this backend's device and its graphics queue
     * family, reports into the DEVICE's clock and parks into the DEVICE's queue, and presents nothing. It is built on
     * the first frame that names no target, because a device that never runs an off-screen frame should not pay for
     * one.
     *
     * @return The session, or null when this backend has no device to run frames on (the reason is reported by the
     *         caller as the unserved frame it is).
     */
    [[nodiscard]] api::OffscreenSession* ensureOffscreenSession();

    /** @brief Collects one full-screen drawing call whose source is one of the pass' declared inputs. */
    void collectScreenDraw(vn::graphics::RenderTarget*                     source,
                           vn::raw_ptr<const vn::graphics::ShaderProgram> program,
                           vn::raw_ptr<const vn::graphics::Camera>        camera);

    /** @brief Holds the host's target and builds what its description can make (the half setRenderTarget shares).
     *
     * @param target          The host's target (never null).
     * @param picture_depends Whether a pass of the frame being run draws into it: a description that cannot become
     *                        objects at all is REPORTED then (inside a frame, where it blocks the picture) and is
     *                        silent otherwise, because a host configures a target before it draws into it and a
     *                        borrow whose lender arrives later in that setup is order rather than failure.
     * @return true when the target is held (see HostTargets::State for what its objects are).
     */
    bool adoptHostTarget(vn::raw_ptr<vn::graphics::RenderTarget> target, bool picture_depends);

    /** @brief Reports why @p target could not be built, once per episode (its lender missing picks the sentence).
     *
     * @param target               The target the host announced (named in the sentence).
     * @param depth_source_missing Whether the reason is a borrowed depth whose lender this backend does not hold.
     */
    /// Why a target's description could not become objects (see reportUnbuildableTarget).
    enum class UnbuildableReason : std::uint8_t
    {
        BuildFailed,        ///< The objects could not be created.
        DepthSourceMissing, ///< The depth is borrowed from a target this backend does not hold.
        SamplesUnsupported, ///< The sample count is above the device's, or a multisampled target borrows a depth.
    };

    void reportUnbuildableTarget(vn::raw_ptr<vn::graphics::RenderTarget> target, UnbuildableReason reason,
                                 std::uint32_t max_samples);

    /** @brief Replaces the frame's block storage when a frame outgrew it or the learned in-flight count did.
     *
     * Two requests ask for it - a frame that ran out of budget (see BlockStorage's file note) and rings too
     * shallow for the count the session LEARNED (see BlockStorage::layoutForInFlight) - and they are answered
     * together, in ONE replacement: a replacement starts with no growth request of its own, so answering
     * either half alone would silently drop the other.
     * BETWEEN frames, never inside one: the replacement is a NEW buffer (its bytes are laid out the same way,
     * so only the sets' elements move - see ContentAssembly::repoint), and the storage it replaces is parked
     * through the retirement queue for the window a submitted command buffer may still name its bytes. A
     * replacement that cannot be built leaves the old storage serving (and counting): its refusals were
     * already reported per pass, so there is nothing new to say.
     */
    void growBlockStorageIfNeeded();

    /** @brief Replaces the block storage when the live material set is larger than the arena it holds.
     *
     * Runs BEFORE the frame's content is recorded (see commitFrameThrough): the requirement is the number of
     * material rows the store answers for, every one of which keeps a slot until the host releases it. Growing
     * here means the frame never loses a draw to a full arena, and the replacement moves no recorded bytes - the
     * same rule growBlockStorageIfNeeded follows, one step later in the frame.
     */
    void growMaterialArenaIfNeeded();

    /** @brief Lets the drive use @p storage from now on, parking the one it replaces (the growth step).
     *
     * @param storage The replacement (null leaves the current storage in place and answers false).
     * @return true when the drive adopted it.
     */
    bool adoptBlockStorage(std::shared_ptr<BlockStorage> storage);

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
     * means holding objects of a dying device (two `vsg::Device`s at once are no longer fatal - the cap is 4,
     * see this plugin's CMakeLists - but the ordering is the contract, and it did not change with it).
     * `shutdown()` and @ref initialize both need it, for the same reason and in the same order.
     */
    void releaseContentWorld() noexcept;

  private:
    friend class detail::BackendContentAccess;

    struct Data;
    std::unique_ptr<Data> d;
};

VN_VSG_NS_END
