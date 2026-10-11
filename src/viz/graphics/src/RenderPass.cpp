#include <vine/graphics/RenderPass.hpp>

#include <memory>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

VN_GRAPHICS_NS_BEGIN

VN_OBJECT_META_IMPL(RenderPass, vn::Object);

RenderPass::RenderPass() = default;

RenderPass::~RenderPass() = default;

String RenderPass::name() const
{
    return name_;
}

void RenderPass::setName(const String& name)
{
    name_ = name;
}

raw_ptr<RenderTarget> RenderPass::renderTarget() const
{
    return render_target_.get();
}

void RenderPass::setRenderTarget(intrusive_ptr<RenderTarget> target)
{
    render_target_ = std::move(target);
}

raw_ptr<Camera> RenderPass::camera() const
{
    return camera_.get();
}

void RenderPass::setCamera(intrusive_ptr<Camera> camera)
{
    camera_ = std::move(camera);
}

Color RenderPass::clearColor() const
{
    return clear_color_;
}

void RenderPass::setClearColor(const Color& color)
{
    clear_color_ = color;
}

bool RenderPass::shouldClearDepth() const
{
    return clear_depth_;
}

void RenderPass::setShouldClearDepth(bool clear)
{
    clear_depth_ = clear;
}

bool RenderPass::isClearEnabled() const
{
    return clear_enabled_;
}

void RenderPass::setClearEnabled(bool enabled)
{
    clear_enabled_ = enabled;
}

DepthMode RenderPass::depthMode() const
{
    return depth_mode_;
}

void RenderPass::setDepthMode(DepthMode mode)
{
    depth_mode_ = mode;
}

PassRenderState RenderPass::stateOverrides() const
{
    PassRenderState state;
    state.depth       = depth_mode_;
    state.polygonMode = polygon_mode_override_;
    return state;
}

bool RenderPass::isEnabled() const
{
    return enabled_;
}

void RenderPass::setEnabled(bool enabled)
{
    enabled_ = enabled;
}

void RenderPass::setViewport(int x, int y, int width, int height)
{
    setViewport(Viewport{ x, y, width, height });
}

void RenderPass::setViewport(const Viewport& viewport)
{
    viewport_ = viewport;
    has_viewport_ = true;
}

bool RenderPass::hasViewport() const
{
    return has_viewport_;
}

Viewport RenderPass::viewport() const
{
    return viewport_;
}

void RenderPass::clearViewport()
{
    has_viewport_ = false;
}

void RenderPass::setOutputName(const String& name)
{
    output_name_ = name;
}

String RenderPass::outputName() const
{
    return output_name_;
}

void RenderPass::setOutput(intrusive_ptr<ImageRef> image)
{
    output_image_ = std::move(image);
}

raw_ptr<ImageRef> RenderPass::output() const
{
    return output_image_.get();
}

void RenderPass::setOutputTarget(intrusive_ptr<RenderTarget> target)
{
    output_target_ = std::move(target);
}

raw_ptr<RenderTarget> RenderPass::outputTarget() const
{
    return output_target_.get();
}

void RenderPass::addInputName(const String& name)
{
    if (!name.empty()) {
        input_names_.push_back(name);
    }
}

const std::vector<String>& RenderPass::inputNames() const
{
    return input_names_;
}

void RenderPass::clearInputNames()
{
    input_names_.clear();
}

void RenderPass::addInput(intrusive_ptr<ImageRef> image)
{
    if (image != nullptr) {
        input_images_.push_back(std::move(image));
    }
}

const std::vector<intrusive_ptr<ImageRef>>& RenderPass::inputs() const
{
    return input_images_;
}

void RenderPass::addInputTarget(intrusive_ptr<RenderTarget> target)
{
    if (target != nullptr) {
        input_targets_.push_back(std::move(target));
    }
}

const std::vector<intrusive_ptr<RenderTarget>>& RenderPass::inputTargets() const
{
    return input_targets_;
}

void RenderPass::clearInputs()
{
    input_images_.clear();
    input_targets_.clear();
}


raw_ptr<ShaderProgram> RenderPass::programOverride() const
{
    return program_override_.get();
}

void RenderPass::setProgramOverride(intrusive_ptr<ShaderProgram> program)
{
    program_override_ = std::move(program);
}

std::optional<PolygonMode> RenderPass::polygonModeOverride() const
{
    return polygon_mode_override_;
}

void RenderPass::setPolygonModeOverride(PolygonMode mode)
{
    // Deliberately NOT a wiring change: rasterisation is not part of what this pass declares about its
    // target, inputs or camera (see the declaration), and the backend reads it per frame from the commands.
    polygon_mode_override_ = mode;
}

void RenderPass::clearPolygonModeOverride()
{
    polygon_mode_override_.reset();
}

std::optional<ClearPolicy> RenderPass::clearPolicy() const noexcept
{
    // The ONE place "does this pass clear, and how" is answered: a pass that does not clear
    // says nothing, so a backend can tell "clears to this colour" from "draws over what is
    // already there" — and cannot be told two different things by two call sites.
    if (!clear_enabled_) {
        return std::nullopt;
    }
    return ClearPolicy{ clear_color_, clear_depth_ };
}

void RenderPass::fillPassFacts(PassDescription& out)
{
    out.target   = render_target_.get();
    out.viewport = has_viewport_ ? std::optional<Viewport>(viewport_) : std::nullopt;
    out.clear    = clearPolicy();
    // This pass' state for the content it draws - its depth policy and its rasterisation override, in one
    // announcement: what a backend needs to resolve each command's FINAL state (StateNode.hpp's
    // resolveRenderState). Both are pass facts, not per-command rewrites: the pass states them once.
    out.state    = stateOverrides();
    out.draws.clear();
}

bool RenderPass::describe(raw_ptr<Scene> scene, PassDescription& out)
{
    if (scene == nullptr) {
        // A scene pass with no scene issues nothing AT ALL - not even a target: it used to return before the
        // first announcement, and a pass that is not drawing must not clear, stack or claim a target.
        return false;
    }
    fillPassFacts(out);
    if (camera_ == nullptr) {
        // No view to build: the pass announces what it is and where, and issues no draw. A draw without a camera
        // used to be a silent no-op at the wire; here it is simply absent.
        return true;
    }
    DrawDescription draw;
    draw.kind   = DrawDescription::Kind::Scene;
    draw.camera = camera_.get();
    // The shared list, not a copy of it: every pass of a frame draws the same commands, and the copy this
    // used to make cost an intrusive_ptr increment per command (three of them) plus a command-sized
    // memcpy per pass per frame. What a pass may change is its own VIEW of the list, so the copy happens
    // below — and only for a pass that actually rewrites something.
    const std::shared_ptr<const std::vector<RenderCommand>> commands =
        scene->collectRenderCommandsShared(camera_.get());
    // The pass lights whatever content scene it renders: forward the
    // scene's lights so the backend can match this pass's view lighting.
    // Announced even when there are none: "there are no lights" is what restores a backend's default.
    std::vector<raw_ptr<const Light>> light_ptrs;
    light_ptrs.reserve(scene->lights().size());
    for (const auto& light : scene->lights()) {
        light_ptrs.push_back(light.get());
    }
    draw.lights = std::move(light_ptrs);
    // A program override is the one pass-level override that still rewrites commands: replacing the program is not
    // "another state item" but "draw this content with another program", and the rewrite is what keeps a backend's
    // per-command pipeline key valid (see setProgramOverride). The rasterisation override is NOT rewritten any
    // more: it travels with the pass' state (stateOverrides()) and is resolved per command, in one place
    // (StateNode.hpp's resolveRenderState) - so a pass that overrides only rasterisation costs no copy at all.
    if (program_override_ == nullptr) {
        draw.commands = commands;
    }
    else {
        auto overridden = std::make_shared<std::vector<RenderCommand>>(*commands);
        for (auto& command : *overridden) {
            command.program = program_override_;
        }
        draw.commands = std::move(overridden);
    }
    out.draws.push_back(std::move(draw));
    return true;
}

void RenderPass::execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend,
                         raw_ptr<PresentTarget> present_to)
{
    if (backend == nullptr) {
        return;
    }
    // Describe, then hand the device's frame half the frame that says so - there is no interleaving and no second
    // wire: what this pass issues is a value (the description needs nothing from the backend - the picture's shape
    // is the camera's), and a pass a host drives itself is a frame with one pass in it (see singlePassFrame),
    // which is the same shape the engine builds for a whole frame. The target is what its default framebuffer row
    // resolves to (see RenderFrameExecutor::executeFrame); presenting it is the driver's own call
    // (WindowPresentTarget::present), exactly as it is for the engine.
    PassDescription description;
    if (!describe(scene, description)) {
        return;
    }
    PresentTarget* const target = present_to;
    backend->executor().executeFrame(singlePassFrame(this, /*order*/ 0, /*inputs*/ {}, std::move(description)),
                                    target);
    if (target != nullptr) {
        target->present();
    }
}

VN_GRAPHICS_NS_END
