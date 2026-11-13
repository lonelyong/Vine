#pragma once

#include <vine/graphics/RenderBackend.hpp>
#include <vine/backend_vk/backend_vk_global.hpp>

VN_BACKEND_VK_NS_BEGIN

/**
 * @brief The direct-Vulkan backend's device (milestone M0: lifecycle and honest capabilities).
 *
 * @details
 * - WHAT IT IS. A `RenderBackend` implemented directly on Vulkan through volk: it owns one `VkInstance`, one
 *   `VkPhysicalDevice`, one `VkDevice` and one graphics queue, and NOTHING else yet. M0 exists so the seam is proved
 *   before anything is drawn: registry -> factory -> device -> `initialize()` / `shutdown()`, with the validation
 *   layer's messages arriving on the host's diagnostic route (see `.ai/research/graphics-2026-11-13-gpu-resource-
 *   abstraction.md` §8.3).
 * - WHAT IT DELIBERATELY DOES NOT DO YET. No render targets (M1), no content draws (M2), no window (M5). It answers
 *   `capabilities()` from what it can really serve - `render_targets == false`, `presentation == false` - instead of
 *   claiming the device's Vulkan abilities, because a host plans frames against that answer.
 * - FRAME AND HOST VERBS. `executeFrame()` runs nothing and says so once; `setRenderTarget()` refuses to promise
 *   anything; the readbacks keep the SDK's `Unsupported` answer. A host that drives this backend today gets a device
 *   that comes up, reports, and draws nothing - which is the honest M0 picture.
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
     * @brief Remembers the target a host announced; M0 builds no attachments for it.
     *
     * @param target Target the host will draw into, or null for the default framebuffer.
     */
    void setRenderTarget(vn::graphics::RenderTarget* target) override;

    /** @brief Gets what this device can serve TODAY (see Capabilities): M0 serves no target and no window. */
    [[nodiscard]] vn::graphics::Capabilities capabilities() const noexcept override;

    /**
     * @brief Runs nothing: M0 has no pass recording, so a frame is answered `Skipped` and reported once.
     *
     * @param frame      The frame to execute (ignored).
     * @param present_to Target the frame's default framebuffer resolves to (ignored).
     * @return `PresentOutcome::Skipped` - this device presents nothing yet.
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
    struct Impl;

    std::unique_ptr<Impl> impl;
};

VN_BACKEND_VK_NS_END
