#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vine/graphics/RenderBackend.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The SDK-facing backend: the rewrite's session and frame drive behind `vine::graphics::RenderBackend`.
 *
 * WHERE THIS SITS (design §2.2). Every other `api/` type is one piece a frame is made of - the session, the
 * store, the assembly, the executor - and this is the seam the ENGINE talks to: it implements the SDK's
 * `RenderBackend` methods and drives those pieces through them, deciding nothing of its own. That is why it
 * is allowed to be thin: `initialize()` builds the session, a frame is `beginFrame()` -> `endFrame()` ->
 * `swapBuffers()` over the pieces that exist, and every call it cannot serve yet is REPORTED (the SDK's own
 * rule: report what could not be done instead of answering with nothing).
 *
 * WHAT IS SERVED TODAY (the facade's first slice):
 *   * the session lifecycle - up, down, up again, `shutdown()` idempotent and safe before any
 *     `initialize()`, exactly what the SDK's contract demands of a backend whose surface is recreated;
 *   * the frame protocol - an EMPTY frame is compiled, recorded and presented (the session's own rule:
 *     opening a frame may acquire an image, and the only call that hands it back is the present);
 *   * the surface facts - `setWindowHandle()`/`nativeHandle()` for a host surface, and `resize()`'s
 *     announcement, which is applied when the session comes up (the session's window is created at that
 *     size) and reported while the session is live (resizing a live surface is not served yet);
 *   * the diagnostics route - the core's one route feeds the SDK's `reportDiagnostic`, so a host's sink and
 *     `diagnosticCount()` see the session's own reports without this layer re-reporting anything.
 *
 * WHAT IS REPORTED AS NOT SERVED YET: the drawing half - passes, off-screen targets, content, full-screen
 * programs and readback. `supportsRenderTargets()` answers false, which is how the engine knows BEFORE it
 * stages off-screen work (the SDK asks once per frame, and declining is a state the host is told about),
 * and each call that has nowhere to go says so once (core::ReportOnce). A facade that silently dropped them
 * would look like a working backend with a black screen.
 *
 * ONE THREAD, LIKE EVERYTHING ELSE HERE: the engine drives a backend from one thread and never reentrantly
 * (see RenderBackend's class note), so this type keeps no synchronisation.
 */
V_VSG_NS_BEGIN

/** @brief The SDK's render backend, driven by the rewrite's session (see the file note). */
class V_VSG_API VsgBackend : public vine::graphics::RenderBackend
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

    /** @brief Announces a surface size: applied by the next @ref initialize, reported while live. */
    void resize(int width, int height) override;

    /** @brief Remembers the default content program (the drawing slices shade program-less content with it). */
    void setDefaultContentProgram(vine::intrusive_ptr<const vine::graphics::ShaderProgram> program) override;

    /** @brief Answers false: off-screen targets are a later slice, and the engine is told so here. */
    bool supportsRenderTargets() override;

    /** @brief Accepts the default framebuffer; an off-screen target is reported as not served yet. */
    void setRenderTarget(vine::raw_ptr<vine::graphics::RenderTarget> target) override;

    /** @brief Reports that the pass protocol is not served yet (see the file note). */
    void beginPass(vine::raw_ptr<const vine::graphics::RenderPass> pass) override;

    /** @brief Reports that the pass protocol is not served yet (see the file note). */
    void endPass() override;

    /** @brief Reports that the pass protocol is not served yet (see the file note). */
    void setPassOrder(int order) override;

    /** @brief Reports that per-draw viewports are not served yet (see the file note). */
    void setViewport(int x, int y, int width, int height) override;

    /** @brief Reports that lights are not served yet (see the file note). */
    void setLights(const std::vector<vine::raw_ptr<const vine::graphics::Light>>& lights) override;

    /** @brief Reports that pass inputs are not served yet (see the file note). */
    void setPassInputs(const std::vector<vine::raw_ptr<vine::graphics::RenderTarget>>& inputs) override;

    /** @brief Reports that the pass' depth policy is not served yet (see the file note). */
    void setDepthMode(vine::graphics::DepthMode mode) override;

    /** @brief Reports that clear policies are not served yet (see the file note). */
    void setClearPolicy(const vine::graphics::ClearPolicy& policy) override;

    /** @brief Reports that content drawing is not served yet (see the file note). */
    void render(const std::vector<vine::graphics::RenderCommand>& commands,
                const vine::graphics::Camera*                      camera) override;

    /** @brief Reports that full-screen programs are not served yet (see the file note). */
    void drawScreenProgram(vine::graphics::RenderTarget*                     source,
                           vine::raw_ptr<const vine::graphics::ShaderProgram> program,
                           vine::raw_ptr<const vine::graphics::Camera>        camera) override;

    /** @brief Reports that pass retirement is not served yet (see the file note). */
    void releasePass(vine::raw_ptr<const vine::graphics::RenderPass> pass) override;

    /** @brief Reports that off-screen target retirement is not served yet (see the file note). */
    void releaseRenderTarget(vine::graphics::RenderTarget* target) override;

    /** @brief Reports that readback is not served yet, and answers `Unsupported` (the file note). */
    bool readColorBuffer(const vine::graphics::RenderTarget* target, int attachment,
                         std::vector<std::uint8_t>& outPixels, vine::graphics::ReadbackResult* why = nullptr) override;

    /** @brief Reports that readback is not served yet, and answers `Unsupported` (the file note). */
    bool readDepthBuffer(const vine::graphics::RenderTarget* target, std::vector<float>& outDepths,
                         vine::graphics::ReadbackResult* why = nullptr) override;

  public:
    /** @brief Gets whether the session is up. */
    [[nodiscard]] bool initialized() const noexcept;

    /** @brief Gets how many frames the session has presented. */
    [[nodiscard]] std::uint64_t framesPresented() const noexcept;

    /** @brief Gets how many times this backend has waited the device idle. */
    [[nodiscard]] std::size_t deviceWaits() const noexcept;

  private:
    /** @brief Reports one unserved entry point, once per episode (see the file note). */
    void reportUnserved(std::size_t slot) noexcept;

  private:
    struct Data;
    std::unique_ptr<Data> d;
};

V_VSG_NS_END
