#include <vine/vsg/core/FrameRecorder.hpp>

#include <cstddef>
#include <string>

V_VSG_NS_BEGIN

namespace core
{
namespace
{

/// @brief Builds a `vine::String` from an ASCII sentence (the house spelling for UTF-8 bytes).
vine::String asString(const std::string& text)
{
    return vine::String(reinterpret_cast<const char8_t*>(text.c_str()));
}

/// @brief Names one call in the sentence a refusal produces.
const char* callName(CallKind kind) noexcept
{
    switch (kind)
    {
    case CallKind::BeginFrame:
        return "beginFrame()";
    case CallKind::EndFrame:
        return "endFrame()";
    case CallKind::BeginPass:
        return "beginPass()";
    case CallKind::EndPass:
        return "endPass()";
    case CallKind::SetScopeAttribute:
        return "an announced scope attribute";
    case CallKind::Draw:
        return "a drawing call";
    case CallKind::SwapBuffers:
        return "swapBuffers()";
    case CallKind::ReleaseRenderTarget:
        return "releaseRenderTarget()";
    }
    return "a call";
}

/// @brief Pairs a borrowed program with the revision it is at.
ProgramRef programRef(const vine::graphics::ShaderProgram* program) noexcept
{
    ProgramRef ref;
    if (program != nullptr)
    {
        ref.program  = program;
        ref.revision = program->revision();
    }
    return ref;
}

}  // namespace

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
    const Decision decision = protocol_.onCall(CallKind::BeginFrame);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::BeginFrame);
        }
        return false;
    }

    // The frame boundary is the ONE moment a span handed out earlier may die (see FrameArena).
    arena_.reset();
    token_       = token;
    pass_open_   = false;
    pass_inputs_ = {};
    pending_has_viewport_ = false;
    pending_viewport_     = {};
    pending_lights_       = {};
    open_draws_.clear();
    passes_.clear();

    description_                 = FrameDescription{};
    description_.token           = token;
    description_.default_program = default_program_;
    return true;
}

bool FrameRecorder::endFrame()
{
    const Decision decision = protocol_.onCall(CallKind::EndFrame);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::EndFrame);
        }
        return false;
    }

    // The pass records are published in one arena array: the compiler reads them after this frame's calls
    // are over, and the caller's own containers are gone by then.
    const std::span<CollectedPass> published = arena_.makeArray<CollectedPass>(passes_.size());
    for (std::size_t i = 0; i < passes_.size(); ++i)
    {
        published[i] = passes_[i];
    }
    description_.passes = published;
    return true;
}

bool FrameRecorder::swapBuffers()
{
    const Decision decision = protocol_.onCall(CallKind::SwapBuffers);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SwapBuffers);
        }
        return false;
    }
    return true;
}

bool FrameRecorder::beginPass(PassId pass)
{
    const Decision decision = protocol_.onCall(CallKind::BeginPass);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::BeginPass);
        }
        return false;
    }

    pass_open_              = true;
    pass_id_                = pass;
    order_                  = 0;
    pass_target_            = nullptr;
    pass_has_clear_         = false;
    pass_clear_             = ClearPolicy{};
    pass_depth_             = vine::graphics::DepthMode::TestAndWrite;
    pass_inputs_            = {};
    pending_has_viewport_   = false;
    pending_viewport_       = {};
    pending_lights_         = {};
    open_draws_.clear();
    return true;
}

bool FrameRecorder::endPass()
{
    const Decision decision = protocol_.onCall(CallKind::EndPass);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::EndPass);
        }
        return false;
    }

    // A scope enters the plan when it can have an effect: it drew something, or it announced a clear (a
    // clear is applied by the pass' own load-op, so a clear-only pass is a real pass with no draws). Every
    // other attribute of a scope that drew nothing is dropped here, which is what stops it from leaking
    // into the next pass.
    if (!open_draws_.empty() || pass_has_clear_)
    {
        CollectedPass record;
        record.pass      = pass_id_;
        record.order     = order_;
        record.target    = pass_target_;
        record.inputs    = pass_inputs_;
        record.has_clear = pass_has_clear_;
        record.clear     = pass_clear_;
        record.depth     = pass_depth_;

        const std::span<CollectedDraw> draws = arena_.makeArray<CollectedDraw>(open_draws_.size());
        for (std::size_t i = 0; i < open_draws_.size(); ++i)
        {
            draws[i] = open_draws_[i];
        }
        record.draws = draws;

        passes_.push_back(record);
        ++observe_.counters().passes;
    }

    pass_open_   = false;
    open_draws_.clear();
    pass_inputs_ = {};
    return true;
}

bool FrameRecorder::setPassOrder(int order)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }
    order_ = order;
    return true;
}

bool FrameRecorder::setRenderTarget(const void* target)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }

    // The protocol holds the "this scope's target was released" fact, and a new announcement is what ends
    // that episode (see Protocol::noteAnnouncedTarget) - so the note is not bookkeeping the recorder keeps
    // on its own.
    pass_target_ = target;
    protocol_.noteAnnouncedTarget(target);
    return true;
}

bool FrameRecorder::setViewport(int x, int y, int width, int height)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }
    pending_has_viewport_ = true;
    pending_viewport_     = vine::graphics::Viewport{ x, y, width, height };
    return true;
}

bool FrameRecorder::setClearPolicy(const ClearPolicy& policy)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }
    pass_has_clear_ = true;
    pass_clear_     = policy;
    return true;
}

bool FrameRecorder::setDepthMode(vine::graphics::DepthMode mode)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }
    pass_depth_ = mode;
    return true;
}

bool FrameRecorder::setPassInputs(std::span<const vine::graphics::RenderTarget* const> inputs)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }

    // COPIED, not held: the engine's resolved-input vector is a reused member that the next pass refills
    // (RenderEngine::resolvePassInputs), so holding a span into it would read the next pass' inputs.
    const std::span<InputRef> copy = arena_.makeArray<InputRef>(inputs.size());
    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        copy[i].target = inputs[i];
    }
    pass_inputs_ = copy;
    return true;
}

bool FrameRecorder::setLights(std::span<const vine::graphics::Light* const> lights)
{
    const Decision decision = protocol_.onCall(CallKind::SetScopeAttribute);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::SetScopeAttribute);
        }
        return false;
    }
    snapshotLights(lights);
    return true;
}

bool FrameRecorder::render(std::span<const vine::graphics::RenderCommand> commands,
                           const vine::graphics::Camera*                    camera)
{
    const Decision decision = protocol_.onCall(CallKind::Draw);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::Draw);
        }
        return false;
    }

    CollectedDraw draw;
    draw.kind         = DrawKind::Content;
    draw.camera       = snapshotCamera(camera);
    draw.has_viewport = pending_has_viewport_;
    draw.viewport     = pending_viewport_;
    draw.lights       = pending_lights_;
    draw.commands     = snapshotCommands(commands);
    open_draws_.push_back(draw);

    // ONE announcement serves ONE drawing call: the next draw of this scope that wants them announces
    // again (see the file note). The pass-level attributes stay, because they belong to the pass.
    pending_has_viewport_ = false;
    pending_viewport_     = {};
    pending_lights_       = {};

    ++observe_.counters().draws;
    return true;
}

bool FrameRecorder::drawScreenProgram(const void* source, const vine::graphics::ShaderProgram* program,
                                      const vine::graphics::Camera* camera)
{
    const Decision decision = protocol_.onCall(CallKind::Draw);
    if (decision.verdict != Verdict::Allow)
    {
        if (decision.report)
        {
            reportRefusal(CallKind::Draw);
        }
        return false;
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

bool FrameRecorder::setDefaultContentProgram(const vine::graphics::ShaderProgram* program)
{
    // Frame-level, and legal in every state - there is no question for the protocol to answer here (see
    // the file note). The setting outlives the frame it was made in, so it is kept next to the token.
    default_program_             = programRef(program);
    description_.default_program = default_program_;
    return true;
}

bool FrameRecorder::releaseRenderTarget(const void* target)
{
    // Always legal, whenever it arrives (the contract says so): the caller is telling the backend a target
    // is going away, and the announcement an open scope may still hold is dropped by the note below.
    (void)protocol_.onCall(CallKind::ReleaseRenderTarget);
    protocol_.noteTargetReleased(target);
    return true;
}

const FrameDescription& FrameRecorder::description() const noexcept
{
    return description_;
}

bool FrameRecorder::inFrame() const noexcept
{
    return protocol_.frameOpen();
}

bool FrameRecorder::inPass() const noexcept
{
    return protocol_.scopeOpen();
}

const Protocol& FrameRecorder::protocol() const noexcept
{
    return protocol_;
}

void FrameRecorder::reportRefusal(CallKind kind)
{
    const bool dead_scope = protocol_.scopeOpen() && protocol_.announcedTargetReleased();

    std::string message = "frame " + std::to_string(token_.frame) + ": ";
    if (dead_scope && kind == CallKind::Draw)
    {
        message += "a drawing call would have used the render target released while this scope was open: it "
                   "is skipped rather than redirected to the default framebuffer, so the content is missing "
                   "instead of drawn where the caller never asked for it";
    }
    else if (dead_scope)
    {
        message += std::string(callName(kind)) +
                   " arrives for a scope whose announced render target was released: it is dropped, so "
                   "nothing it configures can reach a draw";
    }
    else
    {
        switch (kind)
        {
        case CallKind::BeginFrame:
            message += "beginFrame() arrives while a frame is already open: the open frame keeps its state";
            break;
        case CallKind::EndFrame:
            message += "endFrame() does not close a frame: it arrives with no frame open, or while a pass "
                       "scope is still open";
            break;
        case CallKind::BeginPass:
            message += "beginPass() arrives with no frame open, or while a pass scope is still open: the "
                       "announced pass does not run, and the state the caller expects will not apply";
            break;
        case CallKind::EndPass:
            message += "endPass() closes no scope: it arrives with no pass scope open (a nested beginPass() "
                       "was refused earlier, so the caller's bookkeeping is off by one scope)";
            break;
        case CallKind::SetScopeAttribute:
            message += "an announced scope attribute arrives with no pass scope open: it is dropped, and the "
                       "next pass starts from an empty request";
            break;
        case CallKind::Draw:
            message += "a drawing call arrives with no pass scope open: it is refused and nothing is drawn, "
                       "rather than drawn with the previous pass' state";
            break;
        case CallKind::SwapBuffers:
            message += "swapBuffers() cannot present: it arrives with no frame open, or with a pass scope "
                       "still open";
            break;
        case CallKind::ReleaseRenderTarget:
            break;
        }
    }

    diagnostics_.report(vine::graphics::DiagnosticSeverity::Error,
                        vine::graphics::DiagnosticCategory::PassProtocolViolation, asString(message));
}

void FrameRecorder::snapshotLights(std::span<const vine::graphics::Light* const> lights)
{
    const std::span<LightRef> copy = arena_.makeArray<LightRef>(lights.size());
    for (std::size_t i = 0; i < lights.size(); ++i)
    {
        const vine::graphics::Light* light = lights[i];
        if (light == nullptr)
        {
            continue;  // a null entry stays the default "disabled" light: the array keeps its positions
        }
        LightRef& ref     = copy[i];
        ref.enabled       = light->isEnabled();
        ref.type          = light->type();
        ref.color         = light->color();
        ref.intensity     = light->intensity();
        ref.has_direction = light->hasDirection();
        if (ref.has_direction)
        {
            ref.direction = light->direction();
        }
    }
    pending_lights_ = copy;
}

std::span<const CollectedCommand> FrameRecorder::snapshotCommands(
    std::span<const vine::graphics::RenderCommand> commands)
{
    const std::span<CollectedCommand> copy = arena_.makeArray<CollectedCommand>(commands.size());
    for (std::size_t i = 0; i < commands.size(); ++i)
    {
        const vine::graphics::RenderCommand& source = commands[i];
        CollectedCommand&                    target = copy[i];

        target.geometry          = source.geometry.get();
        target.geometry_revision = source.geometry != nullptr ? source.geometry->revision() : 0;
        target.program           = programRef(source.program.get());
        target.material          = source.material.get();
        target.model             = source.modelMatrix;
        target.opacity           = source.opacity;
        target.state             = source.renderState;
        target.depth_explicit    = source.depthExplicit;
    }
    return copy;
}

CameraSnapshot FrameRecorder::snapshotCamera(const vine::graphics::Camera* camera)
{
    CameraSnapshot snapshot;
    if (camera == nullptr)
    {
        return snapshot;  // present stays false: "the call named no camera" is a fact, not a default
    }
    snapshot.present    = true;
    snapshot.view       = camera->viewMatrix();
    snapshot.projection = camera->projectionMatrix();
    snapshot.eye        = camera->eye();
    snapshot.target     = camera->target();
    snapshot.up         = camera->up();
    return snapshot;
}

}  // namespace core

V_VSG_NS_END
