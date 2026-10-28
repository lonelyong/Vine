#include <vine/graphics/backend/FrameRecorder.hpp>

#include <cmath>
#include <cstddef>

VN_GRAPHICSBACKEND_NS_BEGIN

using vn::graphics::FrameCounters;

namespace
{

/// @brief Pairs a borrowed program with the revision it is at.
ProgramRef programRef(const vn::graphics::ShaderProgram* program) noexcept
{
    ProgramRef ref;
    if (program != nullptr) {
        ref.program  = program;
        ref.revision = program->revision();
    }
    return ref;
}

} // namespace

FrameRecorder::FrameRecorder(FrameArena& arena, Diagnostics& diagnostics, Observe& observe) noexcept
  : arena_(arena)
  , diagnostics_(diagnostics)
  , observe_(observe)
{
    // Working memory for one frame's collection. Reserved once, so a steady frame's collection does not
    // allocate: a scene with more passes or more drawing calls than this grows on its first frames and
    // keeps the capacity from then on (see the design's "steady frame allocates nothing" gate).
    passes_.reserve(16);
    open_draws_.reserve(64);
}

bool FrameRecorder::beginFrame(FrameToken token)
{
    if (frame_open_) {
        return false; // one frame at a time: nothing may be collected into two
    }
    frame_open_ = true;

    // The frame boundary is the ONE moment a span handed out earlier may die (see FrameArena).
    arena_.reset();
    token_                 = token;
    pass_open_             = false;
    scope_target_          = nullptr;
    scope_target_released_ = false;
    pass_inputs_           = {};
    pending_has_viewport_  = false;
    pending_viewport_      = {};
    pending_lights_        = {};
    open_draws_.clear();
    passes_.clear();

    description_                 = CollectedFrame{};
    description_.token           = token;
    description_.default_program = default_program_;
    return true;
}

bool FrameRecorder::endFrame()
{
    if (!frame_open_ || pass_open_) {
        return false; // nothing to close, or a scope still open
    }

    // The pass records are published in one arena array: the compiler reads them after this frame's calls
    // are over, and the caller's own containers are gone by then.
    const std::span<CollectedPass> published = arena_.makeArray<CollectedPass>(passes_.size());
    for (std::size_t i = 0; i < passes_.size(); ++i) {
        published[i] = passes_[i];
    }
    description_.passes = published;
    return true;
}

bool FrameRecorder::swapBuffers()
{
    if (!frame_open_ || pass_open_) {
        return false; // the present closes a committed frame, and never an open scope
    }
    frame_open_ = false;
    return true;
}

bool FrameRecorder::beginPass(PassId pass)
{
    if (!frame_open_ || pass_open_) {
        return false; // no frame to belong to, or a scope is already open (the outer one keeps its state)
    }
    pass_open_ = true;

    pass_open_            = true;
    pass_id_              = pass;
    order_                = 0;
    pass_target_          = nullptr;
    pass_has_clear_       = false;
    pass_clear_           = ClearValues{};
    pass_state_           = vn::graphics::PassRenderState{};
    pass_depth_range_     = vn::graphics::DepthRange{};
    pass_inputs_          = {};
    pending_has_viewport_ = false;
    pending_viewport_     = {};
    pending_lights_       = {};
    open_draws_.clear();
    return true;
}

bool FrameRecorder::endPass()
{
    if (!pass_open_) {
        return false; // nothing to close
    }

    // A scope enters the plan when it can have an effect: it drew something, or it announced a clear (a
    // clear is applied by the pass' own load-op, so a clear-only pass is a real pass with no draws). Every
    // other attribute of a scope that drew nothing is dropped here, which is what stops it from leaking
    // into the next pass.
    if (!open_draws_.empty() || pass_has_clear_) {
        CollectedPass record;
        record.pass        = pass_id_;
        record.order       = order_;
        record.target      = pass_target_;
        record.inputs      = pass_inputs_;
        record.has_clear   = pass_has_clear_;
        record.clear       = pass_clear_;
        record.state       = pass_state_;
        record.depth_range = pass_depth_range_;

        const std::span<CollectedDraw> draws = arena_.makeArray<CollectedDraw>(open_draws_.size());
        for (std::size_t i = 0; i < open_draws_.size(); ++i) {
            draws[i] = open_draws_[i];
        }
        record.draws = draws;

        passes_.push_back(record);
        ++observe_.counters().passes;
    }

    pass_open_             = false;
    scope_target_          = nullptr;
    scope_target_released_ = false;
    open_draws_.clear();
    pass_inputs_ = {};
    return true;
}

bool FrameRecorder::setPassOrder(int order)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    order_ = order;
    return true;
}

bool FrameRecorder::setRenderTarget(const void* target)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }

    pass_target_           = target;
    scope_target_          = target;
    scope_target_released_ = false; // a new announcement is the target the scope draws into now (see the file note)
    return true;
}

bool FrameRecorder::setViewport(int x, int y, int width, int height)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    pending_has_viewport_ = true;
    pending_viewport_     = vn::graphics::Viewport{ x, y, width, height };
    return true;
}

bool FrameRecorder::setDepthRange(const vn::graphics::DepthRange& range)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    pass_depth_range_ = range;
    return true;
}

bool FrameRecorder::setClearPolicy(const ClearValues& policy)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    pass_has_clear_ = true;
    pass_clear_     = policy;
    return true;
}

bool FrameRecorder::setPassState(const vn::graphics::PassRenderState& state)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    pass_state_ = state;
    return true;
}

bool FrameRecorder::setPassInputs(std::span<const vn::graphics::ImageSource> inputs)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }

    // COPIED, not held: the engine's resolved-input vector is a reused member that the next pass refills
    // (RenderEngine::resolvePassInputs), so holding a span into it would read the next pass' inputs.
    const std::span<InputRef> copy = arena_.makeArray<InputRef>(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        copy[i].source = inputs[i];
    }
    pass_inputs_ = copy;
    return true;
}

bool FrameRecorder::setPassInputs(std::span<const vn::graphics::RenderTarget* const> inputs)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }

    const std::span<InputRef> copy = arena_.makeArray<InputRef>(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        // This convenience spelling receives pointers the caller only READS; ImageSource names the target as
        // mutable because a source may also be a producer's handle. Nothing here writes through it.
        copy[i].source =
            inputs[i] != nullptr ? vn::graphics::ImageSource::producedTarget(const_cast<vn::graphics::RenderTarget*>(inputs[i])) : vn::graphics::ImageSource{};
    }
    pass_inputs_ = copy;
    return true;
}

bool FrameRecorder::setLights(std::span<const vn::graphics::Light* const> lights)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to fill, or the scope's target is gone (see the file note)
    }
    snapshotLights(lights);
    return true;
}

bool FrameRecorder::render(std::span<const vn::graphics::RenderCommand> commands,
                           const vn::graphics::Camera*                  camera,
                           const vn::graphics::ShaderProgram*           program_override)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to draw in, or its target is gone: nothing is redirected
    }

    // A CALL WITH NOTHING TO DRAW IS NOT A DRAW. The engine really announces one, and it is not exotic: the
    // demo's axis-gizmo pass calls render() with an empty list while its surface size is still unknown
    // (measured 2026-09-24). Recording that as a draw put a content draw with NO commands into the plan, and
    // the content layer read it as "this pass has content, but no compiled content half was built for it" -
    // it refused the WHOLE pass, the clear the pass carries included, and reported a warning (the application
    // log's last one). The call is still a call: what it announced is consumed below, exactly as it is for a
    // call that draws, so a host's announcement never leaks into the next call - only the DRAW is not
    // recorded, because there is nothing in it (see FrameCounters::draws: "draw calls recorded").
    if (!commands.empty()) {
        CollectedDraw draw;
        draw.kind         = DrawKind::Content;
        draw.camera       = snapshotCamera(camera);
        draw.has_viewport = pending_has_viewport_;
        draw.viewport     = pending_viewport_;
        draw.lights       = pending_lights_;
        draw.commands     = snapshotCommands(commands, program_override);
        open_draws_.push_back(draw);
        ++observe_.counters().draws;
    }

    // ONE announcement serves ONE drawing call: the next draw of this scope that wants them announces
    // again (see the file note). The pass-level attributes stay, because they belong to the pass.
    pending_has_viewport_ = false;
    pending_viewport_     = {};
    pending_lights_       = {};

    return true;
}

bool FrameRecorder::drawScreenProgram(const void* source, const vn::graphics::ShaderProgram* program, const vn::graphics::Camera* camera)
{
    if (!pass_open_ || scope_target_released_) {
        return false; // no scope to draw in, or its target is gone: nothing is redirected
    }

    CollectedDraw draw;
    draw.kind         = DrawKind::Screen;
    draw.source       = source;
    draw.program      = programRef(program);
    draw.camera       = snapshotCamera(camera);
    draw.has_viewport = pending_has_viewport_;
    draw.viewport     = pending_viewport_;
    draw.lights       = pending_lights_;
    open_draws_.push_back(draw);

    pending_has_viewport_ = false;
    pending_viewport_     = {};
    pending_lights_       = {};

    ++observe_.counters().draws;
    return true;
}

bool FrameRecorder::setDefaultContentProgram(const vn::graphics::ShaderProgram* program)
{
    // Frame-level, and legal in every state (see the file note). The setting outlives the frame it was made
    // in, so it is kept next to the token.
    default_program_             = programRef(program);
    description_.default_program = default_program_;
    return true;
}

bool FrameRecorder::releaseRenderTarget(const void* target)
{
    // Always legal, whenever it arrives (the contract says so): the caller is telling the backend a target is
    // going away. An open scope that announced it may not keep that pointer, so the announcement is dropped
    // here and the scope is done - its remaining effects are dropped and its draws are skipped rather than
    // redirected to the default framebuffer (see the file note).
    if (pass_open_ && scope_target_ == target) {
        scope_target_          = nullptr;
        scope_target_released_ = true;
    }
    return true;
}

const CollectedFrame& FrameRecorder::description() const noexcept
{
    return description_;
}

bool FrameRecorder::inFrame() const noexcept
{
    return frame_open_;
}

bool FrameRecorder::inPass() const noexcept
{
    return pass_open_;
}

void FrameRecorder::snapshotLights(std::span<const vn::graphics::Light* const> lights)
{
    const std::span<LightRef> copy = arena_.makeArray<LightRef>(lights.size());
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const vn::graphics::Light* light = lights[i];
        if (light == nullptr) {
            continue; // a null entry stays the default "disabled" light: the array keeps its positions
        }
        LightRef& ref     = copy[i];
        ref.identity      = light;
        ref.enabled       = light->isEnabled();
        ref.type          = light->type();
        ref.color         = light->color();
        ref.intensity     = light->intensity();
        ref.has_direction = light->hasDirection();
        ref.cast_shadow   = light->isCastingShadow();
        ref.shadow_bias   = light->shadowSettings().bias;
        if (ref.has_direction) {
            ref.direction = light->direction();
        }
        ref.has_position       = light->hasPosition();
        ref.range              = light->range();
        ref.inner_cone_degrees = light->innerConeAngle();
        ref.outer_cone_degrees = light->outerConeAngle();
        if (ref.has_position) {
            ref.position = light->position();
        }
    }
    pending_lights_ = copy;
}

std::span<const CollectedCommand> FrameRecorder::snapshotCommands(std::span<const vn::graphics::RenderCommand> commands,
                                                                  const vn::graphics::ShaderProgram*           program_override)
{
    const std::span<CollectedCommand> copy = arena_.makeArray<CollectedCommand>(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const vn::graphics::RenderCommand& source = commands[i];
        CollectedCommand&                  target = copy[i];

        target.geometry          = source.geometry.get();
        target.geometry_revision = source.geometry != nullptr ? source.geometry->revision() : 0;
        target.primitive         = source.primitive_index;
        // The primitive's topology travels with the command: it is the data's word (the pass may still override
        // it - see resolveRenderState), and a command whose primitive the geometry no longer holds keeps the
        // documented default, which draws nothing because the facts cannot answer for it.
        if (const vn::graphics::Primitive* primitive = source.primitive()) {
            target.topology = primitive->topology;
        }
        // The pass' override wins over the command's own program (see render()): the identity the plan keys its
        // pipeline variants by is therefore the EFFECTIVE one, and nothing downstream needs to know about overrides.
        target.program        = programRef(program_override != nullptr ? program_override : source.program.get());
        target.material       = source.material.get();
        target.model          = source.model_matrix;
        target.opacity        = source.opacity;
        // The transparency decision is made ONCE, here, from the command's own rule (`isTransparent`: the
        // material's resolved alpha mode and the drawable's opacity). The plan carries the answer because
        // compilation has only the identity, and the blend enable has to agree with the order the collector
        // sorted by - two spellings of "is this see-through" is exactly the drift that made a Blend material sort
        // as transparent and draw unblended (graphics-material.md 2.7).
        target.transparent    = source.isTransparent();
        target.authored_state = source.authored_state;
        if (!source.instance_transforms.empty()) {
            target.instance_buffer   = source.instance_transforms.values.get();
            target.instance_revision = source.instance_transforms.values->revision();
            target.first_instance    = source.first_instance;
            target.instance_count    = source.instance_count;
        }
    }
    return copy;
}

CameraSnapshot FrameRecorder::snapshotCamera(const vn::graphics::Camera* camera) const
{
    CameraSnapshot snapshot;
    if (camera == nullptr) {
        return snapshot; // present stays false: "the call named no camera" is a fact, not a default
    }
    snapshot.present    = true;
    snapshot.view       = camera->viewMatrix();
    // The CAMERA's own shape is what the projection is built with (see Camera): the engine culls with the same
    // number, so the frustum culled against and the projection rendered with cannot drift.
    snapshot.projection = camera->projectionMatrix();
    snapshot.eye        = camera->eye();
    snapshot.target     = camera->target();
    snapshot.up         = camera->up();
    return snapshot;
}

VN_GRAPHICSBACKEND_NS_END
