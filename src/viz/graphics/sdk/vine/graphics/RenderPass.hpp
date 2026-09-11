#pragma once
#include "graphics_global.hpp"

#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/RefCounted.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/Color.hpp>

#include "DepthMode.hpp"
#include "Viewport.hpp"

V_GRAPHICS_NS_BEGIN

class Camera;
class RenderTarget;
class RenderBackend;
class Scene;
class ShaderProgram;

using ShaderProgramPtr = intrusive_ptr<ShaderProgram>;

/**
 * @brief A render pass describing one complete rendering stage.
 *
 * Binds a camera, render target, and clear state. Executing a pass
 * collects render commands from a scene and dispatches them to a backend.
 * Multiple passes can be chained for split-screen, post-processing, etc.
 *
 * Naming: this is an engine-level rendering STAGE (the equivalent of an
 * OSG render stage / a VulkanSceneGraph view + target + clear policy), NOT a
 * Vulkan render pass. Several passes targeting the same framebuffer are
 * recorded into one backend render pass; the backend owns that distinction.
 */
class V_GRAPHICS_API RenderPass : public Object, public RefCounted<RenderPass> {
    V_OBJECT_META_DECL;

  public:
    RenderPass();
    ~RenderPass();

  public:
    /** @brief Gets the pass name. */
    String name() const;

    /** @brief Sets the pass name. */
    void setName(const String& name);

    /** @brief Gets the associated render target. */
    raw_ptr<RenderTarget> renderTarget() const;

    /** @brief Sets the render target this pass renders into.
     *
     * The pass keeps a reference to the target (its output resource), so the
     * target stays alive as long as the pass uses it.
     *
     * @param target Render target to render into, or null for the default
     *               framebuffer.
     */
    void setRenderTarget(intrusive_ptr<RenderTarget> target);

    /** @brief Gets the camera used by this pass. */
    raw_ptr<Camera> camera() const;

    /** @brief Sets the camera used by this pass. */
    void setCamera(raw_ptr<Camera> camera);

    /** @brief Gets the clear color. */
    Color clearColor() const;

    /** @brief Sets the clear color. */
    void setClearColor(const Color& color);

    /** @brief Returns whether the depth buffer is cleared. */
    bool shouldClearDepth() const;

    /** @brief Sets whether the depth buffer is cleared before this pass.
     *
     * Controls the clearDepth argument passed to RenderBackend::clear() when
     * this pass clears (see setClearEnabled). Whether a false value actually
     * preserves the previous depth depends on the target: it is honoured for
     * off-screen targets (their depth survives via a depth-LOAD pass), but a
     * pass rendering into the window / main surface always gets a cleared
     * depth buffer (the surface render pass clears depth), so the flag is
     * ignored there.
     *
     * @param clear True to clear the depth buffer (the default).
     */
    void setShouldClearDepth(bool clear);

    /** @brief Returns whether the colour/depth buffer is cleared before this pass.
     *
     * The main pass clears by default; top / HUD passes usually disable it so
     * they draw over the previous frame's content.
     */
    bool clearEnabled() const;

    /** @brief Sets whether the colour/depth buffer is cleared before this pass.
     *
     * @param enabled True to clear the buffers (the default).
     */
    void setClearEnabled(bool enabled);

    /** @brief Returns how this pass's content handles depth.
     *
     * @return The depth mode (DepthMode::TestAndWrite by default).
     */
    DepthMode depthMode() const;

    /** @brief Sets how this pass's content handles depth.
     *
     * TestAndWrite (the default) draws the content as depth-occluded opaque
     * scene content; TestOnly tests against the target's current depth but
     * does not write it (translucent content composited over already-written
     * depth); Disabled draws on top of whatever is already in the target with
     * no depth testing (HUD / overlay style). Independent of clearing and of
     * lighting. A translucent pass drawn into a target whose depth an earlier
     * pass wrote uses TestOnly and disables only the clear.
     *
     * @param mode The depth mode.
     */
    void setDepthMode(DepthMode mode);

    /** @brief Returns whether this pass's content is occluded by (tests
     * against) the target's current depth.
     *
     * Convenience for depthMode() != DepthMode::Disabled.
     *
     * @return True when depth testing is on.
     */
    bool occlusionEnabled() const;

    /** @brief Convenience: TestAndWrite when enabled, Disabled when not.
     *
     * Use setDepthMode for the finer-grained translucent (TestOnly) case.
     *
     * @param enabled True for depth-tested scene content.
     */
    void setOcclusionEnabled(bool enabled);

    /** @brief Returns whether this pass is drawn by the engine this frame. */
    bool enabled() const;

    /** @brief Sets whether this pass is drawn by the engine.
     *
     * The engine skips disabled passes, which lets a registered pass (e.g. a
     * HUD overlay) be toggled on and off without removing it. The default is
     * true.
     *
     * @param enabled True to draw the pass (the default).
     */
    void setEnabled(bool enabled);

    /** @brief Restricts this pass to a sub-rectangle of the render target.
     *
     * Used for sub-viewports such as an axis gizmo in a screen corner. The
     * pass falls back to the full surface when no viewport is set.
     *
     * @param viewport Draw rectangle in device pixels (top-left origin).
     */
    void setViewport(const Viewport& viewport);

    /** @brief Restricts this pass to a sub-rectangle of the render target.
     *
     * Convenience for setViewport(const Viewport&).
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     */
    void setViewport(int x, int y, int width, int height);

    /** @brief Returns whether a sub-viewport is configured for this pass. */
    bool hasViewport() const;

    /** @brief Gets the configured sub-viewport in device pixels.
     *
     * Values are only meaningful when hasViewport() is true.
     *
     * @param x      Receives the viewport origin x.
     * @param y      Receives the viewport origin y.
     * @param width  Receives the viewport width.
     * @param height Receives the viewport height.
     */
    void getViewport(int& x, int& y, int& width, int& height) const;

    /** @brief Gets the configured draw viewport.
     *
     * Only meaningful when hasViewport() is true; otherwise the pass draws
     * the full surface.
     *
     * @return The draw rectangle in device pixels.
     */
    Viewport viewport() const;

    /** @brief Clears any configured sub-viewport (pass renders to the full surface). */
    void clearViewport();

    /** @brief Sets the name this pass publishes its output under.
     *
     * When a pass renders into a non-null RenderTarget and declares an output
     * name, the engine registers that target in its named-output registry
     * after the pass runs, so later passes can sample it by name without
     * holding a pointer to the producer. Leave empty to publish nothing.
     *
     * @param name Output slot name, or empty to disable publishing.
     */
    void setOutputName(const String& name);

    /** @brief Gets the output slot name this pass publishes under.
     *
     * @return The output name (empty when publishing is disabled).
     */
    String outputName() const;

    /** @brief Adds an input texture slot this pass consumes by name.
     *
     * A consumer declares "I want the texture published as X" without
     * holding a pointer to the producer. The engine resolves each declared
     * name against its named-output registry before execute() and hands the
     * matching targets to resolveInputTextures().
     *
     * The producer must run EARLIER in the same frame (a lower pass order, or
     * an earlier registration at the same order): the registry is cleared at the
     * start of every frame. Several names may be declared as alternatives — the
     * first that resolves is used (a chain that falls back). When NONE of them
     * resolves, the pass draws nothing; the engine reports that on the host's
     * diagnostic channel, because a wiring mistake used to be silent.
     *
     * @param name Name of a published output to consume.
     */
    void addInputName(const String& name);

    /** @brief Gets the declared input texture slot names.
     *
     * @return The input names, in the order they were added.
     */
    const std::vector<String>& inputNames() const;

    /** @brief Clears all declared input texture slots. */
    void clearInputNames();

    /** @brief Receives the engine-resolved input textures.
     *
     * Called by the engine just before execute() once per frame with the
     * targets resolved from the named-output registry for each declared input
     * (same order as inputNames(); entries stay null when a name is not yet
     * published). The base pass ignores the inputs; subclasses such as
     * ScreenPass consume them. The passed targets are borrowed: the registry
     * keeps them alive while they are published.
     *
     * @param inputs Resolved input targets in inputNames() order.
     */
    virtual void resolveInputTextures(const std::vector<raw_ptr<RenderTarget>>& inputs)
    {
        (void)inputs;
    }

    /** @brief Gets the pass-level program override (null when unset). */
    raw_ptr<ShaderProgram> programOverride() const;

    /** @brief Forces every command this pass renders to use one program.
     *
     * By default each geometry renders with its own effective program (leaf /
     * StateNode resolution). Setting an override replaces the program of every
     * collected command, so the same content scene can be re-rendered with a
     * different shader (e.g. a wireframe or alternate-shading pass over the
     * same scene). The pass is its own retained slot in the backend (keyed by
     * the pass, see RenderBackend::beginPass), so an override and the
     * per-geometry pass never collapse into one slot; use setViewport /
     * setClearEnabled to control where each one lands.
     * The pass keeps a reference.
     *
     * @param program Program applied to all content, or null for per-geometry
     *                programs (the default).
     */
    void setProgramOverride(intrusive_ptr<ShaderProgram> program);

    /** @brief Executes this render pass.
     *
     * @param scene   Scene containing drawables.
     * @param backend Backend to render with.
     */
    virtual void execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend);

  private:
    String name_;
    String output_name_;
    std::vector<String> input_names_;
    ShaderProgramPtr program_override_;   // null = per-geometry programs
    intrusive_ptr<RenderTarget> render_target_;
    raw_ptr<Camera> camera_ = nullptr;
    Color clear_color_{ 51, 51, 51, 255 };
    bool clear_depth_ = true;
    bool clear_enabled_ = true;
    DepthMode depth_mode_ = DepthMode::TestAndWrite;
    bool enabled_ = true;
    bool has_viewport_ = false;
    Viewport viewport_;
};

using RenderPassPtr = intrusive_ptr<RenderPass>;

V_GRAPHICS_NS_END
