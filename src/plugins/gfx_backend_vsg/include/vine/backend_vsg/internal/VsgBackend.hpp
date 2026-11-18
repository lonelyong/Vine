#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/backend_vsg/backend_vsg_global.hpp>

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
 *     targets in api/TargetTable (the description is COPIED, the objects are built lazily, and the resize /
 *     rebuild after that is the plan's answer applied by the executor), a pass' declared inputs (the value's
 *     `inputs`) announce what it reads - and the images of those inputs are what the content recording binds -
 *     and a draw of `Kind::Screen` is the full-screen call whose source is one of them. `releaseRenderTarget()`
 *     drops everything held for a target the host is about to destroy;
 *   * the READBACK: `readColorBuffer()`/`readDepthBuffer()` stop the device (a counted wait), submit the
 *     target's own copy commands ONCE and fence them, so the pixels / depths the host gets are the ones the
 *     last submitted frame left (api/HostReadback). A refusal answers the SDK's machine-readable result and
 *     says its reason once per episode;
 *   * the surface facts - `setWindowHandle()`/`nativeHandle()` for a host surface, `resize()` and the present:
 *     THE DEVICE IS NOT A WINDOW (see `.ai/design/graphics-architecture.md`), so what this backend hands out from
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
 *     is let go ONCE PER FRAME: the streams and the textures ask their residency question from the frame's own
 *     tables (inside api/ContentAssembly::beginFrame), and the store MARKS what the host let go after the frame's
 *     content has been recorded and before the commit advances the queue - its rows then leave by epoch at the
 *     next frame boundary, with no park of its own. `releasedContentObjects()` / `releasedTextures()` /
 *     `releasedStreams()` are the counters that keep the sweep visible from outside.
 *
 * WHAT IS REPORTED AS NOT SERVED YET: two calls that have nowhere to go - a frame asked for with no session,
 * and a draw before the content world is up. Each says so once per episode (vn::graphics::ReportOnce); a facade that
 * silently dropped them would look like a working backend with a black screen.
 *
 * ONE SESSION IS ONE THREAD'S, AND TWO DRIVERS MAY RUN AT ONCE (`.ai/design/graphics-sessions.md` S8c). The device
 * gate has two sides (see `vine/graphics/FrameGate.hpp`): a frame body takes it SHARED, so two engines'
 * frames overlap, and a host verb that reconfigures the device or reads a device-wide answer takes it EXCLUSIVE.
 * What that leaves is TWO serial
 * sections, each with its own lock:
 *   * THE SAME WINDOW is driven by one frame at a time (a window owns one swapchain and one session); a second
 *     frame naming it waits for the first to end.
 *   * THE DEVICE'S CONTENT is touched by one CONTENT PHASE at a time (the backend's content lock): the frame head
 *     (passes, targets, content tracking, the tables, the storage's growth) and the phase that assembles, records
 *     and commits it. The COLLECTION and PLAN COMPILATION in between run on the driver's own scratch and take no
 *     device lock - that is where two frames genuinely overlap today; making the record walk lock-free too is the
 *     remaining half of S8c (`S8c-2`), and `C3`'s "record path takes no device lock" is NOT yet true for this
 *     backend until it lands.
 * A sink or a hook that calls back into the device from inside a frame gets a REFUSAL, never a wait (the readback
 * answers `Failed`, a content verb answers false / drops with one report): waiting there would be the thread waiting
 * for itself.
 */
VN_BACKEND_VSG_NS_BEGIN

class BlockStorage;
class FrameScratch;

namespace api
{

class OffscreenSession;
class WindowSession;

} // namespace api

namespace detail
{

class BackendContentAccess;

} // namespace detail

class VsgDriver;
class VsgExecutor;

/** @brief The SDK's render device, driven by the rewrite's session (see the file note). */
class VN_BACKEND_VSG_API VsgBackend : public vn::graphics::RenderBackend {
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
     *   (`vine/backend_vsg/internal/Device.hpp`) is the door that needs no surface, so such a device never owns a window.
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
     * (see `.ai/design/graphics-architecture.md`).
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
    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget> createSurfaceTarget(void*                              native_handle,
                                                                                     vn::graphics::SurfaceTargetResult* why = nullptr) override;

    /** @brief Gets how many windows this device serves (0 before the first ask). */
    [[nodiscard]] std::size_t windowCount() const noexcept;

    /** @brief Presents window @p index's last frame: what a target object this device handed out calls.
     *
     * @param index Window index (see createSurfaceTarget); an out-of-range one does nothing.
     */
    vn::graphics::PresentOutcome presentWindow(std::size_t index);

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
    virtual vn::graphics::PresentOutcome executeFrame(const vn::graphics::FrameDescription& frame, vn::graphics::PresentTarget* present_to = nullptr) override;

    /** @brief Prepares a frame's CONTENT without running it: the same value executeFrame() walks, stopped earlier.
     *
     * @details
     * - WHAT IT DOES. The value is collected and compiled exactly as executeFrame() does it - the plan, its target
     *   facts and its pass scopes come out identical - and then the VALUE side of the frame is built and the recorded
     *   node is DROPPED: the compiled halves (`api/CompiledPrograms`), the declared sets (`api/BindingSets`), the
     *   sampled-input sets, the stream uploads (`api/StreamUploads`) and every pipeline identity the frame would
     *   acquire (`ContentPipeline::acquire`, which is what moves the variant pool). A frame recorded afterwards finds
     *   all of it and builds nothing (`R23`), which is the criterion `T33` pins.
     * - WHAT IT NEVER DOES, and this is why the entry exists at all: no session frame is opened. A windowed session's
     *   `beginFrame` ACQUIRES the next swapchain image, and an image acquired and never presented starves the next
     *   frame (`P11`) - so the plan is compiled against the target facts the target itself answers (a const question),
     *   and the block storage's own frame opens and closes around the recording without any of that. Nothing is
     *   submitted, nothing is presented, and the frame-progress watermarks do not move.
     * - WHAT IT LEAVES BEHIND is exactly what a recorded frame would have built, under the same keys: the recorded
     *   NODE is thrown away (`d->packets` is cleared), because a command graph is the frame's, not a prepared value's.
     *
     * @param frame      The frame to prepare, in execution order (see vn::graphics::FrameDescription).
     * @param present_to Target the frame's default framebuffer resolves to, or nullptr for the off-screen form. It is
     *                   read, never acquired from.
     * @return What the call built (see vn::graphics::PrepareReport); `Unsupported` is never answered by this backend.
     */
    [[nodiscard]] vn::graphics::PrepareReport prepareFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to = nullptr) override;

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

    /** @brief Hands out a new FRAME DRIVER: its own executor state, the device's content and target registry.
     *
     * @details
     * - WHAT A DRIVER OWNS (S8b): the frame state that is one frame's - the window the frame drives, the passes it
     *   recorded, its measurement entries, its skip count - and the default content program the engine declared. What
     *   it BORROWS is the device: its content world, its target registry, its clock and park queue.
     * - The device owns what it hands out (see RenderDevice::createSession): the pointer is valid until
     *   releaseSession() or shutdown().
     *
     * @return The driver; never null (the device's own driver is `&executor()`).
     */
    [[nodiscard]] vn::graphics::RenderFrameExecutor* createSession() override;

    /** @brief Returns a driver handed out by @ref createSession (the device's own is a no-op, per the SDK). */
    void releaseSession(vn::graphics::RenderFrameExecutor& session) override;

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

    /** @brief Releases the stream uploads read from one buffer; the geometry that reads it stays.
     *
     * @details
     * - A stream's residency key IS its buffer (see api/StreamUploads), so this is the one entry that can name it:
     *   the geometry's rows and tables are content and stay, and only the GPU copy goes - the next frame that draws
     *   the geometry uploads again.
     * - Parked through the device's retirement queue, never stopped synchronously; a session that has not learned
     *   its in-flight count yet keeps the entries (counted as retained) rather than freeing bytes a frame may name.
     *
     * @param buffer Vertex or instance stream buffer.
     * @return true when the release was accepted (scheduled, or nothing was resident); false when the session is
     *         not up.
     */
    bool releaseContent(const vn::Buffer<float>& buffer) override;

    /** @brief Releases the index upload read from one buffer (see the float overload).
     *
     * @param buffer Index buffer.
     * @return true when the release was accepted (see the float overload).
     */
    bool releaseContent(const vn::Buffer<std::uint32_t>& buffer) override;

    /** @brief Reads a colour image back: the device is stopped (counted), the copy is fenced, bytes out. */
    vn::graphics::ReadbackOutcome readColorBuffer(const vn::graphics::ImageRef& image, std::vector<std::uint8_t>& outPixels) override;

    /** @brief Reads a depth image back as normalised values (a borrowed depth is the source's to read). */
    vn::graphics::ReadbackOutcome readDepthBuffer(const vn::graphics::ImageRef& image, std::vector<float>& outDepths) override;

    using vn::graphics::RenderDevice::readColorBuffer;
    using vn::graphics::RenderDevice::readDepthBuffer;

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

    /** @brief Gets how many abandoned content objects the frame's sweep has let go (see ContentStore::releaseAbandoned).
     *
     * ONE PER OBJECT whose last holder was the content store - a geometry, a program or a material the host
     * dropped. The counter is how "the sweep runs, and it only ever lets go of what nobody holds" stays
     * checkable from outside: a host that keeps everything it draws reads zero however long it runs, and a
     * host that drops something reads it move on the next frame.
     */
    [[nodiscard]] std::size_t releasedContentObjects() const noexcept;

    /** @brief Gets how many texture uploads left because no live content declares them (see MaterialImages::released).
     *
     * The observable behind the image cache's pin rule: an entry lives while a material row names its texture or a
     * pass imports it, so a culled map keeps its upload and a dropped one leaves once its row is gone.
     */
    [[nodiscard]] std::size_t releasedTextures() const noexcept;

    /** @brief Gets how many shared streams the frame's steps have let go because no live content declares them.
     *
     * The observable behind api/StreamUploads's lifetime rule: an entry lives while a tracked geometry row names
     * its buffer. A geometry that is merely culled keeps its uploads (the row is still there); one that leaves the
     * tree loses them once the content store has parked the row's removal for the executor's own window
     * (`slots + 1`). A host that keeps drawing everything reads zero; one that drops a mesh reads it move once the
     * window has passed. It is deliberately NOT part of `releasedContentObjects()`: that one counts objects the
     * host let go of, this one counts uploads no content declares any more.
     */
    [[nodiscard]] std::size_t releasedStreams() const noexcept;

    /** @brief Gets how much host memory this device is holding as upload staging, across every live session.
     *
     * @details
     * - The quantity the frame boundary gives back, and the one the host sees as `SharedUsage` in the GPU counters
     *   (measured: 1.2 GB after loading a 704 MB model, before this backend stopped keeping it). It is 0 in the steady
     *   state - an upload's scratch is freed at the first frame whose boundary finds its copy done - and rises only while
     *   a copy is actually in flight.
     * - Read between frames, like everything else here: it sums each session's own task.
     *
     * @return Staged bytes held right now; zero when nothing is staged.
     */
    [[nodiscard]] std::size_t uploadStagingBytes() const noexcept;

  private:
    /** @brief Frees every session's upload staging whose copy has finished, and says whether anything went back.
     *
     * @details
     * - WHAT KEEPS UPLOAD MEMORY FROM ACCUMULATING, and the only thing that does: it runs at the top of every frame, so
     *   a buffer is freed the first frame after the copy that read it signals its fence. A host asks for nothing.
     * - Call it between frames only - it frees memory a submitted copy may still be reading, under each block's own fence
     *   and nothing else, so nothing here waits and nothing is parked.
     *
     * @return true when at least one session gave bytes back.
     */
    bool releaseFinishedUploadStaging();

    /** @brief Drops the frame in flight's plan and tables on every session's scratch (see FrameScratch).
     *
     * @details
     * - WHAT IT IS FOR: a teardown or a re-initialize must not leave a plan that a later present could commit. The
     *   plans live in the sessions' scratches (a plan is one frame's), so clearing them is the device's one sweep
     *   over the sessions it holds - not a second copy of the frame state (`P8`).
     */
    void resetFrameScratch() noexcept;

    /** @brief Opens the frame's books: storage, token, acquisition, and the plan's own frame.
     *
     * The first half of what used to be `beginFrame()` (the SDK's frame is one call now - see
     * RenderFrameExecutor::executeFrame): the frame's token is minted and the session's clock advanced - the recorder
     * opens the plan for the same token, so the description and the session's frame cannot disagree about which frame
     * it is. It takes no device lock: the token and the acquisition are the SESSION's.
     *
     * WHICH session opens it is the frame's own answer: the window a target names, or the off-screen session when the
     * frame names none (see executeFrame).
     *
     * @param scratch The driving session's collection scratch (the plan lives in it).
     * @param driving The window index the frame drives, or VsgExecutor::kNoWindow for the off-screen form.
     */
    void openFrameBooks(FrameScratch& scratch, std::size_t driving);

    /** @brief Resolves, ONCE, everything the frame will read from the DEVICE (the frame's head).
     *
     * @details
     * - WHY A HEAD AT ALL (`G10`): the collection walk that follows compiles a plan and records thousands of draws;
     *   taking the device's content lock per lookup would pay a mutex per draw. So the device's half of a frame is
     *   resolved HERE, under ONE acquisition of that lock: every pass' number (`PassRegistry::adopt`, into the
     *   scratch's `pass_ids`), every host target the passes draw into (adopted and built as far as its description
     *   allows), every sampled target's facts observed, every content object the drawing calls name tracked into the
     *   content store, the target-fact table the plan compiles against, the staging sweep, and the block storage's
     *   growth. The collection and compilation then read the SCRATCH only.
     * - WHAT IT DOES NOT DO: it opens no session frame and records nothing - a frame whose head ran and whose
     *   collection then fails simply leaves the tables as they were (a plan nobody commits).
     *
     * @param executor The driver whose frame this is (its default program was already handed to the scratch).
     * @param scratch  The driving session's collection scratch (its tables are filled here).
     * @param frame    The frame to collect, in execution order.
     * @param executing Whether this is an EXECUTED frame (an executed frame replaces the device's upload answer; a
     *                  warm-up leaves it describing the last frame that really ran).
     */
    void openFrameHead(VsgExecutor& executor, FrameScratch& scratch, const vn::graphics::FrameDescription& frame, bool executing);

    /** @brief Walks @p frame into the collection stage: one pass scope per entry, in the order the value states.
     *
     * @details
     * - THE TRANSLATION OF A FRAME VALUE INTO THIS LAYER'S OWN COLLECTION, and the only place it exists: the value's
     *   passes become pass scopes, their targets / viewports / clears / states / inputs are announced under those
     *   scopes, and their drawing calls are collected. `executeFrame` runs it and then records; `prepareFrame` runs
     *   the SAME one and then builds only the value side - so a prepared frame and a recorded frame cannot disagree
     *   about what the frame is (`G1`), which is what makes "prepared" carry over to the next frame.
     * - IT READS THE SCRATCH'S SNAPSHOT, NOT THE DEVICE: the pass numbers come from `scratch.pass_ids` (filled by
     *   openFrameHead), so the walk is a pure function of the frame value and the session's own memory. That is the
     *   property the device gate's shared side needs: two drivers' collections run at once.
     * - It touches no session and no image: opening the frame's books is the caller's step (see openFrameBooks).
     *
     * @param scratch The driving session's collection scratch.
     * @param frame   The frame to collect, in execution order.
     */
    void collectFrame(FrameScratch& scratch, const vn::graphics::FrameDescription& frame);

    /** @brief Rebuilds the target-facts table the plan is compiled against (the window row plus every held target).
     *
     * @details
     * - The rows are the same two sources `closeFrameBooks` has always used: the window the frame drives (answered by
     *   the target itself, a const question) and every host target this backend holds, built or not. They are read
     *   into `scratch.facts` by the frame's HEAD, because the table is a copy of the device's targets - reading it
     *   while another driver's frame changes those targets is exactly the race the head's lock removes.
     *
     * @param scratch The driving session's collection scratch (its facts are replaced).
     * @param driving The window index the frame drives, or VsgExecutor::kNoWindow for the off-screen form.
     */
    void collectTargetFacts(FrameScratch& scratch, std::size_t driving);

    /** @brief Closes the frame's books: the plan is closed and compiled against what the frame draws into.
     *
     * The second half of the old `endFrame()`: the collection stage's frame closes and swaps its buffers, and the
     * plan is compiled against the facts the frame is answered for. Runs on the driving session's scratch and takes
     * no device lock; the compiler reports through the device's route, so a plan that could not be served is heard
     * while no device lock is held (which is what lets a test hold a compiling frame and see another one finish).
     */
    void closeFrameBooks(FrameScratch& scratch);

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
     * @param executor  The DRIVER whose executor state this frame uses (see createSession).
     * @param window    The window's session that owns the frame, or null when the frame names no window.
     * @param offscreen The off-screen session that owns the frame, or null when the frame belongs to a window
     *                  (exactly one of the two is non-null).
     * @return What the frame did (see PresentOutcome).
     */
    vn::graphics::PresentOutcome commitFrameThrough(VsgExecutor& executor, api::WindowSession* window, api::OffscreenSession* offscreen);

    /** @brief Runs @p frame through @p executor: the body the device's own driver and every handed-out one share.
     *
     * @param executor  The driver's executor state (see createSession).
     * @param frame     The frame to run, in run order.
     * @param present_to Target the frame's default framebuffer resolves to, or nullptr to present nowhere.
     * @return What the frame did (see PresentOutcome).
     */
    vn::graphics::PresentOutcome executeFrameFor(VsgExecutor& executor,
                                                 const vn::graphics::FrameDescription& frame,
                                                 vn::graphics::PresentTarget*          present_to);

    /** @brief Prepares @p frame's content through @p executor (see RenderFrameExecutor::prepareFrame).
     *
     * @param executor  The driver's executor state (see createSession).
     * @param frame     The frame to prepare, in execution order.
     * @param present_to Target the frame's default framebuffer resolves to, or nullptr for the off-screen form.
     * @return What the call built (see PrepareReport).
     */
    [[nodiscard]] vn::graphics::PrepareReport prepareFrameFor(VsgExecutor& executor,
                                                              const vn::graphics::FrameDescription& frame,
                                                              vn::graphics::PresentTarget*          present_to);

    /** @brief Announces @p executor's default content program and tracks it in the device's store.
     *
     * @param executor The driver that made the announcement.
     * @param program  The program, or null for "none".
     */
    void adoptDefaultProgram(VsgExecutor& executor, vn::intrusive_ptr<const vn::graphics::ShaderProgram> program);

    /** @brief Opens a pass' scope under the number the frame head resolved (see api/PassRegistry and scratch.pass_ids).
     *
     * @param scratch The driving session's collection scratch (the recorder lives in it).
     * @param pass    The pass identity the value named (null is skipped by the caller).
     * @param id      The pass number openFrameHead resolved for it (0 never reaches the recorder; the type is
     *                core::PassId, spelled as uint32_t here so this header needs no recorder include - Qt defines
     *                `slots` as a macro, and a recorder member of that name would collide in a Qt translation unit).
     */
    void openPassScope(FrameScratch& scratch, vn::raw_ptr<const vn::graphics::RenderPass> pass, std::uint32_t id);

    /** @brief Closes the pass scope (the recorder drops what no draw consumed). */
    void closePassScope(FrameScratch& scratch);

    /** @brief Announces the pass' stacking order (see FrameDescription::Pass::order). */
    void announcePassOrder(FrameScratch& scratch, int order);

    /** @brief Announces the pass' resolved inputs, in declaration order (see FrameDescription::Pass::inputs).
     *
     * The targets' facts were already observed by the frame head (see openFrameHead); this is the recorder's own copy.
     */
    void announcePassInputs(FrameScratch& scratch, const std::vector<vn::graphics::ImageSource>& inputs);

    /** @brief Announces the target a pass draws into, as the recorder's identity for it.
     *
     * null means the default framebuffer, which the plan spells the same way. The target itself was HELD and built (as
     * far as its description allows) by the frame head (see openFrameHead and TargetTable) - the collection walk only
     * tells the recorder which identity the pass names, so it touches no device table.
     */
    void announceTarget(FrameScratch& scratch, vn::raw_ptr<vn::graphics::RenderTarget> target);

    /** @brief Announces the pass' sub-rectangle (see PassDraw::viewport). */
    void announceViewport(FrameScratch& scratch, const vn::graphics::Viewport& viewport);

    /** @brief Announces the pass' clear policy, translated into the plan's spelling (see PassDraw::clear). */
    void announceClear(FrameScratch& scratch, const vn::graphics::ClearPolicy& policy);

    /** @brief Announces the pass' own state for the content it draws (see PassDraw::state). */
    void announcePassState(FrameScratch& scratch, const vn::graphics::PassRenderState& state);

    /** @brief Announces the depth band the pass maps its draws into (see PassDraw::depth_range). */
    void announceDepthRange(FrameScratch& scratch, const vn::graphics::DepthRange& range);

    /** @brief Announces the lights of one drawing call, as VALUES (see DrawDescription::light_values). */
    void announceLights(FrameScratch& scratch, std::span<const vn::graphics::LightValue> lights);

    /** @brief Collects one content drawing call into the scratch (the objects it names were tracked by the head).
     *
     * @param scratch          The driving session's collection scratch.
     * @param commands         The pass' collected commands (shared and immutable - never rewritten here).
     * @param camera           The view the draw is projected with (the frame's value, not the authored object).
     * @param program_override The pass' program override, applied to every command when the draw is recorded
     *                         (see PassDraw::program_override), or null for each command's own program.
     */
    void collectContentDraw(FrameScratch&                                   scratch,
                            const std::vector<vn::graphics::RenderCommand>& commands,
                            const vn::graphics::CameraSnapshot&             camera,
                            const vn::graphics::ShaderProgramDescriptionPtr& program_override);

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
    [[nodiscard]] api::WindowSession& sessionAt(std::size_t index);

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

    /** @brief Collects one full-screen drawing call whose source is one of the pass' declared inputs.
     *
     * @param scratch             The driving session's collection scratch.
     * @param source              The target the call reads (its images were resolved by the content phase).
     * @param program             The program the call shades with (already tracked by the frame head).
     * @param program_description The program's text as the pass carried it.
     * @param camera              The view the call is projected with.
     */
    void collectScreenDraw(FrameScratch&                                   scratch,
                           vn::graphics::RenderTarget*                     source,
                           vn::raw_ptr<const vn::graphics::ShaderProgram>  program,
                           const vn::graphics::ShaderProgramDescriptionPtr& program_description,
                           const vn::graphics::CameraSnapshot&             camera);

    /** @brief Holds the host's target and builds what its description can make (the half setRenderTarget shares).
     *
     * @param target          The host's target (never null).
     * @param picture_depends Whether a pass of the frame being run draws into it: a description that cannot become
     *                        objects at all is REPORTED then (inside a frame, where it blocks the picture) and is
     *                        silent otherwise, because a host configures a target before it draws into it and a
     *                        borrow whose lender arrives later in that setup is order rather than failure.
     * @return true when the target is held (see TargetTable::State for what its objects are).
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
        BuildFailed,        // The objects could not be created.
        DepthSourceMissing, // The depth is borrowed from a target this backend does not hold.
        SamplesUnsupported, // The sample count is above the device's, or a multisampled target borrows a depth.
    };

    void reportUnbuildableTarget(vn::raw_ptr<vn::graphics::RenderTarget> target, UnbuildableReason reason, std::uint32_t max_samples);

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

    /** @brief Reports (once per episode) a host verb refused because a frame body of THIS thread called it.
     *
     * @details
     * - WHAT IT REPORTS AND WHY IT IS A REFUSAL: a diagnostics sink or a hook that calls a content verb is asking
     *   the device to change the tables a running frame is reading, from inside that same frame. Answering with the
     *   change would be a use-after-free of the frame's own plan; waiting would be the thread waiting for itself. So
     *   the verb answers "nothing happened" and the reason is said once - the same shape the readback's
     *   `InsideFrame` refusal takes.
     *
     * @param verb The entry point's name, as the sentence states it.
     */
    void reportReentry(const char* verb) noexcept;

    /** @brief Runs this frame's sweep: what the host let go is let go (see the class note and the frame drive).
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

    /** @brief The one body behind the two buffer overloads of releaseContent (the element type is not needed).
     *
     * @param buffer Address a `StreamKey` names; null is refused.
     * @return true when the release was accepted (see the public overloads).
     */
    bool releaseStreamBuffer(const void* buffer);

  private:
    friend class detail::BackendContentAccess;
    /// The driver hands its own executor state to the device's frame body (see createSession).
    friend class VsgDriver;

    struct Data;
    std::unique_ptr<Data> d;
};

VN_BACKEND_VSG_NS_END
