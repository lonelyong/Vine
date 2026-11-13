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
 * - WHAT IT DELIBERATELY DOES NOT DO YET. No window (M5), no material/light/texture blocks and no full-screen draws
 *   (M4), no residency accounting (M6). M3 landed the target plan (`planTarget`: a resize keeps the render passes and
 *   every pipeline, a shape change rebuilds them), MRT with per-attachment clears, the depth band, the depth-LOAD
 *   variant and the pass' resolved render state. It answers `capabilities()` from what it can really serve -
 *   `render_targets == true`, `presentation == false`.
 * - FRAME AND HOST VERBS. `executeFrame()` runs the off-screen passes of the frame value and records the draws of each
 *   `DrawDescription`; a pass drawing into the window, or a draw naming no program, is reported and left out. The
 *   readbacks answer the targets this device built.
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

    /** @brief Gets what this device can serve TODAY (see Capabilities): M1 serves off-screen colour targets. */
    [[nodiscard]] vn::graphics::Capabilities capabilities() const noexcept override;

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
     * @brief Runs the frame's passes that draw into off-screen targets: each one clears its target, then records the
     *        drawing calls of every `DrawDescription` it carries.
     *
     * @details
 * - M3's whole frame path. A pass that names a target AND states a clear policy is recorded as one render pass
 *   (colour attachments CLEAR, depth CLEAR or LOAD per `ClearPolicy::depth`); a pass with no target (the window, which
 *   M5 brings up), a pass with no clear at all (which would need a colour LOAD variant) and a full-screen draw (M4)
 *   are reported once and left out.
     * - Each scene draw is projected with its own camera snapshot and shaded with the resolved program (the pass'
     *   override, else the command's own, else the default content program). One `RenderCommand` is one indexed draw.
     * - The submission is synchronous by design: one command buffer, one fence, then the queue is idle, so a readback in
     *   the same breath sees exactly what this frame left.
     *
     * @param frame      The frame to execute.
     * @param present_to Target the default framebuffer resolves to (ignored: M1 presents nothing).
     * @return `PresentOutcome::Skipped` - this device still presents nothing.
     */
    [[nodiscard]] vn::graphics::PresentOutcome executeFrame(const vn::graphics::FrameDescription& frame,
                                                           vn::graphics::PresentTarget*          present_to) override;

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

    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
