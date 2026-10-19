#include <vine/graphics/RenderEngine.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Viewport.hpp>

#include "FrameResources.hpp"

VN_GRAPHICS_NS_BEGIN

namespace
{
/// @brief The shape difference between a pass' rectangle and its camera's picture that is rounding, not a mistake.
///
/// A pane's rectangle is the pane's logical size scaled by the pixel ratio, and that conversion truncates (see
/// deviceViewport), so one shape can come out a device pixel off - a fraction of a percent on any real pane. A
/// mismatch worth reporting is a different RATIO: 16:9 against 4:3 is a third of a picture, and a shared camera
/// between a pane and a square target is most of one.
constexpr double kShapeTolerance = 0.02;

/// @brief The shape of the picture an orthographic camera's projection is built for.
///
/// An orthographic camera's window carries its own shape and its aspect is not read at all (see Camera), so the
/// rectangle it draws into is measured against the window.
///
/// @param camera Camera to read.
/// @return The window's width / height, or 1.0 for a window with no area.
double orthographicShape(const Camera& camera) noexcept
{
    const double height = camera.orthographicTop() - camera.orthographicBottom();
    const double width  = camera.orthographicRight() - camera.orthographicLeft();
    return (height > 0.0 && width > 0.0) ? width / height : 1.0;
}

/// @brief Converts SDK-built ASCII digits to the string type a message is assembled from.
///
/// @param text ASCII text (the SDK's messages carry every number as digits).
/// @return The same text as a u8 string.
std::u8string toU8(const std::string& text)
{
    return std::u8string(text.begin(), text.end());
}

/// @brief Formats a picture shape for a report.
///
/// @param shape Width / height of the picture.
/// @return The shape as digits, e.g. "1.78:1".
std::string shapeText(double shape)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.3g:1", shape);
    return std::string(buffer);
}
}  // namespace

void RenderEngine::compileFrame()
{
    // THIS IS THE FRAME. Everything the frame needs to know is derived here, from the declarations as they
    // stand right now: which pass runs when (the slot list is already ordered), what each declared input
    // resolves to (what ran BEFORE it - the same rule the runtime used to apply as it went), which transient
    // targets exist, and what is wrong with the set. The tables are reused, so a steady frame allocates
    // nothing beyond what the compile's own bookkeeping already has capacity for.
    plan_problems_.clear();
    plan_rows_.clear();
    resources_->beginFrame();
    unusable_inputs_this_frame_.clear();
    // From here until the list is complete, every report the checks make is COLLECTED, not emitted: the
    // episode rule is applied once, at the end (see reportPlanProblems).
    collecting_problems_ = true;

    // A host binding is filled by the HOST: it is there from the start of the frame and no pass owns its
    // hand-off, so it seeds what this frame has already produced (an object-typed input addressing one is
    // answered like one addressing a pass' target).
    resources_->seedFromHostBindings();

    // Transients first: a slot that declares one gets its target before anything reads a target, and one
    // nobody declares this frame is let go of.
    resources_->beginTransientCompile();
    for (Slot& slot : slots_) {
        if (!slot.writes_image.empty()) {
            resolveTransientTarget(slot);
        }
    }
    resources_->retireUndeclared(device_.get());

    // The structural checks read DECLARATIONS only, so they run before anything is resolved: a wire the check
    // already spoke about is one problem, and the resolution below stays quiet about it (see hasPlanProblem).
    collectPlanProblems();

    plan_rows_.reserve(slots_.size());
    for (Slot& slot : slots_) {
        PlanRow row;
        row.pass    = slot.pass.get();
        row.content = slot.content.get();
        row.order   = slot.order;
        if (row.pass != nullptr && row.pass->isEnabled()) {
            // What this pass reads: everything produced BEFORE it, which is exactly what "resolved at draw
            // time" meant - a producer registered later is not part of the frame this pass can see.
            resolveRowInputs(row.pass, row.inputs);
            // What it publishes: the name serves the rest of the frame (the latest publisher wins the name,
            // as the per-frame registry did).
            publishRowOutput(row.pass);
            // What it fills: drawing into a target fills every image it has, which is what an object-typed
            // declared input is answered from. Linear and idempotent - several passes may fill one target.
            if (raw_ptr<RenderTarget> drawn_into = row.pass->renderTarget(); drawn_into != nullptr) {
                resources_->recordProduced(drawn_into);
            }
        }
        plan_rows_.push_back(std::move(row));
    }

    // The last of the compile's checks, and the only one that reads the FRAME's own facts (a target's size) next
    // to a declaration's: whether the picture each pass draws is projected for the shape of the rectangle it
    // lands in (see collectPictureShapeMismatches).
    collectPictureShapeMismatches();

    // And then the frame itself, as the value a backend consumes (see buildFrameValue): the same descriptions the
    // passes would have announced one by one, assembled here, before anything is dispatched - the whole point of
    // the engine's own value (compileFrame) reaching the boundary.
    buildFrameValue();

    collecting_problems_ = false;
}

void RenderEngine::buildFrameValue()
{
    // EVERY ENABLED PASS HAS AN ENTRY, whether it issues anything or not: "this pass ran and had nothing to
    // issue" is a fact of the frame (its scope opens and closes, and a backend keys per-pass retained state by
    // it), which is not the same fact as "this pass did not run" - a disabled pass has no entry at all.
    frame_value_.passes.clear();
    // Rows and slots are one per index (see compileFrame): the row is the frame's answer about a pass, the slot
    // is the declaration itself - and the hook the declaration carries belongs to the DECLARATION.
    for (std::size_t index = 0; index < plan_rows_.size(); ++index) {
        PlanRow&          row  = plan_rows_[index];
        RenderPass* const pass = row.pass;
        if (pass == nullptr || !pass->isEnabled()) {
            if (index < slots_.size()) {
                slots_[index].record_micros = 0;   // a pass that does not run costs nothing this frame
            }
            continue;
        }
        // THIS IS WHAT A PASS COSTS THE FRAME. Under a value-driven frame the work is here: the hook runs, the
        // inputs are told, the pass describes itself (collecting and culling its content) - and the run then only
        // announces what came out. The number is what the plan reports per pass (see
        // FramePlan::PassPlan::record_micros), which is the host-visible half of a frame; the GPU half is the
        // backend's (see RenderEngine::frameReport). One clock read per pass, not per draw.
        const auto pass_started = std::chrono::steady_clock::now();
        // Content that updates itself per frame (an FPS readout, a mirrored gizmo) does it through the hook its
        // declaration carried, and it is what the pass is ABOUT TO describe: it writes content, so it happens
        // here - before the pass is described - and in run order.
        if (index < slots_.size() && slots_[index].before_draw) {
            slots_[index].before_draw();
        }
        // The declared inputs were resolved by the compile (see compileFrame's rows), and the pass is told them
        // here: they are what a ScreenPass' effect reads, and they are part of what the pass describes.
        pass->resolveInputTextures(row.inputs);

        FrameDescription::Pass entry;
        entry.pass  = pass;
        entry.order = row.order;
        entry.inputs.assign(row.inputs.begin(), row.inputs.end());
        PassDescription described;
        if (pass->describe(row.content, described)) {
            entry.description = std::move(described);
        }
        if (index < slots_.size()) {
            slots_[index].record_micros = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                      pass_started)
                    .count());
        }
        frame_value_.passes.push_back(std::move(entry));
    }
}

void RenderEngine::collectPictureShapeMismatches()
{
    // ONE CAMERA, ONE PICTURE SHAPE (see Camera). A pass' picture is projected with its CAMERA's shape while the
    // rectangle it lands in is its viewport, or the whole target it draws into (the surface, for the window) - and
    // the two agreeing is what makes an unstretched picture. Where they disagree the content was culled for one
    // shape and drawn into another: a stretched picture, and nothing downstream can see it, because the backend
    // is handed a rectangle and a camera snapshot that are each perfectly legal.
    //
    // Who this CANNOT be about:
    //   * a ScreenPass, whose full-screen triangle is built in clip space and reads no projection (see
    //     BuiltinShaders::fullscreenVertexProgram): its camera states the view the lighting is computed in, so the
    //     rectangle cannot stretch what it draws;
    //   * a pass that is disabled (it draws nothing), has no camera (it draws nothing - see RenderPass::describe),
    //     or has no content (a scene pass with no scene issues nothing at all);
    //   * an orthographic camera only in the sense that its SHAPE is its window's, not its aspect (see Camera):
    //     the window is what the picture is projected for, so that is what the rectangle is measured against.
    shape_mismatches_this_frame_.clear();

    for (const Slot& slot : slots_) {
        const RenderPass* const pass = slot.pass.get();
        if (pass == nullptr || !pass->isEnabled() || slot.content == nullptr || pass->camera() == nullptr) {
            continue;
        }
        if (dynamic_cast<const ScreenPass*>(pass) != nullptr) {
            continue;
        }

        // The rectangle: the pass' viewport when it named one, else the whole thing it draws into. A window pass
        // (no render target) draws into the surface this frame presents, which is what its rectangle is sized
        // from; a surface nobody has sized yet describes no picture, so there is nothing to check.
        int width  = frame_ctx_.surface_width;
        int height = frame_ctx_.surface_height;
        if (raw_ptr<const RenderTarget> drawn_into = pass->renderTarget(); drawn_into != nullptr) {
            width  = drawn_into->width();
            height = drawn_into->height();
        }
        if (pass->hasViewport()) {
            const Viewport viewport = pass->viewport();
            width                   = viewport.width;
            height                  = viewport.height;
        }
        if (width <= 0 || height <= 0) {
            continue;
        }

        const Camera& camera   = *pass->camera();
        const double  picture  = camera.projectionType() == Camera::ProjectionType::Orthographic
                                     ? orthographicShape(camera)
                                     : camera.aspectRatio();
        const double  rectangle = static_cast<double>(width) / static_cast<double>(height);
        // A sub-pixel difference is a device-pixel rounding, not a mistake: the rectangle of a pane is the pane's
        // logical size scaled by the pixel ratio, and that conversion truncates (see deviceViewport). A real
        // mismatch - 16:9 against 1:1, a pane against a shared camera - is a dozen times this.
        if (std::abs(rectangle - picture) <= kShapeTolerance * picture) {
            continue;
        }

        auto known = std::find_if(shape_mismatches_last_frame_.begin(), shape_mismatches_last_frame_.end(),
                                  [pass](const ShapeMismatchEpisode& episode) { return episode.pass == pass; });
        ShapeMismatchEpisode episode;
        episode.pass = pass;
        if (known != shape_mismatches_last_frame_.end()) {
            episode.reported = known->reported;
        }
        if (episode.reported.shouldReport()) {
            // The numbers go in as ASCII digits, like every other message in the SDK (see
            // RenderPipelineBuilder::reportRequestedShadows).
            const std::string size_text = std::to_string(width) + "x" + std::to_string(height);
            noteProblem(String(u8"pass '") + reportedPassName(pass) + String(u8"' draws into a ") +
                        String(toU8(size_text)) + String(u8" rectangle, but its camera is projected for a picture "
                                                        u8"of ") +
                        String(toU8(shapeText(picture))) +
                        String(u8" - the picture is stretched (the content was culled for one shape and lands in "
                               u8"another). Give the camera the shape of the rectangle it draws into "
                               u8"(Camera::setAspect), or draw it into the rectangle the camera is shaped for"));
        }
        shape_mismatches_this_frame_.push_back(std::move(episode));
    }

    shape_mismatches_last_frame_.clear();
    shape_mismatches_this_frame_.swap(shape_mismatches_last_frame_);
    shape_mismatches_this_frame_.clear();
}

void RenderEngine::resolveTransientTarget(Slot& slot)
{
    // The extent: what the declaration states, or the surface this frame is presenting (a transient is
    // normally the size of the picture that consumes it). A surface nobody has sized yet is not a reason to
    // build nothing: a 1x1 target keeps the pass' shape defined until the host reports a size.
    const TransientShape& shape  = slot.writes_shape;
    int                   width  = shape.width > 0 ? shape.width : frame_ctx_.surface_width;
    int                   height = shape.height > 0 ? shape.height : frame_ctx_.surface_height;
    if (width <= 0 || height <= 0) {
        width  = 1;
        height = 1;
    }

    slot.transient = resources_->resolveTransient(slot.writes_image, shape, width, height, device_.get());
    slot.pass->setRenderTarget(slot.transient);
}

std::size_t RenderEngine::transientTargetCount() const noexcept
{
    return resources_->transientCount();
}

raw_ptr<RenderTarget> RenderEngine::transientTarget(const String& identity) const
{
    return resources_->transient(identity);
}

void RenderEngine::resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<raw_ptr<RenderTarget>>& out)
{
    out.clear();
    if (pass == nullptr) {
        return;
    }

    // The object-typed declarations ARE the wiring; a name is the sugar over it. When a pass declares images
    // or targets, they decide what it gets - one resolved entry per declaration, in declaration order, null
    // where nothing produced it - and the names are not consulted at all: one wire stated twice must not be
    // resolved twice.
    if (!pass->inputs().empty() || !pass->inputTargets().empty()) {
        out.reserve(pass->inputs().size() + pass->inputTargets().size());
        for (const auto& image : pass->inputs()) {
            out.push_back(image != nullptr ? resolveDeclaredImage(pass, *image) : nullptr);
        }
        for (const auto& target : pass->inputTargets()) {
            out.push_back(target != nullptr ? resolveDeclaredTarget(pass, target.get()) : nullptr);
        }
        return;
    }

    const auto& names = pass->inputNames();
    if (names.empty()) {
        return;
    }
    out.reserve(names.size());
    for (const auto& name : names) {
        out.push_back(resources_->resolve(name));
    }

    // A declared input nobody published this frame means the pass draws NOTHING (ScreenPass keeps the first
    // non-null input and returns early when there is none), so the frame silently misses its content: the
    // wiring is the engine's job, so it says so. A pass may declare several alternative names (a chain that
    // falls back), so this only fires when NONE of them resolved - and the compile forgets the problem the
    // moment one of them resolves, which is what re-arms it.
    const bool any_resolved = std::any_of(out.begin(), out.end(),
                                         [](raw_ptr<RenderTarget> target) { return target != nullptr; });
    if (any_resolved) {
        return;
    }
    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        String(u8"pass '") + reportedPassName(pass) + String(u8"' declared input '") +
                            names.front() +
                            String(u8"' but no pass published it this frame; the pass draws nothing"
                                   u8" (check the producer's order, its enabled state and its output name)"));
}

void RenderEngine::publishRowOutput(raw_ptr<RenderPass> pass)
{
    if (pass == nullptr) {
        return;
    }
    const String& name = pass->outputName();
    if (name.empty()) {
        return;   // an empty name is how a pass says "do not publish me": not a problem to report
    }
    raw_ptr<RenderTarget> target = pass->renderTarget();
    if (target == nullptr) {
        // The registry hands out a SAMPLEABLE target, and a pass rendering into the window has none. Reported
        // instead of dropping the declaration: a consumer of that name would otherwise be told "nothing
        // produced it this frame", pointing at the consumer for the producer's mistake.
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped,
                            String(u8"pass '") + reportedPassName(pass) +
                                String(u8"' publishes the output name '") + name +
                                String(u8"' but renders into the window (no render target), so nothing is"
                                       u8" published and a consumer of '") +
                                name + String(u8"' finds nothing — give the pass a render target, or drop the"
                                              u8" output name"));
        return;
    }
    if (detail::FramePublication* published = resources_->findPublication(name); published != nullptr) {
        if (published->target != nullptr && published->target != target) {
            // Two passes published DIFFERENT targets under one name: every consumer of that name silently gets
            // whichever pass ran last, and only the whole table can see the collision (each pass is
            // individually valid).
            const String first =
                published->target->name().empty() ? String(u8"(unnamed)") : published->target->name();
            const String second = target->name().empty() ? String(u8"(unnamed)") : target->name();
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass output '") + name +
                                    String(u8"' is published by two passes this frame (targets '") + first +
                                    String(u8"' and '") + second + String(u8"'); every consumer of '") + name +
                                    String(u8"' samples whichever pass ran last"));
        }
        published->target = target;   // the latest publication serves the name for the rest of the frame
        return;
    }
    resources_->recordPublication(pass, target);
}

void RenderEngine::noteProblem(String message)
{
    // One problem per sentence per frame: the checks visit a subject once, but a subject can be reached from
    // more than one direction (a second and a third claimant of one image), and one mistake should read as one
    // message. The episode rule decides whether it is said out loud at all (see reportPlanProblems).
    const auto known = std::find_if(plan_problems_.begin(), plan_problems_.end(),
                                    [&message](const PlanProblem& problem) { return problem.message == message; });
    if (known == plan_problems_.end()) {
        plan_problems_.push_back(PlanProblem{ std::move(message) });
    }
}

bool RenderEngine::unusableInputReported(raw_ptr<const RenderPass> pass,
                                         const OutputIdentity& identity) const noexcept
{
    const auto key = std::make_pair(pass, identity);
    return std::any_of(unusable_inputs_this_frame_.begin(), unusable_inputs_this_frame_.end(),
                       [&key](const std::pair<raw_ptr<const RenderPass>, OutputIdentity>& entry) {
                           return entry.first == key.first && entry.second.target == key.second.target &&
                                  entry.second.attachment == key.second.attachment &&
                                  entry.second.depth == key.second.depth &&
                                  entry.second.declared == key.second.declared;
                       });
}

void RenderEngine::reportPlanProblems()
{
    for (const PlanProblem& problem : plan_problems_) {
        const auto previous = std::find_if(previous_problems_.begin(), previous_problems_.end(),
                                           [&problem](const PlanProblem& earlier) {
                                               return problem.message == earlier.message;
                                           });
        if (previous != previous_problems_.end()) {
            continue;   // the same problem, in the same words: this episode has already been said
        }
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped, problem.message);
    }
    // What this compile found becomes what the next compile compares against: a problem that is gone drops out,
    // so the same breakage later is a new episode.
    previous_problems_.swap(plan_problems_);
    plan_problems_.clear();
}

intrusive_ptr<RenderPass> RenderEngine::insertSlot(Slot slot)
{
    if (slot.pass == nullptr) {
        return nullptr;
    }
    // Registering the same pass instance twice is ignored: it would otherwise run twice per frame. To rebind
    // content use bindPassContent(); to change the order remove the pass and re-add it.
    for (const auto& existing : slots_) {
        if (existing.pass.get() == slot.pass.get()) {
            return nullptr;
        }
    }
    // Keep the slots ascending by order; equal orders preserve insertion order (stable), so ties are resolved
    // by the addPass() call sequence.
    const int order = slot.order;
    const auto it   = std::find_if(slots_.begin(), slots_.end(),
                                   [order](const Slot& existing) { return existing.order > order; });
    const auto registered = slots_.insert(it, std::move(slot));
    return registered->pass;
}

intrusive_ptr<RenderPass> RenderEngine::addPass(const PassDesc& desc)
{
    // The declaration is turned into the execution node of its kind ONCE, and that node is the only storage of
    // it: a descriptor-declared pass and a hand-built one cannot drift apart, because by the time the engine
    // holds it, it IS a hand-built pass (which is also what keeps the escape hatch the same thing).
    intrusive_ptr<RenderPass> node;
    if (desc.kind == PassKind::Screen) {
        auto screen = make_intrusive<ScreenPass>();
        screen->setProgram(desc.program);
        node = std::move(screen);
    }
    else {
        node = make_intrusive<RenderPass>();
    }

    node->setName(desc.name);
    node->setCamera(desc.camera);
    if (desc.writes != nullptr) {
        node->setRenderTarget(desc.writes);
    }
    // The clear policy is applied only where the declaration states one: an unstated policy leaves the kind's
    // own (a Screen pass never clears, a Scene pass does), which is what keeps a descriptor and a hand-built
    // pass the same pass (see PassDesc::clear_enabled).
    if (desc.clear_enabled.has_value()) {
        node->setClearEnabled(*desc.clear_enabled);
    }
    if (desc.clear_color.has_value()) {
        node->setClearColor(*desc.clear_color);
    }
    if (desc.clear_depth.has_value()) {
        node->setShouldClearDepth(*desc.clear_depth);
    }
    node->setDepthMode(desc.depth_mode);
    node->setLayerMask(desc.layer_mask);
    node->setProgramOverride(desc.program_override);
    if (desc.polygon_mode.has_value()) {
        node->setPolygonModeOverride(*desc.polygon_mode);
    }
    if (desc.viewport.has_value()) {
        node->setViewport(*desc.viewport);
    }
    node->setOutputName(desc.output_name);
    node->setOutputTarget(desc.output_target);
    for (const auto& image : desc.inputs) {
        node->addInput(image);
    }
    for (const auto& target : desc.input_targets) {
        node->addInputTarget(target);
    }
    for (const auto& name : desc.input_names) {
        node->addInputName(name);
    }
    node->setEnabled(desc.enabled);

    Slot slot;
    slot.pass         = std::move(node);
    slot.content      = desc.content;
    slot.order        = desc.order.value_or(pipelineStageOrder(desc.stage));
    slot.tag          = desc.tag;
    slot.writes_image = desc.writes_image;
    slot.writes_shape = desc.writes_shape;
    slot.before_draw  = desc.before_draw;
    return insertSlot(std::move(slot));
}

VN_GRAPHICS_NS_END
