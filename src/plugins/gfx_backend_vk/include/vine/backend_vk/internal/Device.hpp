#pragma once

#include <vine/graphics/RenderBackend.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>

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
 *   `VineShadowBlock` a pass' declared inputs resolve to. It answers `capabilities()` from what it can really serve -
 *   `render_targets == true`, and `presentation == true` once a window target can present.
 * - WHAT IT STILL DOES NOT DO. Multi-sampled targets (`Capabilities::max_samples` is 1), the deferred G-buffer path,
 *   and point-light (cube) shadows. A request for either of the first two is reported once and served without it,
 *   rather than silently drawn differently.
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
 * @warning Not thread-safe: the SDK drives one frame at a time from one thread, and this device keeps no locks.
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
     * - WHEN IT RUNS. Between frames, on the frame thread. This device records, submits and waits on one frame at a
     *   time, so between frames nothing is in flight and what the host released is destroyed immediately (the
     *   parked-release machinery exists for devices whose frames overlap; see RetirementQueue).
     * - NOT the window half: a window target's swapchain belongs to the device's lifetime, and a pass or target
     *   release is `releasePass` / `releaseRenderTarget` (this device keeps no per-pass objects).
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
     * - The copy is synchronous: one command buffer, one fence, the pixels the last frame left.
     *
     * @param target     Target to read (borrowed, may not be null).
     * @param attachment Colour attachment index; M1 serves 0.
     * @param outPixels  Receives `width * height * 4` bytes on success.
     * @param why        Receives why the read did not happen (Ok when it did), or null to ignore.
     * @return true when the pixels were read.
     */
    bool readColorBuffer(const vn::graphics::RenderTarget* target,
                         int                               attachment,
                         std::vector<std::uint8_t>&        outPixels,
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
     *
     * @param frame      The frame to execute (borrowed for the call).
     * @param present_to Window the default framebuffer resolves to, or null for a frame that has no window.
     * @return `Presented` when a window took the picture, `Skipped` for the off-screen form (a first-class state, not
     *         a failure) and `Lost` when a window was named and the picture could not reach it.
     */
    [[nodiscard]] vn::graphics::PresentOutcome executeFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to) override;

    /**
     * @brief Builds a frame's CONTENT ahead of time - programs, streams, pipelines, target attachments - and submits
     *        nothing.
     *
     * @details
     * - THE SAME VALUE `executeFrame` takes, walked to the same identities: this device resolves each pass' target,
     *   each command's program and geometry and the pipeline its state selects (see `ContentDraw::prepare`), so a
     *   later frame of the same declarations builds nothing (its upload/compile deltas are zero).
     * - WHAT IT DELIBERATELY DOES NOT DO. No swapchain image is acquired, no window is resized, no command buffer is
     *   recorded, nothing is submitted or presented. The window passes are walked for their shapes but build no
     *   pipeline of their own unless the swapchain already exists (`WindowTarget::ready`).
     * - TEXTURES ARE THE EXCEPTION, and the honest one: a texture's first use copies bytes into a device-local
     *   image on the frame's command buffer (see `ContentImages`), and this call opens no command buffer - so the
     *   first frame that samples it still uploads it.
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
     * - THE ESCAPE DOOR, and the only place Vulkan may leave this plugin (see
     *   `.ai/research/graphics-2026-11-13-vk-exposure-boundary.md`): a value whose name says what it is, never a verb on
     *   `RenderDevice`. It exists because this backend is a LEARNING one - a validation experiment or a RenderDoc
     *   session needs the real handles - and it is not a promise any other backend makes.
     * - It lives in `internal/` for M0. When a tool needs it from outside the plugin, it moves to
     *   `include/vine/backend_vk/support/` with the same shape (the boundary document says why).
     *
     * @return The handles, all zero before `initialize()`.
     */
    struct Handles {
        std::uint64_t instance{ 0 };        // The `VkInstance`, as its own numeric value.
        std::uint64_t physical_device{ 0 }; // The `VkPhysicalDevice`.
        std::uint64_t device{ 0 };          // The `VkDevice`.
        std::uint64_t queue{ 0 };           // The graphics `VkQueue`.
        std::uint32_t queue_family{ 0 };    // Index of that queue's family.
        bool          validation{ false };  // Whether the validation layer is enabled.
    };

    /** @brief Gets the handles (see Handles). */
    [[nodiscard]] Handles handles() const noexcept;

    /**
     * @brief Gets how many content pipelines this device built over its lifetime.
     *
     * @details
     * - THE EVIDENCE FOR "A RESIZE MUST NOT RECOMPILE": a frame drawn again after an EXTENT change leaves this
     *   unmoved (the pipeline is compatible with the new framebuffer), while a shape change or a new program moves it.
     *   Like `handles()`, it is a learning-backend fact rather than a verb any other backend promises.
     *
     * @return Pipelines created since the device came up (0 when it built none).
     */
    [[nodiscard]] std::uint64_t contentPipelines() const noexcept;

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
     * @brief Brings up the window a `Probe` device presents through, once per session.
     *
     * @details
     * - Called from `initialize()` (the device's own window must exist before the surface is created on it) and from
     *   `createSurfaceTarget(nullptr)` when a host asks for the device's window first - the SDK's documented order.
     * - A window already up is a no-op: one Probe window per device, whatever asks for it first.
     *
     * @param why Receives the reason when no window came up, or null.
     * @return true when the device has a window (its handle is what a target is created from).
     */
    [[nodiscard]] bool ensureProbeWindow(vn::String* why);

    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
