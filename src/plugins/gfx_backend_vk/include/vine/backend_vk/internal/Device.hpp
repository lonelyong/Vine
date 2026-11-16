#pragma once

#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/backend/FrameGate.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>
#include <vine/backend_vk/internal/Handles.hpp>
#include <vine/backend_vk/internal/MemoryAllocator.hpp>
#include <vine/backend_vk/internal/ResidenceTable.hpp>
#include <vine/backend_vk/internal/TargetClaims.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The direct-Vulkan backend's device (milestone M0: lifecycle and honest capabilities).
 *
 * @details
 * - WHAT IT IS. A `RenderBackend` implemented directly on Vulkan through volk: it owns one `VkInstance`, one
 *   `VkPhysicalDevice`, one `VkDevice` and one graphics queue, off-screen targets with their render passes, and (M2)
 *   one content draw per `RenderCommand` - a GLSL program compiled through glslang into a pipeline, the geometry's
 *   channels uploaded as vertex/index streams, the `VineViewBlock`/`VineDrawBlock` pair bound as uniforms, and
 *   `vkCmdDrawIndexed`.
 * - WHAT IT DOES, MILESTONE BY MILESTONE. M4 landed the engine's content ABI (material, texture slots, lights, the
 *   draw block and the 128-byte camera push) and the full-screen draw path; M5 serves a HOST window
 *   (`DevicePresentation::Handle`) through a per-window swapchain, and brings up its OWN window for a `Probe` device;
 *   M6 makes residency an answered question - `releaseContent` lets go of what a host names, `prepareFrame` builds a
 *   frame's content without running it, and `frameReport` says what the last frame did and what is resident; M8
 *   compiles the engine's own programs under the variant names their drawables ask for; M9 records a DEPTH-ONLY pass
 *   (a target with a depth attachment and no colour attachment is the engine's shadow map) and binds that map plus the
 *   `VineShadowBlock` a pass' declared inputs resolve to; M10 runs the engine's DEFERRED chain - the four-attachment
 *   RGBA16F G-buffer, then a full-screen lighting pass that samples it through the pass' declared inputs; M11 draws
 *   MULTI-SAMPLED targets - the colour is drawn into multisampled sample buffers and RESOLVED into the single-sample
 *   image a later pass samples and a readback answers, a sampleable depth is resolved the same way through
 *   `VkSubpassDescriptionDepthStencilResolve` (`sample0`, which needs `vkCreateRenderPass2`), and a count the device
 *   cannot build is REFUSED once rather than lowered (ADR-2026-11-14-sample-count-refused-not-lowered); M12 opens the
 *   one escape door (`support/Handles.hpp`) and is kept honest by `scripts/check_vk_exposure.py`; M13 lets a warm-up
 *   submit ONE transfer-only batch, so `prepareFrame` can stage a texture without running a frame. It answers
 *   `capabilities()` from what it can really serve - `render_targets == true`, and `presentation == true` once a
 *   window target can present.
 * - WHAT IT STILL DOES NOT DO. Point-light (cube) shadows: the engine's one map is 2D, so a cube needs a layered
 *   target and six matrices (backlog S6, not triggered). The depth RESOLVE needs Vulkan 1.2; a device without it
 *   refuses a multisampled target whose depth a shader samples, once, instead of drawing a depth nobody can read.
 * - FRAME AND HOST VERBS. `executeFrame()` runs the frame's passes in the order it carries and records the draws of each
 *   `DrawDescription` - an off-screen pass into its target, and a pass naming no target into the acquired image of the
 *   window the frame presents through. A draw naming no program is reported and left out. The readbacks answer the
 *   targets this device built.
 * - VULKAN LIVENESS. Every handle lives in the implementation object; `shutdown()` returns the device to the state of
 *   a freshly constructed one (idempotent, safe before `initialize()`), which is what the SDK's contract requires of a
 *   backend whose surface can be recreated.
 *
 * @note The class is `Device` and not `VkDevice`: the latter is Vulkan's own type, and this header's implementation
 *       includes volk (see `backend_vk_global.hpp`).
 *
 * @warning FRAME BODIES OVERLAP; MOST HOST VERBS WAIT FOR THEM. `executeFrame` and `prepareFrame` go through their
 *          driver's `FrameGate` first (a second call from the same thread is refused; another thread waits for that
 *          DRIVER), and then take the device's body gate SHARED: two drivers' frames record and submit at the same
 *          time (S6c-2). What a frame touches is its own (scratch, command pool, fence, upload batch) or under its own
 *          lock (the caches at the frame head, the allocator, the target map, the device clock). A frame that
 *          PRESENTS is the exception - the swapchain and its semaphores are one device resource - so a window frame
 *          takes the gate EXCLUSIVELY. THE RELEASE VERBS DO NOT TAKE IT (R6): `releaseContent` (all three) and
 *          `releaseRenderTarget` remove what the host named and PARK it against the completion watermark, so they may
 *          run while another driver's frame records or submits - they wait at most for one cache operation, never for
 *          a frame. The verbs that cannot overlap a frame - `readColorBuffer`, `readDepthBuffer`,
 *          `setDefaultContentProgram`, `createSurfaceTarget`, the diagnostic readers, `shutdown` - take the gate
 *          EXCLUSIVELY and may WAIT for the frames in flight; they do not race them. `initialize()` is the creator's
 *          call, made with no frame in flight.
 *
 * @note A FRAME CONSULTS THE CACHES ONCE (S6c-1). The frame head resolves every program, stream, pipeline and texture
 *       upload under one cache hold and keeps the answers as values (`ContentCaches::CommandPlan`); the recording path
 *       then takes no cache lock and names no cache entry. Texture uploads and the images a revision replaced belong to
 *       the DRIVER's batch (`ContentImages::BatchId`), and a replaced image is parked against the watermark, so the
 *       frame that ends first cannot free an object the frame still recording names.
 */
class VN_BACKEND_VK_API Device final : public vn::graphics::RenderBackend {
  public:
    /**
     * @brief Builds a device for @p desc, without creating anything yet.
     *
     * @details
     * - Construction is inert: no Vulkan object exists until `initialize()`, which is what the SDK's lifecycle says
     *   (a factory answers an object; the creator brings it up).
     *
     * @param desc How this device is meant to present (see DeviceDesc); M0 remembers it and answers
     *             `presentation == false` whatever it says.
     */
    explicit Device(const vn::graphics::DeviceDesc& desc);

    /** @brief Releases the session (see shutdown). */
    ~Device() override;

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;

  public:
    /**
     * @brief Brings up the instance, the physical device and the logical device.
     *
     * @details
     * - The validation layer is enabled whenever the loader reports it, and its messages are forwarded to this
     *   device's diagnostic route through `reportDiagnostic`.
     * - Nothing is created for a device that already runs: a re-announced, unchanged session answers true without
     *   touching what is up.
     *
     * @return true when the device is ready; false when the loader, a queue family or the logical device is missing.
     *         On false the device has already released whatever it built.
     */
    [[nodiscard]] bool initialize() override;

    /** @brief Releases every Vulkan object the session owns (idempotent, safe before initialize). */
    void shutdown() override;

    /** @brief Gets whether initialize() succeeded and no shutdown() has run since. */
    [[nodiscard]] bool initialized() const noexcept override;

    /** @brief Gets the frame half of this device, which is this object (see RenderDevice::executor). */
    [[nodiscard]] vn::graphics::RenderFrameExecutor& executor() override;

    /**
     * @brief Borrows a NEW frame driver (see RenderDevice::createSession).
     *
     * @details
     * - THE OBJECT IS THE DEVICE'S FROM THIS CALL ON, and it is created INERT: `initialize()` is what gives it
     *   a command pool, a fence and a frame scratch (R1 - an engine attaches to a device its creator has not
     *   brought up yet). Releasing it before then simply drops the object.
     * - WHAT THE DEVICE PROMISES TODAY: every driver owns its frame machinery and its scratch, and TWO DRIVERS'
     *   FRAMES MAY RUN AT THE SAME TIME (S6c-2): the caches are consulted once per frame head under one lock, the
     *   allocator, the target map and the device clock each have their own, and a window frame is serialised with
     *   every other frame because the swapchain is one device resource (see `.ai/design/graphics-sessions.md`).
     *
     * @return The new driver, or null when the device is lost.
     */
    [[nodiscard]] vn::graphics::RenderFrameExecutor* createSession() override;

    /**
     * @brief Returns a driver borrowed through @ref createSession (see RenderDevice::releaseSession).
     *
     * @details
     * - Releasing the device's OWN driver is a no-op (the device keeps it for its lifetime).
     * - The caller guarantees nothing is driving it; the device may destroy its objects at once.
     *
     * @param session The driver to return (ignored when this device never handed it out).
     */
    void releaseSession(vn::graphics::RenderFrameExecutor& session) override;

    /**
     * @brief Selects the program content without one of its own is drawn with.
     *
     * @details
     * - M2 does not compile the program by itself: the announcement takes the program's TEXT as a value (see
     *   `ShaderProgram::description`), so a later edit of the object cannot change what a recorded frame draws.
     * - A program the content path cannot compile (no vertex or fragment stage, GLSL glslang refuses) is reported on
     *   this device's diagnostics channel and the content it would shade is skipped, not drawn with something else.
     *
     * @param program Program to shade program-less content with, or null for "none".
     */
    void setDefaultContentProgram(vn::intrusive_ptr<const vn::graphics::ShaderProgram> program) override;

    /**
     * @brief Remembers the target a host announced; M0 builds no attachments for it.
     *
     * @param target Target the host will draw into, or null for the default framebuffer.
     */
    void setRenderTarget(vn::graphics::RenderTarget* target) override;

    /**
     * @brief Frees the attachments this device built for @p target.
     *
     * @details
     * - WHAT IT FREES. Every attachment built for the target - the colour images (one per MRT slot), the depth image
     *   and a multisampled target's resolve images, their views, the framebuffer(s), the render pass and the device
     *   memory behind them - and forgets the entry, so the target is "not built" afterwards.
     * - WHAT IT KEEPS. The host's `RenderTarget` object: it is a description, and a later frame that names it builds
     *   the attachments again through the same `planTarget` path (so the shape is re-checked, never assumed).
     *   Release is a residency choice, not a lifetime change.
     * - WHEN IT RUNS. ANY TIME, including while another driver's frame is in flight (R6): the entry leaves the table
     *   at once and the images are PARKED against the device's completion watermark, so the call returns without
     *   waiting for a frame - it waits at most for one cache operation (the frame head's lock). See RetirementQueue.
     * - NOT THE WINDOW HALF. A window target's swapchain belongs to the device's lifetime (`createSurfaceTarget`);
     *   this verb is for the off-screen targets the host announced, which is what `RenderDevice` states.
     * - A target this device never built, or one already released, answers immediately: there is nothing to free and
     *   nothing to report.
     *
     * @param target Target whose attachments to free; null is ignored.
     */
    void releaseRenderTarget(vn::graphics::RenderTarget* target) override;

    /** @brief Gets what this device can serve TODAY (see Capabilities): off-screen targets and, when asked, a window. */
    [[nodiscard]] vn::graphics::Capabilities capabilities() const noexcept override;

    /**
     * @brief Creates (or answers) the target this device presents @p native_handle through.
     *
     * @details
     * - ONE LIVE TARGET PER WINDOW: a second ask for the same handle answers `WindowOccupied` and hands none out
     *   (the host already holds the target the first ask answered).
     * - A NULL HANDLE IS THE DEVICE'S OWN WINDOW on a `Probe` device: the window is brought up here (or when the
     *   device came up, whichever is first) and the target presents through it. On a `Handle` device a null handle
     *   is wiring it cannot repair, so it answers `UnsupportedSurface` and reports it once.
     * - `DevicePresentation::None` answers `NoPresentation` silently.
     * - The target is BORROWED and the device owns it (it lives until `shutdown()`); a target created before
     *   `initialize()` is attached when the device comes up, which is the shape the SDK's contract describes.
     *
     * @param native_handle Native window handle (`HWND` on Windows), or null for the device's own window.
     * @param why           Receives why no target was created (Ok when one was), or null to ignore.
     * @return The target, or null (see @p why).
     */
    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget> createSurfaceTarget(void* native_handle,
                                                                                  vn::graphics::SurfaceTargetResult* why = nullptr) override;

    /**
     * @brief Releases this device's GPU residency of a content object the host still owns.
     *
     * @details
     * - WHAT IT DROPS. A `Geometry` loses its uploaded streams, a `ShaderProgram` loses its compiled modules AND the
     *   pipelines built from them, a `Texture` loses its uploaded image; a `Material` (asset or instance) is
     *   recognized and accepted although nothing is retained for it - this backend packs a material's values into
     *   each draw's uniform buffer, so there is no per-material GPU state to let go.
     * - WHAT IT KEEPS. Everything keyed by something else: a geometry release leaves the pipelines (they are keyed
     *   by the program, the target shape and the vertex layout), and the next frame that draws the content uploads
     *   or compiles it again - which is what the release costs. The object itself stays valid and keeps its
     *   revision; a copy is released, never the host's object.
     * - WHEN IT RUNS. ANY TIME, including while another driver's frame is in flight (R6): what this call lets go of
     *   leaves the caches at once and its Vulkan objects are PARKED against the completion watermark, so a frame that
     *   had already resolved them finishes with them, and a later frame builds them again. It waits at most for one
     *   cache operation, never for a frame (see RetirementQueue).
     * - NOT the window half or a target: a window target's swapchain belongs to the device's lifetime, `releasePass`
     *   is a no-op (this device keeps no per-pass objects), and `releaseRenderTarget` frees a target's attachments
     *   (see its own note).
     *
     * @param content Content object to evict; null is refused.
     * @return true when @p content was recognized (a texture with no uploaded image is already released), false when
     *         it is not content or the device is not up.
     */
    bool releaseContent(vn::raw_ptr<const vn::Object> content) override;

    /**
     * @brief Releases the streams uploaded from @p buffer (its identity, not its bytes).
     *
     * @details
     * - PER STREAM: a geometry that reads three buffers keeps the two nobody released, and the next frame that
     *   uploads the released one leaves the others alone. The geometry's entry - and every pipeline - stays.
     * - A buffer no stream was uploaded from is already released (true).
     *
     * @param buffer Stream buffer (a geometry's vertex channel or its instance transforms).
     * @return true when the buffer was recognized (or had nothing resident), false when the device is not up.
     */
    bool releaseContent(const vn::Buffer<float>& buffer) override;

    /** @brief Releases the index streams uploaded from @p buffer (see the float overload). */
    bool releaseContent(const vn::Buffer<std::uint32_t>& buffer) override;

    /**
     * @brief Reads back a colour attachment of an off-screen target this device drew into.
     *
     * @details
     * - M1 answers the targets it built, as packed RGBA8 (the SDK's readback contract), and nothing else: another colour
     *   format, an attachment index above 0, a target it never saw or a target whose attachments are not built answer
     *   the matching `ReadbackResult` instead of a picture.
     * - The copy is synchronous: one command buffer, one fence, the pixels the last frame left. THE MACHINERY IS THE
     *   READBACK'S OWN (`ReadbackStaging`'s command pool and fence), so a readback never borrows a frame driver's
     *   command buffer (C7 of `.ai/design/graphics-sessions.md`).
     * - IT WAITS FOR THE FRAMES IN FLIGHT, AND THE WAIT IS COUNTED: with several drivers "between frames" is a moment
     *   the device names for the host, and every readback is one stop in `RetirementNumbers::device_waits` (the same
     *   witness the reference backend raises with its readback idle). A refusal answered without waiting does not.
     * - REFUSED FROM INSIDE A FRAME: a diagnostic sink or hook that calls this while this device drives a frame
     *   answers `Failed` instead of waiting for the frame it is itself inside; read from the host's own thread, after
     *   the frame.
     *
     * @param target     Target to read (borrowed, may not be null).
     * @param attachment Colour attachment index; M1 serves 0.
     * @param outPixels  Receives `width * height * 4` bytes on success (untouched on every refusal).
     * @param why        Receives why the read did not happen (Ok when it did), or null to ignore.
     * @return true when the pixels were read.
     */
    bool readColorBuffer(const vn::graphics::RenderTarget* target,
                         int                               attachment,
                         std::vector<std::uint8_t>&        outPixels,
                         vn::graphics::ReadbackResult*     why = nullptr) override;

    /**
     * @brief Reads back the depth attachment of an off-screen target this device drew into.
     *
     * @details
     * - The copy is synchronous, like readColorBuffer: one command buffer, one fence, the depths the last frame left,
     *   as `width * height` floats in [0, 1], row-major. It owns the command pool and fence it runs on, waits for the
     *   frames in flight (one counted stop in `RetirementNumbers::device_waits`, like the colour readback) and is
     *   refused with `Failed` when asked from inside a frame body.
     * - IT READS THE DEPTH PICTURE: a multisampled target's resolved single-sample image (see
     *   `TargetImages::depthPicture`), never the sample buffer - multisampling cannot change the depths a reader sees.
     * - WHICH FORMATS, from the SDK's own table (`core::depthReadbackOf`, the table `decodeDepth` reads): D16, D32 and
     *   D32F have a plain depth copy; D24 is Vulkan's COMBINED depth/stencil format with no plain depth copy, so it
     *   answers Unsupported instead of decoding bytes that are not depths - the reference backend answers the same way.
     * - A BORROWED depth (`RenderTarget::shareDepth`) answers Unsupported: the image belongs to the lender, and the
     *   SDK's rule is that it is read through the SOURCE target.
     * - Only a successful read changes @p outDepths; every refusal (Invalid / NotReady / Unsupported / Failed) leaves
     *   it untouched and costs no submission.
     *
     * @param target    Target to read (borrowed; null is refused as Invalid).
     * @param outDepths Receives `width * height` depths on success.
     * @param why       Receives why the read did not happen (Ok when it did), or null to ignore.
     * @return true when the depths were read.
     */
    bool readDepthBuffer(const vn::graphics::RenderTarget* target,
                         std::vector<float>&               outDepths,
                         vn::graphics::ReadbackResult*     why = nullptr) override;

    /**
     * @brief Runs the frame's passes and presents the window's picture when a window is named: each pass clears or
     *        loads its target, then records the drawing calls of every `DrawDescription` it carries.
     *
     * @details
     * - WHICH PASSES RUN. One that names an off-screen target is recorded as one render pass (colour attachments
     *   CLEAR or LOAD, depth CLEAR or LOAD, as `ClearPolicy` says); one that names no target draws into the acquired
     *   image of `present_to` when that is a window this device handed out, and the frame's tail presents it (M5).
     *   A frame that names no presenter, or a pass that names a target this device cannot build, is reported once and
     *   left out.
     * - WHAT A PASS SAMPLES. A pass that DECLARES an input which is a shadow map (a target stating whose shadow it is,
     *   with a published view-projection) gets that map bound and its `VineShadowBlock` packed per drawing call: a map
     *   whose light this call does not announce, or whose light stopped casting, leaves the switch off and shades
     *   nothing. The map's depth image is moved to `SHADER_READ_ONLY_OPTIMAL` before the pass begins (a barrier inside
     *   a render pass is not legal), and back before a later pass writes that depth.
     * - Each scene draw is projected with its own camera snapshot and shaded with the resolved program (the pass'
     *   override, else the command's own, else the default content program). One `RenderCommand` is one indexed draw.
     *   A pass whose target has no colour attachment (a shadow map) draws with a depth-only pipeline and its fragment
     *   outputs are discarded.
     * - The submission is synchronous by design: one command buffer, one fence, then the queue is idle, so a readback in
     *   the same breath sees exactly what this frame left - and a content release between frames needs no wait.
     * - A LOST DEVICE ENDS THE SESSION (see `noteDeviceLost`): a later frame is skipped without touching the device
     *   (the one sentence was reported where the loss was seen) and answers `Lost` when it expected a window - the
     *   outcome a host acts on, with `shutdown()` + `initialize()` as the rebuild path.
     *
     * @param frame      The frame to execute (borrowed for the call).
     * @param present_to Window the default framebuffer resolves to, or null for a frame that has no window.
     * @return `Presented` when a window took the picture, `Skipped` for the off-screen form (a first-class state, not
     *         a failure) and `Lost` when a window was named and the picture could not reach it.
     */
    [[nodiscard]] vn::graphics::PresentOutcome executeFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to) override;

    /**
     * @brief Builds a frame's CONTENT ahead of time - programs, streams, pipelines, target attachments, textures - and
     *        submits no frame.
     *
     * @details
     * - THE SAME VALUE `executeFrame` takes, walked to the same identities: this device resolves each pass' target,
     *   each command's program and geometry and the pipeline its state selects (see `ContentDraw::prepare`), and it
     *   records the pass' texture uploads and the 1x1 stand-in (see `ContentDraw::preparePass`), so a later frame of
     *   the same declarations builds nothing (its upload/compile deltas are zero).
     * - WHAT IT DELIBERATELY DOES NOT DO. No swapchain image is acquired, no window is resized, no render pass is
     *   opened, nothing is drawn or presented, and `FrameProgress`'s `submitted` / `presented` do not move: those
     *   count FRAMES. The window passes are walked for their shapes but build no pipeline of their own unless the
     *   swapchain already exists (`WindowTarget::ready`).
     * - THE ONE THING IT MAY SUBMIT is a command buffer holding nothing but transfers (the 1x1 stand-in and the
     *   material textures), submitted on the device's own fence and waited for before the call answers, so what the
     *   report counts is resident when it does. No work means no submit: a second prepare of an unchanged frame
     *   submits nothing (see ADR-2026-11-14).
     * - A pass this device cannot serve yet (its target has no objects) is not an error: it is counted in
     *   `passes`/`draws` (the shape of the frame asked for) and the build counts say what became objects.
     *
     * @param frame      The frame to prepare (borrowed for the call).
     * @param present_to The surface the frame's default framebuffer resolves to, or null for the off-screen form.
     *                   Nothing is acquired from it.
     * @return What this call built (see PrepareReport); `NotReady` before `initialize()`.
     */
    [[nodiscard]] vn::graphics::PrepareReport prepareFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to = nullptr) override;

    /**
     * @brief Gets this device's own report about its frames (see FrameReport).
     *
     * @details
     * - `counters` and `uploads` describe the frame that just ENDED; `retention` and `progress` are cumulative. The
     *   residency ledger is composed HERE, where the stream table, the image cache and the pipeline map are all
     *   reachable, and it reports the three kinds this backend retains (streams, images, compiled halves) with their
     *   live counts and bytes - material blocks and descriptor sets are per-frame objects here, so no row claims
     *   them.
     * - `uploads` stays DISENGAGED until a frame has run: an unmeasured frame must not read like an empty one.
     *
     * @return The report, with the parts this device keeps engaged.
     */
    [[nodiscard]] vn::graphics::FrameReport frameReport() const noexcept override;

  public:
    /**
     * @brief Gets the Vulkan handles this device created, for tools and experiments.
     *
     * @details
     * - THE INTERNAL HALF OF THE ESCAPE DOOR. The value is `support/Handles.hpp`'s `Handles`, which is what
     *   `vkHandles()` hands a test or a tool; this accessor is how the plugin reaches the same facts without a
     *   `dynamic_cast`, and it is the only reason this header names that type (see
     *   `.ai/research/graphics-2026-11-13-vk-exposure-boundary.md` §4.5, §5.2).
     * - It is a value whose name says what it is, never a verb on `RenderDevice`, and it is not a promise any other
     *   backend makes.
     * - THE FRAME PATH DOES NOT USE IT: no source under `src/internal/` includes `support/Handles.hpp`, so the plugin
     *   itself never depends on the door it offers.
     *
     * @return The handles, all `VK_NULL_HANDLE` (family 0) before `initialize()` and after `shutdown()`.
     */
    [[nodiscard]] Handles handles() const noexcept;

    /**
     * @brief Gets how many content pipelines this device built over its lifetime.
     *
     * @details
     * - THE EVIDENCE FOR "A RESIZE MUST NOT RECOMPILE": a frame drawn again after an EXTENT change leaves this
     *   unmoved (the pipeline is compatible with the new framebuffer), while a shape change or a new program moves it.
     *   Like `handles()`, it is a diagnostic fact this backend offers the host, not a verb the SDK promises.
     *
     * @return Pipelines created since the device came up (0 when it built none).
     */
    [[nodiscard]] std::uint64_t contentPipelines() const noexcept;

    /** @brief The recorder's cache witnesses (see ContentDraw::Progress): what the cost model actually did. */
    struct ContentCacheCounters {
        std::uint64_t ring_builds{ 0 }; ///< Uniform rings created, cumulative (one in a steady session).
        std::uint64_t set_builds{ 0 };  ///< Descriptor sets BUILT, cumulative - a cache miss, not a draw.
        std::uint64_t sets_live{ 0 };   ///< Descriptor sets cached NOW.
        /// CACHE LOCK ACQUISITIONS, cumulative (see ContentCaches::lockAcquisitions). THE WITNESS FOR "THE RECORDING
        /// PATH TAKES NO CACHE LOCK": a frame resolves its content once at its head, so this number grows with FRAMES
        /// and with host verbs, never with the drawing calls inside a frame.
        std::uint64_t cache_lock_acquisitions{ 0 };
    };

    /** @brief What the park queue holds: the releases waiting for the frame that could still name them. */
    struct RetirementNumbers {
        std::uint64_t pending{ 0 };      ///< Releases waiting for their frame to be proven complete.
        std::uint64_t released{ 0 };     ///< Releases the queue has run, cumulative.
        std::uint64_t device_waits{ 0 }; ///< Destructive idles taken because parking was unavailable.
    };

    /**
     * @brief Gets the recorder's cache witnesses (see ContentCacheCounters).
     *
     * @details
     * - THE EVIDENCE FOR THE COST MODEL: a draw's constants go into one ring (bound through dynamic offsets) and a
     *   descriptor set is cached by the resources it binds, so a steady frame creates no ring and builds no set. A
     *   test asks these three numbers instead of counting driver calls from outside - the same arrangement
     *   `contentPipelines()` uses for "a resize must not recompile".
     *
     * @return The witnesses (zeros when no recorder is up).
     */
    [[nodiscard]] ContentCacheCounters contentCacheCounters() const noexcept;

    /**
     * @brief Gets the device memory pool's numbers (see MemoryStats).
     *
     * @details
     * - THE EVIDENCE FOR SUBALLOCATION: this backend's resources - attachments, textures, vertex and index streams,
     *   the uniform ring, staging and readback buffers - all take SLICES of one pool (see MemoryAllocator), so the
     *   number of `VkDeviceMemory` objects a session creates is `blocks`, not `allocations`. `maxMemoryAllocationCount`
     *   is a per-device limit whose failure reads like "out of memory", and a test can ask these numbers instead of
     *   counting driver calls from outside.
     * - THE NUMBERS ARE THE SESSION'S: `blocks` and `allocations` are cumulative, while `blocks_live`, `reserved_bytes`
     *   and `used_bytes` describe NOW (zeros after `shutdown()`).
     *
     * @return The pool's numbers (zeros when no session is up).
     */
    [[nodiscard]] MemoryStats memoryStats() const noexcept;

    /** @brief What the park queue holds: the releases waiting for the frame that could still name them.
     *
     * @details
     * - A RELEASE IS TWO FACTS, and residency answers only the first: the cache forgets an entry at once (so the
     *   residency numbers drop and the next frame uploads it again), while the Vulkan objects behind it live until the
     *   completion watermark has passed every frame that could still read them. Without this pair a test cannot tell
     *   "destroyed" from "handed over to the queue".
     * - `device_waits` must stay 0 on a session whose queue is armed: a non-zero number means a release could not be
     *   parked and the device was idled instead (see RetirementQueue::parkingAvailable).
     *
     * @return The queue's numbers (zeros when no session is up).
     */
    [[nodiscard]] RetirementNumbers retirementNumbers() const noexcept;

    /**
     * @brief Gets the device's TARGET CLAIMS: the per-frame leases two drivers contend for (see TargetClaims).
     *
     * @details
     * - EXPOSED FOR THE SAME REASON `retirementNumbers()` IS: a claim is a fact about the DEVICE, not about one
     *   frame, and a test that plays "the other driver" must be able to take a target for a while. It is not a
     *   verb `RenderDevice` promises.
     *
     * @return The claims (no target is held between frames).
     */
    [[nodiscard]] TargetClaims& targetClaims() noexcept;
    /** @brief Gets the target claims for a reader that only reports on them (see the mutable overload). */
    [[nodiscard]] const TargetClaims& targetClaims() const noexcept;

    /**
     * @brief Gets the device's RESIDENCE TABLE: who may build a geometry and who has to copy it (see ResidenceTable).
     *
     * @details
     * - EXPOSED FOR THE SAME REASON `targetClaims()` IS: residency is a fact about the DEVICE, not about one frame,
     *   and a test that plays "the other driver" must be able to hold a claim over a key while a real frame runs
     *   (see `.ai/design/graphics-sessions.md` §3.1.4, F4). It is not a verb `RenderDevice` promises.
     * - It is not gated on the body gate: it has its own lock, and the frame path asks it while a frame runs.
     *
     * @return The table (empty before `initialize()`, cleared by `shutdown()`).
     */
    [[nodiscard]] ResidenceTable& residence() noexcept;
    /** @brief Gets the residence table for a reader that only reports on it (see the mutable overload). */
    [[nodiscard]] const ResidenceTable& residence() const noexcept;

    /**
     * @brief What the persisted pipeline cache did for this session (see PipelineCacheFile).
     *
     * @details
     * - THE EVIDENCE FOR A WARM START: `loaded_bytes` is a cache this session REUSED, `saved_bytes` one it produced for
     *   the next, and `discarded` says a file was there and refused - another device or driver, a corrupt file, an
     *   empty one. Without these numbers "the cache is not helping" and "the cache is not wired" look the same.
     * - IT SURVIVES `shutdown()`: the save happens while the session ends, so the numbers stay readable until the next
     *   `initialize()` starts a fresh report (the same rule `FrameReport::timing` follows per frame).
     */
    struct PipelineCacheReport {
        bool          persisted{ false };    ///< A cache directory was stated at creation (nothing is written without one).
        bool          discarded{ false };    ///< A file was there and refused (see PipelineCacheRefusal).
        std::uint64_t loaded_bytes{ 0 };     ///< Bytes accepted from the file and handed to `vkCreatePipelineCache`.
        std::uint64_t saved_bytes{ 0 };      ///< Bytes written when the session ended (0 when there was nothing to save).
        bool          hits_measured{ false }; ///< This device answers pipeline creation feedback, so `cache_hits` means something.
        std::uint64_t cache_hits{ 0 };        ///< Pipelines the driver served FROM the cache (see hits_measured).
        bool          preload_handed{ false };  ///< A previous session's blob was handed to `vkCreatePipelineCache`.
        bool          preload_refused{ false }; ///< The driver refused it (a corrupt cache; drivers differ in checking).
    };

    /**
     * @brief Gets what the persisted pipeline cache did (see PipelineCacheReport).
     *
     * @return The report (all zeros before `initialize()`).
     */
    [[nodiscard]] PipelineCacheReport pipelineCacheReport() const noexcept;

    /**
     * @brief Forwards one message the Vulkan loader or a validation layer produced.
     *
     * @details
     * - Called from the debug-utils callback (M0 has no frames, so every such message is an initialization-time one);
     *   the severity is the layer's, and it reaches the host on this device's diagnostic route like any other report.
     *
     * @param severity_flags `VkDebugUtilsMessageSeverityFlagBitsEXT` value, as an integer.
     * @param text           The layer's message, or null.
     */
    void reportDriverMessage(int severity_flags, const char* text);

    /**
     * @brief Installs the seam that creates device memory, so a test can reach the failure classes (see
     *        MemoryBlockCreator).
     *
     * @details
     * - WHY A DEVICE-LEVEL DOOR EXISTS. The classification this backend owes a host - a lost device ends the session,
     *   an exhausted heap is a fact the host can act on, a refused driver call is neither - is UNREACHABLE on a
     *   healthy machine, so the tests that pin it need a way to make `vkAllocateMemory` fail. The door is here, not
     *   on the allocator alone, because the classification lives on this device (see `noteMemoryRefusal`).
     * - Empty is the production path. Blocks are created lazily, so this may be installed after `initialize()` and
     *   before the first target or texture.
     *
     * @param creator The block creator, or empty for `vkAllocateMemory` through the device table.
     */
    void setMemoryBlockCreator(MemoryBlockCreator creator);

  private:
    /**
     * @brief Builds (or rebuilds) the Vulkan objects for @p target when they do not fit it.
     *
     * @details
     * - M1 compares the description it built against the target's current one; M3 replaces the comparison with
     *   `planTarget`'s `TargetDecision` (ResizeInPlace keeps the render pass, Rebuild replaces it).
     *
     * @param target The host's target (borrowed).
     * @return true when the objects exist and fit the target.
     */
    [[nodiscard]] bool ensureTarget(const vn::graphics::RenderTarget& target);

    /**
     * @brief Brings up the session's window system, opening its connection once.
     *
     * @details
     * - Called from `initialize()` (a Probe device's window must exist before the surface is created on it, and a
     *   Handle device's surface needs the connection) and from `createSurfaceTarget()` when a host asks for a target
     *   first - the SDK's documented order for the Handle form.
     * - Idempotent: one window system per session, and a second call only re-opens a connection that was closed.
     *
     * @param why Receives the reason when no window system came up, or null.
     * @return true when surfaces can be created through it.
     */
    [[nodiscard]] bool ensureWindowSystem(vn::String* why);

    /**
     * @brief Classifies a queue/acquire/present result: `VK_ERROR_DEVICE_LOST` ends the session and is reported once.
     *
     * @details
     * - A LOST DEVICE IS NOT A FAILED OPERATION. Every other failed result leaves this session usable (a refused
     *   submit is reported and the next frame tries again), while a lost device cannot answer another frame at all -
     *   so the answer a host needs is a different one: `initialized()` stops answering true, frames are skipped
     *   without touching the device, and the rebuild path is `shutdown()` + `initialize()` (`initialize()` also
     *   performs it on its own when it finds a lost session).
     * - Report ONCE per session: the sentence names the call that saw it, and a host polling frames after the loss
     *   must not be flooded with the same fact.
     *
     * @param result The `VkResult` of the call (any value; only `VK_ERROR_DEVICE_LOST` is classified as a loss).
     * @param what   The call's name, used in the sentence ("vkQueueSubmit", "vkQueuePresentKHR").
     * @return true when @p result was `VK_ERROR_DEVICE_LOST` (the session is now marked over).
     */
    bool noteDeviceLost(VkResult result, const char* what);

    /**
     * @brief Turns one allocation's outcome into this backend's ONE answer for it (see MemoryRefusalReport).
     *
     * @details
     * - THE ONE CLASSIFICATION POINT FOR MEMORY, the same way `noteDeviceLost` is the one for queue work. The pool
     *   reports every allocation here (see `MemoryAllocator::setFailureSink`), which is what makes it impossible for an
     *   allocation site to drop the reason: `allocate` has six callers and they only need "did it work".
     * - WHAT EACH CLASS BECOMES: `DeviceLost` takes `noteDeviceLost` (so the session ends once, however many
     *   allocations see it); `OutOfMemory` is a warning a host can act on by releasing content; `DriverRefused` is an
     *   error naming the driver's own result; `NoType` says this device cannot host the resource; the two wiring
     *   defects are reported as such rather than as memory pressure.
     * - `Ok` RE-ARMS THE EPISODES: an allocation that worked says the pool can allocate again, which is where the
     *   condition ends (the repository's `ReportOnce` rule).
     * - The sentence carries the numbers a host can act on: what was asked, which memory type served it, and the
     *   pool's reserved / used bytes at that moment.
     *
     * @param report The pool's account of one allocation (see MemoryRefusalReport).
     */
    void noteMemoryRefusal(const MemoryRefusalReport& report);

    /// The frame driver this device owns (see the class note): it holds the command machinery and the gate.
    class FrameDriver;

    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
