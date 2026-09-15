#include <vine/graphics/RenderPass.hpp>

#include <memory>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

V_GRAPHICS_NS_BEGIN

V_OBJECT_META_IMPL(RenderPass, vine::Object);

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
    bumpWiringRevision();
}

raw_ptr<Camera> RenderPass::camera() const
{
    return camera_.get();
}

void RenderPass::setCamera(raw_ptr<Camera> camera)
{
    camera_ = camera;
    bumpWiringRevision();
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

bool RenderPass::clearEnabled() const
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

bool RenderPass::occlusionEnabled() const
{
    return depth_mode_ != DepthMode::Disabled;
}

void RenderPass::setOcclusionEnabled(bool enabled)
{
    depth_mode_ = enabled ? DepthMode::TestAndWrite : DepthMode::Disabled;
}

bool RenderPass::enabled() const
{
    return enabled_;
}

void RenderPass::setEnabled(bool enabled)
{
    enabled_ = enabled;
    bumpWiringRevision();
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
    bumpWiringRevision();
}

String RenderPass::outputName() const
{
    return output_name_;
}

void RenderPass::setOutput(intrusive_ptr<ImageRef> image)
{
    output_image_ = std::move(image);
    bumpWiringRevision();
}

raw_ptr<ImageRef> RenderPass::output() const
{
    return output_image_.get();
}

void RenderPass::setOutputTarget(intrusive_ptr<RenderTarget> target)
{
    output_target_ = std::move(target);
    bumpWiringRevision();
}

raw_ptr<RenderTarget> RenderPass::outputTarget() const
{
    return output_target_.get();
}

void RenderPass::addInputName(const String& name)
{
    if (!name.empty()) {
        input_names_.push_back(name);
        bumpWiringRevision();
    }
}

const std::vector<String>& RenderPass::inputNames() const
{
    return input_names_;
}

void RenderPass::clearInputNames()
{
    input_names_.clear();
    bumpWiringRevision();
}

void RenderPass::addInput(intrusive_ptr<ImageRef> image)
{
    if (image != nullptr) {
        input_images_.push_back(std::move(image));
        bumpWiringRevision();
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
        bumpWiringRevision();
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
    bumpWiringRevision();
}

raw_ptr<ShaderProgram> RenderPass::programOverride() const
{
    return program_override_.get();
}

void RenderPass::setProgramOverride(intrusive_ptr<ShaderProgram> program)
{
    program_override_ = std::move(program);
    bumpWiringRevision();
}

std::uint64_t RenderPass::wiringRevision() const noexcept
{
    return wiring_revision_;
}

void RenderPass::bumpWiringRevision() noexcept
{
    ++wiring_revision_;
}

void RenderPass::execute(raw_ptr<Scene> scene, raw_ptr<RenderBackend> backend)
{
    if (backend == nullptr || scene == nullptr) {
        return;
    }
    backend->setRenderTarget(render_target_.get());
    if (has_viewport_) {
        backend->setViewport(viewport_.x, viewport_.y, viewport_.width, viewport_.height);
    }
    if (clear_enabled_) {
        backend->clear(clear_color_, clear_depth_);
    }
    // Depth handling is explicit (DepthMode), never inferred from the clear
    // flag: depth test/write are independent of whether the pass clears and of
    // how it is lit (the content scene decides the lights). Forwarded so the
    // backend's render() picks the right content-slot depth state.
    backend->setDepthMode(depth_mode_);
    // The shared list, not a copy of it: every pass of a frame draws the same commands, and the copy this
    // used to make cost an intrusive_ptr increment per command (three of them) plus a command-sized
    // memcpy per pass per frame. What a pass may change is its own VIEW of the list, so the copy happens
    // below — and only for a pass that actually rewrites something.
    const std::shared_ptr<const std::vector<RenderCommand>> commands = scene->collectRenderCommandsShared(camera_.get());
    if (camera_ != nullptr) {
        // The pass lights whatever content scene it renders: forward the
        // scene's lights so the backend can match this pass's view lighting.
        std::vector<raw_ptr<const Light>> light_ptrs;
        light_ptrs.reserve(scene->lights().size());
        for (const auto& light : scene->lights()) {
            light_ptrs.push_back(light.get());
        }
        backend->setLights(light_ptrs);
    }
    if (program_override_ == nullptr) {
        if (camera_ != nullptr) {
            backend->render(*commands, camera_.get());
        }
        return;
    }
    // Pass-level global program override: replace every command's effective
    // (per-geometry / StateNode) program so the whole content renders with one
    // program (see setProgramOverride). This is the one caller that needs its own
    // list, so it is the one that makes one.
    auto overridden = std::make_shared<std::vector<RenderCommand>>(*commands);
    for (auto& command : *overridden) {
        command.program = program_override_;
    }
    if (camera_ != nullptr) {
        backend->render(*overridden, camera_.get());
    }
}

V_GRAPHICS_NS_END
