#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

/**
 * @brief Builds the frame a case drives a backend with, one pass at a time (see RenderFrameExecutor::executeFrame).
 *
 * THE VALUE'S OWN SHAPE, SPELLED FOR A TEST. A case that used to drive the backend flat - open a pass, announce
 * its order, its target, its clear and its depth, draw, close, end the frame - names the same facts here and the
 * backend receives the finished value in one call. Nothing in this builder carries order semantics: `pass()`
 * starts a NEW entry, every setter fills that entry's description (see FrameDescription / PassDescription), and a
 * call before any `pass()` has no entry to fill at all - so the shapes the flat protocol had to forbid cannot be
 * written here either.
 *
 * A case that drives the ENGINE does not need this: the engine compiles the value itself (RenderEngine::frame).
 * This is for the cases that drive a backend directly (the SDK-facing backend's own tests, the plugin's).
 *
 * ```cpp
 *   backend->executeFrame(TestFrame().pass(pass.get()).target(nullptr).clear(clear).content(commands, camera));
 *   backend->present();
 * ```
 */
class TestFrame
{
  public:
    /** @brief Starts a new pass entry under @p pass' identity (the backend's per-pass state key).
     *
     * @param pass  The pass this entry names (borrowed, like every pointer in a frame).
     * @param order Where it stacks in its target (see FrameDescription::Pass::order).
     * @return This builder, for chaining.
     */
    TestFrame& pass(vn::raw_ptr<const vn::graphics::RenderPass> pass, int order = 0)
    {
        vn::graphics::FrameDescription::Pass entry;
        entry.pass  = pass;
        entry.order = order;
        frame_.passes.push_back(std::move(entry));
        return *this;
    }

    /** @brief Sets the open pass' order (see FrameDescription::Pass::order).
     *
     * @param order The pass' stacking order inside its target.
     * @return This builder, for chaining.
     */
    TestFrame& order(int order)
    {
        current().order = order;
        return *this;
    }

    /** @brief Sets the open pass' resolved inputs, in declaration order (see FrameDescription::Pass::inputs).
     *
     * @param inputs What the pass' declared inputs resolved to this frame.
     * @return This builder, for chaining.
     */
    TestFrame& inputs(std::vector<vn::raw_ptr<vn::graphics::RenderTarget>> inputs)
    {
        current().inputs = std::move(inputs);
        return *this;
    }

    /** @brief Sets the target the open pass draws into (null = the default framebuffer).
     *
     * @param target The pass' render target, or null for the window.
     * @return This builder, for chaining.
     */
    TestFrame& target(vn::graphics::RenderTarget* target)
    {
        description().target = target;
        return *this;
    }

    /** @brief Sets the sub-rectangle the open pass draws into (not calling it = the whole target).
     *
     * @param x      Viewport origin x in device pixels.
     * @param y      Viewport origin y in device pixels (top-left origin).
     * @param width  Viewport width in device pixels.
     * @param height Viewport height in device pixels.
     * @return This builder, for chaining.
     */
    TestFrame& viewport(int x, int y, int width, int height)
    {
        description().viewport = vn::graphics::Viewport{ x, y, width, height };
        return *this;
    }

    /** @brief Announces the clear of the open pass (not calling it leaves the target uncleared).
     *
     * @param policy The colour of attachment 0 and whether depth is cleared too (see ClearPolicy).
     * @return This builder, for chaining.
     */
    TestFrame& clear(const vn::graphics::ClearPolicy& policy)
    {
        description().clear = policy;
        return *this;
    }

    /** @brief Announces the open pass' depth policy for the content it draws (see PassRenderState::depth).
     *
     * @param depth How the pass' content handles the target's current depth.
     * @return This builder, for chaining.
     */
    TestFrame& depth(vn::graphics::DepthMode depth)
    {
        description().state.depth = depth;
        return *this;
    }

    /** @brief Adds one CONTENT drawing call to the open pass: its collected commands, through @p camera.
     *
     * @param commands The commands to draw (copied into the value, which owns what it carries).
     * @param camera   The view the draw is projected and lit with.
     * @return This builder, for chaining.
     */
    TestFrame& content(const std::vector<vn::graphics::RenderCommand>& commands,
                       const vn::graphics::Camera*                      camera)
    {
        vn::graphics::DrawDescription draw;
        draw.kind     = vn::graphics::DrawDescription::Kind::Scene;
        draw.camera   = camera;
        draw.commands = std::make_shared<const std::vector<vn::graphics::RenderCommand>>(commands);
        description().draws.push_back(std::move(draw));
        return *this;
    }

    /** @brief Adds one full-screen drawing call to the open pass: @p source's colour, through @p program.
     *
     * @param source  The target whose colour attachments are sampled.
     * @param program The fragment program to draw with.
     * @param camera  The view that transforms the pushed lights.
     * @return This builder, for chaining.
     */
    TestFrame& screen(vn::graphics::RenderTarget*                     source,
                      vn::raw_ptr<const vn::graphics::ShaderProgram> program,
                      const vn::graphics::Camera*                    camera)
    {
        vn::graphics::DrawDescription draw;
        draw.kind    = vn::graphics::DrawDescription::Kind::Screen;
        draw.camera  = camera;
        draw.source  = source;
        draw.program = std::move(program);
        description().draws.push_back(std::move(draw));
        return *this;
    }

    /** @brief Gets the frame as it stands, ready for RenderFrameExecutor::executeFrame.
     *
     * @return The value a backend is handed.
     */
    [[nodiscard]] const vn::graphics::FrameDescription& value() const noexcept { return frame_; }

  private:
    /** @brief Gets the entry a setter fills: the last `pass()`, starting one when none was opened.
     *
     * Starting one is not a shape a case should need - a value's facts BELONG to a pass - and the entry it makes
     * names NO pass, which a backend walks as nothing to run: a case that forgot `pass()` reads it immediately
     * (the frame does nothing) instead of getting a silently different picture.
     *
     * @return The open entry.
     */
    [[nodiscard]] vn::graphics::FrameDescription::Pass& current()
    {
        if (frame_.passes.empty()) {
            frame_.passes.push_back(vn::graphics::FrameDescription::Pass{});
        }
        return frame_.passes.back();
    }

    /** @brief Engages and gets the open pass' description (a pass that says nothing has none).
     *
     * @return The open entry's description.
     */
    [[nodiscard]] vn::graphics::PassDescription& description()
    {
        vn::graphics::FrameDescription::Pass& entry = current();
        if (!entry.description.has_value()) {
            entry.description.emplace();
        }
        return *entry.description;
    }

    vn::graphics::FrameDescription frame_;
};
