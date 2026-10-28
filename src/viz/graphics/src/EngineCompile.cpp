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
#include "FrameScheduler.hpp"

VN_GRAPHICS_NS_BEGIN

namespace
{

/// @brief The shape difference between a pass' rectangle and its camera's picture that is rounding, not a mistake.
///
/// A pane's rectangle is the pane's logical size scaled by the pixel ratio, and that conversion truncates (see
/// PixelGeometry.hpp's toDeviceSize), so one shape can come out a device pixel off - a fraction of a percent on any
/// real pane. A mismatch worth reporting is a different RATIO: 16:9 against 4:3 is a third of a picture, and a
/// shared camera between a pane and a square target is most of one.
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

} // namespace

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
        if (slot.pass != nullptr && !slot.pass->declaration().writes_image.empty()) {
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
        row.content = slot.pass != nullptr ? slot.pass->content() : nullptr;
        row.order   = slot.order;
        plan_rows_.push_back(std::move(row));
    }

    // THE ONE SCHEDULER: what a pass READS decides when it may run, and the announced order only breaks ties. A
    // producer announced AFTER its consumer is therefore ordinary, not a wiring mistake - the resolution below
    // walks the run order, so the consumer sees it. A cyclic component is left out of the run order and reported.
    scheduleRows();

    for (const std::uint32_t index : frame_order_) {
        PlanRow&          row  = plan_rows_[index];
        RenderPass* const pass = row.pass;
        if (pass == nullptr) {
            continue; // excluded by the scheduler (disabled or cyclic); buildFrameValue never sees it
        }
        // What this pass reads: everything that RUNS before it, which is now the scheduled order and no longer
        // "whatever happened to be registered earlier".
        resolveRowInputs(pass, row.inputs);
        // What it publishes: the name serves the rest of the frame (the latest publisher wins the name, as the
        // per-frame registry did).
        publishRowOutput(pass);
        // What it fills: drawing into a target fills every image it has, which is what an object-typed declared
        // input is answered from. Linear and idempotent - several passes may fill one target.
        if (raw_ptr<RenderTarget> drawn_into = pass->renderTarget(); drawn_into != nullptr) {
            resources_->recordProduced(drawn_into);
        }
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

void RenderEngine::scheduleRows()
{
    // The announced key of every row, in one contiguous buffer the scheduler reads (reused, like its own tables).
    frame_order_keys_.clear();
    frame_order_keys_.reserve(plan_rows_.size());
    for (const PlanRow& row : plan_rows_) {
        frame_order_keys_.push_back(row.order);
    }
    scheduler_->reset(std::span<const int>(frame_order_keys_.data(), frame_order_keys_.size()));

    const std::uint32_t count = static_cast<std::uint32_t>(plan_rows_.size());
    for (std::uint32_t index = 0; index < count; ++index) {
        RenderPass* const pass = plan_rows_[index].pass;
        if (pass == nullptr || !pass->isEnabled()) {
            scheduler_->exclude(index); // a disabled pass neither runs nor orders anything this frame
        }
    }

    // "A pass writes its target" is the producer half. It is judged from the DECLARATION (the resolution runs after
    // this order is known), and it mirrors what the backend counts as a write: a pass that can issue a draw. A pass
    // that merely names a target (no camera, no program) writes nothing and orders nothing - a clear is part of a
    // draw, not a substitute for one.
    const auto writes = [](const RenderPass& pass) -> bool {
        if (const auto* screen = dynamic_cast<const ScreenPass*>(&pass)) {
            return screen->program() != nullptr && (!pass.inputs().empty() || !pass.inputTargets().empty() || !pass.inputNames().empty());
        }
        return pass.camera() != nullptr && (pass.content() != nullptr || pass.isClearEnabled());
    };
    const auto add_edges = [this, count, &writes](std::uint32_t consumer, const RenderTarget* target) {
        if (target == nullptr) {
            return;
        }
        for (std::uint32_t producer = 0; producer < count; ++producer) {
            if (producer == consumer) {
                continue; // sampling what this pass itself writes is the feedback pattern, not a cycle
            }
            RenderPass* const writer = plan_rows_[producer].pass;
            if (writer != nullptr && writer->isEnabled() && writer->renderTarget() == target && writes(*writer)) {
                scheduler_->addEdge(producer, consumer);
            }
        }
    };

    for (std::uint32_t consumer = 0; consumer < count; ++consumer) {
        RenderPass* const pass = plan_rows_[consumer].pass;
        if (pass == nullptr || !pass->isEnabled()) {
            continue;
        }
        for (const intrusive_ptr<RenderTarget>& target : pass->declaration().input_targets) {
            add_edges(consumer, target.get());
        }
        for (const ImageInput& input : pass->declaration().inputs) {
            if (raw_ptr<ImageRef> image = input.image(); image != nullptr) {
                add_edges(consumer, image->target());
            }
        }
        for (const String& name : pass->inputNames()) {
            for (std::uint32_t producer = 0; producer < count; ++producer) {
                RenderPass* const writer = plan_rows_[producer].pass;
                if (producer != consumer && writer != nullptr && writer->isEnabled() && writer->renderTarget() != nullptr && writer->outputName() == name &&
                    writes(*writer))
                {
                    scheduler_->addEdge(producer, consumer);
                }
            }
        }
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr) {
            const DepthView depth = target->sharedDepth();
            if (depth.isValid()) {
                add_edges(consumer, depth.target());
            }
        }
    }

    const detail::FrameSchedule& schedule = scheduler_->schedule();
    frame_order_.assign(schedule.order.begin(), schedule.order.end());

    // A cycle is a declaration no order can satisfy: its whole component is skipped (every other pass still runs and
    // the frame still presents) and said once per episode, because the members and their mistake do not change.
    for (const std::vector<std::uint32_t>& component : schedule.cycles) {
        String members;
        for (const std::uint32_t member : component) {
            if (!members.empty()) {
                members += String(u8", ");
            }
            members += reportedPassName(plan_rows_[member].pass);
        }
        noteProblem(String(u8"passes ") + members +
                    String(u8" declare a dependency cycle (each reads what another writes), which no execution order "
                           u8"can satisfy: those passes are skipped this frame, and the rest of the frame still runs"));
    }

    // A row that does not run this frame (disabled or cyclic) has no timing to report.
    for (Slot& slot : slots_) {
        slot.record_micros = 0;
    }
}

void RenderEngine::buildFrameValue()
{ // EVERY ENABLED PASS HAS AN ENTRY, whether it issues anything or not: "this pass ran and had nothing to
    // issue" is a fact of the frame (its scope opens and closes, and a backend keys per-pass retained state by
    // it), which is not the same fact as "this pass did not run" - a disabled pass has no entry at all.
    //
    // The entries are REUSED rather than rebuilt (see below): a steady frame hands over the same pass list with the
    // same input lists, so it must not free and re-allocate them 60 times a second.
    std::size_t used           = 0;
    // A window pass has no target to read a device-pixel size from. The engine states the surface size (converted
    // with the surface's pixel ratio) on every pass; RenderPass::describe prefers a sub-viewport or the target's
    // own size, so off-screen passes are unaffected.
    const double pixel_ratio   = area_ != nullptr ? area_->pixelRatio() : 1.0;
    const int    device_width  = static_cast<int>(std::lround(static_cast<double>(surface_width_) * pixel_ratio));
    const int    device_height = static_cast<int>(std::lround(static_cast<double>(surface_height_) * pixel_ratio));
    // Rows and slots are one per index (see compileFrame): the row is the frame's answer about a pass, the slot
    // is the declaration itself - and the hook the declaration carries belongs to the DECLARATION.
    for (const std::uint32_t index : frame_order_) {
        PlanRow&          row  = plan_rows_[index];
        RenderPass* const pass = row.pass;
        if (pass == nullptr || !pass->isEnabled()) {
            continue; // the scheduler never puts a pass that does not run into the order
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
        if (index < slots_.size() && slots_[index].pass != nullptr && slots_[index].pass->declaration().before_draw) {
            slots_[index].pass->declaration().before_draw();
        }
        // The declared inputs were resolved by the compile (see compileFrame's rows), and the pass is told them
        // here: they are what a ScreenPass' effect reads, and they are part of what the pass describes.
        pass->resolveInputTextures(row.inputs);

        // Reuse the entry this index had last frame: the input list keeps its capacity (assign, not a fresh copy) and
        // an already-described pass keeps its description's draw list, which fillPassFacts clears but does not free.
        if (used == frame_value_.passes.size()) {
            frame_value_.passes.emplace_back();
        }
        FrameDescription::Pass& entry = frame_value_.passes[used++];
        entry.pass                    = pass;
        entry.order                   = row.order;
        entry.inputs.assign(row.inputs.begin(), row.inputs.end());
        if (!entry.draw.has_value()) {
            entry.draw.emplace();
        }
        pass->setDrawSize(device_width, device_height);
        if (!pass->describe(row.content, *entry.draw)) {
            // "This pass issued nothing" must not leave the previous frame's picture behind: the draw goes away and
            // the entry keeps the fact that the pass ran (see the note at the top).
            entry.draw.reset();
        }
        if (index < slots_.size()) {
            slots_[index].record_micros =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pass_started).count());
        }
    }
    // A frame with fewer running passes than the last one drops the surplus entries (the only frame that frees
    // anything here, and it is a frame whose shape changed).
    if (frame_value_.passes.size() > used) {
        frame_value_.passes.resize(used);
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
        if (pass == nullptr || !pass->isEnabled() || pass->content() == nullptr || pass->camera() == nullptr) {
            continue;
        }
        if (dynamic_cast<const ScreenPass*>(pass) != nullptr) {
            continue;
        }

        // The rectangle: the pass' viewport when it named one, else the whole thing it draws into. A window pass
        // (no render target) draws into the surface this frame presents, which is what its rectangle is sized
        // from; a surface nobody has sized yet describes no picture, so there is nothing to check.
        int width  = surface_width_;
        int height = surface_height_;
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

        const Camera& camera    = *pass->camera();
        const double  picture   = camera.projectionType() == Camera::ProjectionType::Orthographic ? orthographicShape(camera) : camera.aspect();
        const double  rectangle = static_cast<double>(width) / static_cast<double>(height);
        // A sub-pixel difference is a device-pixel rounding, not a mistake: the rectangle of a pane is the pane's
        // logical size scaled by the pixel ratio, and that conversion truncates (see PixelGeometry.hpp's
        // toDeviceSize). A real mismatch - 16:9 against 1:1, a pane against a shared camera - is a dozen times this.
        if (std::abs(rectangle - picture) <= kShapeTolerance * picture) {
            continue;
        }

        auto known = std::find_if(shape_mismatches_last_frame_.begin(), shape_mismatches_last_frame_.end(), [pass](const ShapeMismatchEpisode& episode) {
            return episode.pass == pass;
        });
        ShapeMismatchEpisode episode;
        episode.pass = pass;
        if (known != shape_mismatches_last_frame_.end()) {
            episode.reported = known->reported;
        }
        if (episode.reported.shouldReport()) {
            // The numbers go in as ASCII digits, like every other message in the SDK (see
            // the chain build).
            const std::string size_text = std::to_string(width) + "x" + std::to_string(height);
            noteProblem(String(u8"pass '") + reportedPassName(pass) + String(u8"' draws into a ") + String(toU8(size_text)) +
                        String(u8" rectangle, but its camera is projected for a picture "
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
    const TransientShape& shape  = slot.pass->declaration().writes_shape;
    int                   width  = shape.width > 0 ? shape.width : surface_width_;
    int                   height = shape.height > 0 ? shape.height : surface_height_;
    if (width <= 0 || height <= 0) {
        width  = 1;
        height = 1;
    }

    slot.transient = resources_->resolveTransient(slot.pass->declaration().writes_image, shape, width, height, device_.get());
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

void RenderEngine::resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<ImageSource>& out)
{
    out.clear();
    if (pass == nullptr) {
        return;
    }

    // The object-typed declarations ARE the wiring; a name is the sugar over it. When a pass declares images,
    // textures or targets, they decide what it gets - one resolved entry per declaration, in declaration order,
    // an empty ImageSource where nothing produced it - and the names are not consulted at all: one wire stated
    // twice must not be resolved twice.
    if (!pass->inputs().empty() || !pass->inputTargets().empty()) {
        out.reserve(pass->inputs().size() + pass->inputTargets().size());
        for (const ImageInput& input : pass->inputs()) {
            if (raw_ptr<ImageRef> image = input.image(); image != nullptr) {
                raw_ptr<RenderTarget> target = resolveDeclaredImage(pass, *image);
                out.push_back(target != nullptr ? ImageSource::producedImage(target, image->attachment(), image->kind() == ImageRef::Kind::Depth)
                                                : ImageSource{});
            }
            else if (raw_ptr<Texture> texture = input.texture(); texture != nullptr) {
                out.push_back(ImageSource::imported(texture, input.sampler()));
            }
            else {
                out.push_back(ImageSource{});
            }
        }
        for (const auto& target : pass->inputTargets()) {
            raw_ptr<RenderTarget> resolved = target != nullptr ? resolveDeclaredTarget(pass, target.get()) : nullptr;
            out.push_back(resolved != nullptr ? ImageSource::producedTarget(resolved) : ImageSource{});
        }
        return;
    }

    const auto& names = pass->inputNames();
    if (names.empty()) {
        return;
    }
    out.reserve(names.size());
    for (const auto& name : names) {
        raw_ptr<RenderTarget> resolved = resources_->resolve(name);
        out.push_back(resolved != nullptr ? ImageSource::producedTarget(resolved) : ImageSource{});
    }

    // A declared input nobody published this frame means the pass draws NOTHING (ScreenPass keeps the first
    // non-null input and returns early when there is none), so the frame silently misses its content: the
    // wiring is the engine's job, so it says so. A pass may declare several alternative names (a chain that
    // falls back), so this only fires when NONE of them resolved - and the compile forgets the problem the
    // moment one of them resolves, which is what re-arms it.
    const bool any_resolved = std::any_of(out.begin(), out.end(), [](const ImageSource& source) { return static_cast<bool>(source); });
    if (any_resolved) {
        return;
    }
    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                        vn::graphics::DiagnosticCategory::ContentSkipped,
                        String(u8"pass '") + reportedPassName(pass) + String(u8"' declared input '") + names.front() +
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
        return; // an empty name is how a pass says "do not publish me": not a problem to report
    }
    raw_ptr<RenderTarget> target = pass->renderTarget();
    if (target == nullptr) {
        // The registry hands out a SAMPLEABLE target, and a pass rendering into the window has none. Reported
        // instead of dropping the declaration: a consumer of that name would otherwise be told "nothing
        // produced it this frame", pointing at the consumer for the producer's mistake.
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                            vn::graphics::DiagnosticCategory::ContentSkipped,
                            String(u8"pass '") + reportedPassName(pass) + String(u8"' publishes the output name '") + name +
                                String(u8"' but renders into the window (no render target), so nothing is"
                                       u8" published and a consumer of '") +
                                name +
                                String(u8"' finds nothing — give the pass a render target, or drop the"
                                       u8" output name"));
        return;
    }
    if (detail::FramePublication* published = resources_->findPublication(name); published != nullptr) {
        if (published->target != nullptr && published->target != target) {
            // Two passes published DIFFERENT targets under one name: every consumer of that name silently gets
            // whichever pass ran last, and only the whole table can see the collision (each pass is
            // individually valid).
            const String first  = published->target->name().empty() ? String(u8"(unnamed)") : published->target->name();
            const String second = target->name().empty() ? String(u8"(unnamed)") : target->name();
            reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                vn::graphics::DiagnosticCategory::ContentSkipped,
                                String(u8"pass output '") + name + String(u8"' is published by two passes this frame (targets '") + first +
                                    String(u8"' and '") + second + String(u8"'); every consumer of '") + name + String(u8"' samples whichever pass ran last"));
        }
        published->target = target; // the latest publication serves the name for the rest of the frame
        return;
    }
    resources_->recordPublication(pass, target);
}

void RenderEngine::noteProblem(String message)
{
    // One problem per sentence per frame: the checks visit a subject once, but a subject can be reached from
    // more than one direction (a second and a third claimant of one image), and one mistake should read as one
    // message. The episode rule decides whether it is said out loud at all (see reportPlanProblems).
    const auto known =
        std::find_if(plan_problems_.begin(), plan_problems_.end(), [&message](const PlanProblem& problem) { return problem.message == message; });
    if (known == plan_problems_.end()) {
        plan_problems_.push_back(PlanProblem{ std::move(message) });
    }
}

bool RenderEngine::unusableInputReported(raw_ptr<const RenderPass> pass, const OutputIdentity& identity) const noexcept
{
    const auto key = std::make_pair(pass, identity);
    return std::any_of(unusable_inputs_this_frame_.begin(),
                       unusable_inputs_this_frame_.end(),
                       [&key](const std::pair<raw_ptr<const RenderPass>, OutputIdentity>& entry) {
                           return entry.first == key.first && entry.second.target == key.second.target && entry.second.attachment == key.second.attachment &&
                                  entry.second.depth == key.second.depth && entry.second.declared == key.second.declared;
                       });
}

void RenderEngine::reportPlanProblems()
{
    for (const PlanProblem& problem : plan_problems_) {
        const auto previous = std::find_if(previous_problems_.begin(), previous_problems_.end(), [&problem](const PlanProblem& earlier) {
            return problem.message == earlier.message;
        });
        if (previous != previous_problems_.end()) {
            continue; // the same problem, in the same words: this episode has already been said
        }
        reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning, vn::graphics::DiagnosticCategory::ContentSkipped, problem.message);
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
    const int  order      = slot.order;
    const auto it         = std::find_if(slots_.begin(), slots_.end(), [order](const Slot& existing) { return existing.order > order; });
    const auto registered = slots_.insert(it, std::move(slot));
    return registered->pass;
}

intrusive_ptr<RenderPass> RenderEngine::addPass(const PassDesc& desc)
{
    return addPassOwnedBy(desc, nullptr, String());
}

intrusive_ptr<RenderPass> RenderEngine::addPassOwnedBy(const PassDesc& desc, raw_ptr<Object> owner, const String& role)
{
    // The declaration is turned into the execution node of its kind ONCE, and the node then IS the declaration
    // (RenderPass::setDesc): a descriptor-declared pass and a hand-built one cannot drift apart, because by the time
    // the engine holds it, it is a hand-built pass (which is also what keeps the escape hatch the same thing).
    intrusive_ptr<RenderPass> node;
    if (desc.kind == PassKind::Screen) {
        node = make_intrusive<ScreenPass>();
    }
    else {
        node = make_intrusive<RenderPass>();
    }

    // The depth edge comes with the pass that states it (see PassDesc::depth_view). Three answers, in the order a
    // host can act on them:
    //   * a LAYERED view is not a mechanism this SDK has yet (a target carries one depth image) - refused, said
    //     once, and never silently turned into "the whole image";
    //   * a target draws against ONE depth image, so a second, DIFFERENT declaration is refused too, once per
    //     (target, image) pair - the first declaration stands;
    //   * otherwise the declaration becomes the target's mechanism (setDesc applies it, and the pass keeps it, so a
    //     later setRenderTarget carries the edge along).
    // A refused view is dropped from what the pass adopts rather than left to the pass: the refusal is this compile's
    // judgement, and a declaration the SDK cannot honour must not look accepted on the node.
    PassDesc adopted = desc;
    if (adopted.depth_view.has_value()) {
        const DepthView declared = *adopted.depth_view;
        if (declared.isValid() && declared.layer() != 0) {
            if (layered_depth_reported_.shouldReport()) {
                reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                    vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                    u8"a pass names a layered depth view; layered targets are not built yet, so "
                                    u8"the declaration is refused");
            }
            adopted.depth_view.reset();
        }
        else if (adopted.writes != nullptr && declared.isValid()) {
            const DepthView current = adopted.writes->sharedDepth();
            if (current.isValid() && current.target() != declared.target()) {
                if (depth_view_conflicts_.insert(std::make_pair(adopted.writes.get(), declared.target())).second) {
                    reportEngineProblem(vn::graphics::DiagnosticSeverity::Warning,
                                        vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                        u8"two passes name different depth views for one target; the first "
                                        u8"declaration stands and this one is refused");
                }
                adopted.depth_view.reset();
            }
        }
    }
    node->setDesc(std::move(adopted));

    Slot slot;
    slot.pass  = std::move(node);
    slot.order = desc.order.value_or(passStageOrder(desc.stage));
    slot.owner = std::move(owner);
    slot.role  = role;
    return insertSlot(std::move(slot));
}

void RenderEngine::removePassesOwnedBy(const Object* owner) noexcept
{
    if (owner == nullptr) {
        return; // unowned passes belong to whoever added them; no chain can take them back
    }
    // Collected first: removePass() mutates the table, and the release path it takes (backend state, an owned
    // target) must run once per pass - which is exactly what removePass does for a handle.
    std::vector<raw_ptr<RenderPass>> doomed;
    for (const Slot& slot : slots_) {
        if (slot.owner == owner && slot.pass != nullptr) {
            doomed.push_back(slot.pass.get());
        }
    }
    for (raw_ptr<RenderPass> pass : doomed) {
        removePass(pass);
    }
}

std::vector<intrusive_ptr<RenderPass>> RenderEngine::passesOwnedBy(const Object* owner) const
{
    std::vector<intrusive_ptr<RenderPass>> passes;
    if (owner == nullptr) {
        return passes;
    }
    for (const Slot& slot : slots_) {
        if (slot.owner == owner && slot.pass != nullptr) {
            passes.push_back(slot.pass);
        }
    }
    return passes;
}

raw_ptr<RenderPass> RenderEngine::passOwnedBy(const Object* owner, const String& role) const
{
    if (owner == nullptr) {
        return nullptr;
    }
    for (const Slot& slot : slots_) {
        if (slot.owner == owner && slot.pass != nullptr && slot.role == role) {
            return slot.pass.get();
        }
    }
    return nullptr;
}

VN_GRAPHICS_NS_END
