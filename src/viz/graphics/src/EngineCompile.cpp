#include "SessionShape.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <string>
#include <typeinfo>
#include <vector>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/ImageRef.hpp>
#include <vine/graphics/PassDesc.hpp>
#include <vine/graphics/Plan.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ScreenPass.hpp>
#include <vine/graphics/Viewport.hpp>

#include "EngineDiagnostics.hpp"
#include "FrameExpand.hpp"
#include "FrameResources.hpp"
#include "FrameScheduler.hpp"
#include "PublicationStore.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

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

/**
 * @brief Builds one compile problem: a stable code, its class, the sentence and (when there is one) the pass.
 *
 * @details
 * - A COMPILE PROBLEM IS ALWAYS A WARNING with a code: the frame still runs (the pass is skipped), and the code is
 *   what a host matches on (`engine.compile.*`, see scripts/diagnostic_codes.txt).
 * - The SUBJECT is the pass the problem is about, when there is exactly one: that is what lets a host badge the pass
 *   rather than a log line.
 *
 * @param code     Stable identifier of the cause.
 * @param category What it is about.
 * @param message  The sentence (it names its subject too, because that is the problem's identity to the episode rule).
 * @param subject  The pass it is about, or empty when the problem spans several.
 * @return The diagnostic a report would carry.
 */
[[nodiscard]] vn::graphics::RenderDiagnostic compileProblem(const char8_t*                        code,
                                                           const vn::graphics::DiagnosticCategory category,
                                                           vn::String                             message,
                                                           vn::String                             subject = {})
{
    return detail::engineProblem(code, category, std::move(message), std::move(subject));
}

} // namespace

void SessionShape::compile(EngineFrame& frame, RenderBackend* device)
{
    // The compile is one call with two borrowed facts: the frame value it fills and the device a replaced transient
    // is released through. Neither outlives the call (see the class note).
    frame_  = &frame;
    device_ = device;
    compileFrame();
    // The problems the compile found are reported once per episode: what the previous compile already found is not
    // said again, and what is gone is forgotten (see reportPlanProblems).
    reportPlanProblems();
    frame_  = nullptr;
    device_ = nullptr;
}

void SessionShape::compileFrame()
{
    // THIS IS THE FRAME. Everything the frame needs to know is derived here, from the declarations as they
    // stand right now: which pass runs when (the slot list is already ordered), what each declared input
    // resolves to (what ran BEFORE it - the same rule the runtime used to apply as it went), which transient
    // targets exist, and what is wrong with the set. The tables are reused, so a steady frame allocates
    // nothing beyond what the compile's own bookkeeping already has capacity for.
    plan_problems_.clear();
    plan_rows_.clear();
    resources_.beginFrame();
    unusable_inputs_this_frame_.clear();
    // From here until the list is complete, every report the checks make is COLLECTED, not emitted: the
    // episode rule is applied once, at the end (see reportPlanProblems).
    collecting_problems_ = true;

    // A host binding is filled by the HOST: it is there from the start of the frame and no pass owns its
    // hand-off, so it seeds what this frame has already produced (an object-typed input addressing one is
    // answered like one addressing a pass' target).
    resources_.seedFromHostBindings();

    // Transients first: a slot that declares one gets its target before anything reads a target, and one
    // nobody declares this frame is let go of.
    resources_.beginTransientCompile();
    for (Slot& slot : slots_) {
        if (slot.pass != nullptr && slot.pass->declaration().target.isTransient()) {
            resolveTransientTarget(slot);
        }
    }
    resources_.retireUndeclared(device_);

    // The structural checks read DECLARATIONS only, so they run before anything is resolved: a wire the check
    // already spoke about is one problem, and the resolution below stays quiet about it (see unusableInputReported).
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
            resources_.recordProduced(drawn_into);
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

void SessionShape::scheduleRows()
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
        if (const auto* screen = obj_cast<ScreenPass>(&pass)) {
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
        noteProblem(compileProblem(u8"engine.compile.cycle",
                                   vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                   String(u8"passes ") + members +
                                       String(u8" declare a dependency cycle (each reads what another writes), which no execution order "
                                              u8"can satisfy: those passes are skipped this frame, and the rest of the frame still runs")));
    }

    // A row that does not run this frame (disabled or cyclic) has no timing to report.
    for (Slot& slot : slots_) {
        slot.record_micros = 0;
    }
}

void SessionShape::buildFrameValue()
{
    // EVERY ENABLED PASS HAS AN ENTRY, whether it issues anything or not: "this pass ran and had nothing to
    // issue" is a fact of the frame (its scope opens and closes, and a backend keys per-pass retained state by
    // it), which is not the same fact as "this pass did not run" - a disabled pass has no entry at all.
    //
    // The entries are REUSED rather than rebuilt (see below): a steady frame hands over the same pass list with the
    // same input lists, so it must not free and re-allocate them 60 times a second.
    std::size_t used = 0;
    // Which entries the expansion has to answer, in run order (the passes whose description issued something). It
    // lives on the FRAME, so the two halves share no mutable state: this half fills it, the engine half walks it.
    frame_->engine_order.clear();
    frame_->engine_micros.clear();
    // A window pass has no target to read a device-pixel size from. The frame states the surface size (converted
    // with the surface's pixel ratio, taken when the frame started - see EngineFrame) on every pass;
    // RenderPass::describe prefers a sub-viewport or the target's own size, so off-screen passes are unaffected.
    const double pixel_ratio   = frame_->pixel_ratio;
    const int    device_width  = static_cast<int>(std::lround(static_cast<double>(frame_->width) * pixel_ratio));
    const int    device_height = static_cast<int>(std::lround(static_cast<double>(frame_->height) * pixel_ratio));
    // Rows and slots are one per index (see compileFrame): the row is the frame's answer about a pass, the slot
    // is the declaration itself.
    //
    // PER-FRAME CONTENT WORK BELONGS TO THE OBJECT THAT DOES IT (see FrameUpdate): an overlay that repaints itself,
    // a gizmo that mirrors a camera, runs its own update() - the host/chain calls it before this frame is built
    // (SceneView::update), and what it publishes is the value below describes. The frame itself only describes and
    // expands, which is what `record_micros` measures.

    // THE DESCRIPTIONS, each bound to the publication that stands NOW. Nothing here publishes: publishing is the
    // content writer's verb (`Scene::publish`), and the frame takes the value it finds - so a host controls the picture
    // by publishing, and a publication that lands after this frame was built cannot change what it draws.
    for (const std::uint32_t index : frame_order_) {
        PlanRow&          row  = plan_rows_[index];
        RenderPass* const pass = row.pass;
        if (pass == nullptr || !pass->isEnabled()) {
            continue; // the scheduler never puts a pass that does not run into the order
        }
        // THIS IS WHAT A PASS COSTS THE FRAME. Under a value-driven frame the work is here: the publication (for the
        // pass that names the scene first), the inputs told, the pass stating its VIEW, and the frame's expansion
        // collecting it (see FrameExpand) - one measured interval, so the number keeps meaning what it always meant.
        // The number is what the table reports per pass (see PassRow::record_micros), which is the
        // host-visible half of a frame; the GPU half is the backend's (see RenderEngine::frameReport). One clock read
        // per pass, not per draw.
        const auto pass_started = std::chrono::steady_clock::now();
        // The declared inputs were resolved by the compile (see compileFrame's rows), and the pass is told them
        // here: they are what a ScreenPass' effect reads, and they are part of what the pass describes.
        pass->resolveInputTextures(row.inputs);

        // Reuse the entry this index had last frame: the input list keeps its capacity (assign, not a fresh copy) and
        // an already-described pass keeps its description's draw list, which fillPassFacts clears but does not free.
        if (used == frame_->description.passes.size()) {
            frame_->description.passes.emplace_back();
        }
        FrameDescription::Pass& entry = frame_->description.passes[used++];
        entry.pass                    = pass;
        entry.order                   = row.order;
        entry.inputs.assign(row.inputs.begin(), row.inputs.end());
        if (!entry.draw.has_value()) {
            entry.draw.emplace();
        }
        detail::PassNode::setDrawSize(*pass, device_width, device_height);
        if (!pass->describe(row.content, *entry.draw)) {
            // "This pass issued nothing" must not leave the previous frame's picture behind: the draw goes away and
            // the entry keeps the fact that the pass ran (see the note at the top).
            entry.draw.reset();
        }
        else {
            // THE BOUNDARY THAT SAYS WHOSE PUBLICATION ANSWERS THE VIEW: the pass stated a view, the content's write is
            // already published (see the publication step above), and this is where the frame value stops naming the
            // content and starts naming its rows (see detail::bindSceneDraws). A pass with no content binds nothing -
            // its draws are already what it described.
            if (row.content != nullptr) {
                const SceneSnapshotRef publication = publications_.publicationFor(*row.content);
                detail::SnapshotSource* const source = publications_.sourceFor(*row.content, publication);
                detail::bindSceneDraws(*entry.draw, source);
                if (source == nullptr) {
                    // "You drew a scene that never published": reported once per episode, and the pass draws nothing -
                    // the honest answer, instead of quietly reading the live tree.
                    noteProblem(compileProblem(u8"engine.compile.unpublished_scene",
                                               vn::graphics::DiagnosticCategory::ContentSkipped,
                                               String(u8"pass '") + reportedPassName(pass)
                                                   + String(u8"' draws a scene that was never published: publish it (Scene::publish), or bind a"
                                                            u8" publication before the frame (RenderEngine::bindSnapshot)"),
                                               reportedPassName(pass)));
                }
            }
            // THE ANSWER IS NOT TAKEN HERE. The pass stated a view and whose content answers it (and the content's
            // write is already published); asking that source for the list is the ENGINE half's step, which runs
            // over the whole described frame (see expandFrameValue) - that is the seam a thread crosses, and it is
            // why it is a function of its own rather than a line in this loop.
            frame_->engine_order.push_back(index);
        }
        if (index < slots_.size()) {
            slots_[index].record_micros +=
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - pass_started).count());
        }
    }
    // A frame with fewer running passes than the last one drops the surplus entries (the only frame that frees
    // anything here, and it is a frame whose shape changed).
    if (frame_->description.passes.size() > used) {
        frame_->description.passes.resize(used);
    }
}

void SessionShape::collectPictureShapeMismatches()
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
        if (obj_cast<ScreenPass>(pass) != nullptr) {
            continue;
        }

        // The rectangle: the pass' viewport when it named one, else the whole thing it draws into. A window pass
        // (no render target) draws into the surface this frame presents, which is what its rectangle is sized
        // from; a surface nobody has sized yet describes no picture, so there is nothing to check.
        int width  = frame_->width;
        int height = frame_->height;
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
            noteProblem(compileProblem(u8"engine.compile.picture_shape_mismatch",
                                       vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                       String(u8"pass '") + reportedPassName(pass) + String(u8"' draws into a ") + String(toU8(size_text)) +
                                           String(u8" rectangle, but its camera is projected for a picture "
                                                  u8"of ") +
                                           String(toU8(shapeText(picture))) +
                                           String(u8" - the picture is stretched (the content was culled for one shape and lands in "
                                                  u8"another). Give the camera the shape of the rectangle it draws into "
                                                  u8"(Camera::setAspect), or draw it into the rectangle the camera is shaped for"),
                                       reportedPassName(pass)));
        }
        shape_mismatches_this_frame_.push_back(std::move(episode));
    }

    shape_mismatches_last_frame_.clear();
    shape_mismatches_this_frame_.swap(shape_mismatches_last_frame_);
    shape_mismatches_this_frame_.clear();
}

void SessionShape::resolveTransientTarget(Slot& slot)
{
    // The extent: what the declaration states, or the surface this frame is presenting (a transient is
    // normally the size of the picture that consumes it). A surface nobody has sized yet is not a reason to
    // build nothing: a 1x1 target keeps the pass' shape defined until the host reports a size.
    const Target&         declared = slot.pass->declaration().target;
    const TransientShape  shape    = declared.shape.value_or(TransientShape{});
    int                   width    = declared.extent.has_value() ? declared.extent->width : frame_->width;
    int                   height   = declared.extent.has_value() ? declared.extent->height : frame_->height;
    if (width <= 0 || height <= 0) {
        width  = 1;
        height = 1;
    }

    slot.transient = resources_.resolveTransient(declared.name, shape, width, height, device_);
    // THE RESOLUTION IS EXECUTION STATE, not a rewrite of the declaration: the pass keeps saying "I asked for this
    // identity" while the frame that resolved it answers `renderTarget()` (see detail::PassNode::resolveTarget).
    detail::PassNode::resolveTarget(*slot.pass, slot.transient);
}

void SessionShape::resolveRowInputs(raw_ptr<RenderPass> pass, std::vector<ImageSource>& out)
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
        raw_ptr<RenderTarget> resolved = resources_.resolve(name);
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
    noteProblem(compileProblem(u8"engine.compile.input_not_produced",
                               vn::graphics::DiagnosticCategory::ContentSkipped,
                               String(u8"pass '") + reportedPassName(pass) + String(u8"' declared input '") + names.front() +
                                   String(u8"' but no pass published it this frame; the pass draws nothing"
                                          u8" (check the producer's order, its enabled state and its output name)"),
                               reportedPassName(pass)));
}

void SessionShape::publishRowOutput(raw_ptr<RenderPass> pass)
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
        noteProblem(compileProblem(u8"engine.compile.output_to_window",
                                   vn::graphics::DiagnosticCategory::ContentSkipped,
                                   String(u8"pass '") + reportedPassName(pass) + String(u8"' publishes the output name '") + name +
                                       String(u8"' but renders into the window (no render target), so nothing is"
                                              u8" published and a consumer of '") +
                                       name +
                                       String(u8"' finds nothing — give the pass a render target, or drop the"
                                              u8" output name"),
                                   reportedPassName(pass)));
        return;
    }
    if (detail::FramePublication* published = resources_.findPublication(name); published != nullptr) {
        if (published->target != nullptr && published->target != target) {
            // Two passes published DIFFERENT targets under one name: every consumer of that name silently gets
            // whichever pass ran last, and only the whole table can see the collision (each pass is
            // individually valid).
            const String first  = published->target->name().empty() ? String(u8"(unnamed)") : published->target->name();
            const String second = target->name().empty() ? String(u8"(unnamed)") : target->name();
            noteProblem(compileProblem(u8"engine.compile.duplicate_output",
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       String(u8"pass output '") + name + String(u8"' is published by two passes this frame (targets '") + first +
                                           String(u8"' and '") + second + String(u8"'); every consumer of '") + name + String(u8"' samples whichever pass ran last")));
        }
        published->target = target; // the latest publication serves the name for the rest of the frame
        return;
    }
    resources_.recordPublication(pass, target);
}

void SessionShape::noteProblem(const vn::graphics::RenderDiagnostic& diagnostic)
{
    // One problem per sentence per frame: the checks visit a subject once, but a subject can be reached from
    // more than one direction (a second and a third claimant of one image), and one mistake should read as one
    // message. The episode rule decides whether it is said out loud at all (see reportPlanProblems).
    const auto known = std::find_if(plan_problems_.begin(), plan_problems_.end(), [&diagnostic](const CompileProblem& problem) {
        return problem.diagnostic.message == diagnostic.message;
    });
    if (known == plan_problems_.end()) {
        plan_problems_.push_back(CompileProblem{ diagnostic });
    }
}

bool SessionShape::unusableInputReported(raw_ptr<const RenderPass> pass, const OutputIdentity& identity) const noexcept
{
    const auto key = std::make_pair(pass, identity);
    return std::any_of(unusable_inputs_this_frame_.begin(),
                       unusable_inputs_this_frame_.end(),
                       [&key](const std::pair<raw_ptr<const RenderPass>, OutputIdentity>& entry) {
                           return entry.first == key.first && entry.second.target == key.second.target && entry.second.attachment == key.second.attachment &&
                                  entry.second.depth == key.second.depth && entry.second.declared == key.second.declared;
                       });
}

void SessionShape::reportPlanProblems()
{
    for (const CompileProblem& problem : plan_problems_) {
        const auto previous = std::find_if(previous_problems_.begin(), previous_problems_.end(), [&problem](const CompileProblem& earlier) {
            return problem.diagnostic.message == earlier.diagnostic.message;
        });
        if (previous != previous_problems_.end()) {
            continue; // the same problem, in the same words: this episode has already been said
        }
        // THE WHOLE DIAGNOSTIC IS RE-EMITTED: the code, the category and the subject the check stated survive the
        // episode rule - flattening them here is what used to turn every compiled problem into "content skipped".
        diagnostics_.report(problem.diagnostic);
    }
    // What this compile found becomes what the next compile compares against: a problem that is gone drops out,
    // so the same breakage later is a new episode.
    previous_problems_.swap(plan_problems_);
    plan_problems_.clear();
}

raw_ptr<RenderTarget> SessionShape::resolveDeclaredTarget(raw_ptr<RenderPass> pass, raw_ptr<RenderTarget> target)
{
    // Resolved against what the frame has produced so far (FrameResources): a target the frame fills LATER is
    // not something this pass can read, which is the same answer the per-pass resolution used to give.
    if (resources_.wasProduced(static_cast<raw_ptr<const RenderTarget>>(target))) {
        return target;
    }
    // A pass reading what it draws into is the feedback pattern, and whether the actual draw call is
    // a feedback loop is the BACKEND's to judge (the vsg backend rejects source == destination and
    // says so): this layer stays quiet about it, so one mistake is reported once.
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass,
                          OutputIdentity::targetOf(*target),
                          String(u8"the input target '") + (target->name().empty() ? String(u8"(unnamed target)") : target->name()) + u8"'");
    return nullptr;
}

raw_ptr<RenderTarget> SessionShape::resolveDeclaredImage(raw_ptr<RenderPass> pass, const ImageRef& image)
{
    raw_ptr<RenderTarget> target = image.target();
    if (target == nullptr) {
        // An unbound declaration names nothing that can be read this frame. The wiring check already
        // reports the declaration itself; nothing more to say per frame.
        return nullptr;
    }
    const bool depth_usable = (image.kind() != ImageRef::Kind::Depth) || target->hasDepth();
    const bool produced     = resources_.wasProduced(static_cast<raw_ptr<const RenderTarget>>(target));
    if (depth_usable && produced) {
        return target;
    }
    // Same as above: a pass that reads the target it draws into is the feedback pattern, judged by
    // the backend (the depth case included — its layout is the backend's business).
    if (pass->renderTarget() == target) {
        return nullptr;
    }
    reportUnproducedInput(pass, OutputIdentity::of(image), String(u8"the input image '") + image.label() + u8"'");
    return nullptr;
}

void SessionShape::reportUnproducedInput(raw_ptr<RenderPass> pass, const OutputIdentity& identity, const String& what)
{
    // A wire the structural check already reported THIS compile (no producer at all, or one registered too
    // late) is ONE problem: the resolution does not repeat it in frame terms. Both are collected by the same
    // compile (see collectPlanProblems), so this is a question about this frame, not about the last episode.
    if (unusableInputReported(pass, identity)) {
        return;
    }
    noteProblem(compileProblem(u8"engine.pass.input_not_produced",
                               vn::graphics::DiagnosticCategory::ContentSkipped,
                               String(u8"pass '") + reportedPassName(pass) + String(u8"' declares ") + what +
                                   String(u8" but nothing produced it this frame, so the pass draws nothing"
                                          u8" (check the producer's order and its enabled state)")));
}

SessionShape::OutputIdentity SessionShape::OutputIdentity::of(const ImageRef& image) noexcept
{
    OutputIdentity identity;
    identity.target     = image.target();
    identity.attachment = image.attachment();
    identity.depth      = (image.kind() == ImageRef::Kind::Depth);
    // Only while there is no address: the object is all the identity that exists yet.
    identity.declared   = (identity.target == nullptr) ? &image : nullptr;
    return identity;
}

SessionShape::OutputIdentity SessionShape::OutputIdentity::colorOf(const RenderTarget& target, int attachment) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = attachment;
    return identity;
}

SessionShape::OutputIdentity SessionShape::OutputIdentity::depthOf(const RenderTarget& target) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = 0; // a depth attachment has no colour index; `depth` is what tells them apart
    identity.depth      = true;
    return identity;
}

SessionShape::OutputIdentity SessionShape::OutputIdentity::targetOf(const RenderTarget& target) noexcept
{
    OutputIdentity identity;
    identity.target     = &target;
    identity.attachment = kWholeTarget;
    return identity;
}

bool SessionShape::OutputIdentity::operator==(const OutputIdentity& other) const noexcept
{
    return target == other.target && attachment == other.attachment && depth == other.depth && declared == other.declared;
}

bool SessionShape::OutputIdentity::operator<(const OutputIdentity& other) const noexcept
{
    if (target != other.target) {
        // std::less, not the built-in `<`: comparing unrelated pointers with `<` is unspecified,
        // and these keys are addresses of distinct objects (as CollisionPair does for pairs).
        return std::less<const void*>{}(target, other.target);
    }
    if (target == nullptr) {
        // Both unbound: the declared object is the only identity there is.
        return std::less<const void*>{}(declared, other.declared);
    }
    if (depth != other.depth) {
        return depth < other.depth;
    }
    return attachment < other.attachment;
}

void SessionShape::collectPlanProblems()
{
    // Every pass on its own is valid, so only the whole table can see these; and they are properties of the
    // DECLARATION, not of a frame's outcome. Run as part of the compile, so the answer is about THIS frame;
    // what is REPORTED is decided once, by comparing this list with the previous compile's (see
    // reportPlanProblems) - the checks below only say what they found.
    //
    // Two phases, because a consumer may be registered before its producer: the producers have to be collected
    // before any consumer is judged against them.
    const auto target_name = [](raw_ptr<const RenderTarget> target) -> String {
        if (target == nullptr) {
            return String(u8"(no target)");
        }
        return target->name().empty() ? String(u8"(unnamed target)") : target->name();
    };
    /// What a pass writes, for a message about a promise it cannot keep (null render target = the
    /// window, which is a wire of its own).
    const auto drawn_into_name = [&target_name](raw_ptr<const RenderPass> pass) -> String {
        raw_ptr<const RenderTarget> drawn_into = pass->renderTarget();
        return (drawn_into == nullptr) ? String(u8"the window") : target_name(drawn_into);
    };
    const auto identity_name = [&target_name](const OutputIdentity& identity) -> String {
        if (identity.attachment == OutputIdentity::kWholeTarget) {
            return String(u8"the images of target '") + target_name(identity.target) + u8"'";
        }
        if (identity.depth) {
            return String(u8"the depth of target '") + target_name(identity.target) + u8"'";
        }
        return String(u8"an image of target '") + target_name(identity.target) + u8"'";
    };

    /// Who fills what: the lowest-indexed pass that renders into a target or promises the whole of
    /// it. Keyed by TARGET, because filling a target fills every image it has. A DISABLED pass still
    /// declares its wire: toggling a pass off is not a wiring mistake, it is a paused pass.
    /// Who claims which image: a fine promise, and the images of a coarse one. Two claimants of one
    /// image is what the collision report is about.
    /// Both tables (and the collision/unbound ones below) are the ENGINE's, cleared here: the pass count is small, so a
    /// flat scan both beats a tree and keeps its capacity - a steady frame allocates nothing while it checks the wiring.
    auto& filled           = wiring_filled_;
    auto& claimed          = wiring_claimed_;
    auto& colliding_images = wiring_colliding_;
    auto& unbound_declared = wiring_unbound_;
    filled.clear();
    claimed.clear();
    colliding_images.clear();
    unbound_declared.clear();

    // First wins in every table, exactly as the keyed containers did: a second filler of one target, or a second
    // claimant of one image, is a fact the reports below are about, not a replacement.
    const auto findFilled = [this](raw_ptr<const RenderTarget> target) -> const Declaration* {
        const auto it = std::find_if(wiring_filled_.begin(), wiring_filled_.end(), [target](const Declaration& entry) { return entry.target == target; });
        return it == wiring_filled_.end() ? nullptr : &*it;
    };
    const auto noteFilled = [this, &findFilled](Declaration declaration) {
        if (findFilled(declaration.target) == nullptr) {
            wiring_filled_.push_back(std::move(declaration));
        }
    };
    const auto findClaimed = [this](const OutputIdentity& identity) -> const Declaration* {
        const auto it =
            std::find_if(wiring_claimed_.begin(), wiring_claimed_.end(), [&identity](const Declaration& entry) { return entry.identity == identity; });
        return it == wiring_claimed_.end() ? nullptr : &*it;
    };
    const auto noteColliding = [this](const OutputIdentity& identity) {
        const auto known =
            std::any_of(wiring_colliding_.begin(), wiring_colliding_.end(), [&identity](const OutputIdentity& entry) { return entry == identity; });
        if (!known) {
            wiring_colliding_.push_back(identity);
        }
    };
    const auto isColliding = [this](const OutputIdentity& identity) {
        return std::any_of(wiring_colliding_.begin(), wiring_colliding_.end(), [&identity](const OutputIdentity& entry) { return entry == identity; });
    };
    const auto noteUnbound = [this](raw_ptr<const ImageRef> image, raw_ptr<const RenderPass> pass, bool as_output) {
        const auto known =
            std::any_of(wiring_unbound_.begin(), wiring_unbound_.end(), [image](const UnboundDeclaration& entry) { return entry.image == image; });
        if (!known) {
            wiring_unbound_.push_back(UnboundDeclaration{ image, pass, as_output });
        }
    };

    // Phase 1: who fills and who claims. The fillers come from ALL passes (a declaration is a
    // declaration), the claims from the ENABLED ones only — what the collision report is about is
    // "whoever runs last silently wins", and a disabled pass runs nothing.
    //
    // A host binding (publish) is filled by the HOST: it is there from the start of the frame and no
    // pass owns its hand-off, so a declared input addressing it is answered like one addressing a
    // pass' target.
    for (const auto& binding : resources_.hostBindings()) {
        if (binding.second != nullptr) {
            noteFilled(Declaration{ nullptr, binding.second.get(), nullptr, 0 });
        }
    }

    for (std::size_t index = 0; index < slots_.size(); ++index) {
        RenderPass* pass = slots_[index].pass.get();
        if (pass == nullptr) {
            continue;
        }

        // THE DECLARATION'S OWN SHAPE (see Target / Output): a value that states two forms at once is a
        // declaration mistake, reported here instead of letting one of them win silently.
        if (!pass->declaration().target.isValid()) {
            noteProblem(compileProblem(u8"engine.pass.target_invalid",
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       String(u8"pass '") + reportedPassName(pass) +
                                           String(u8"' states a target that mixes its forms: a Target is the "
                                                  u8"window, one explicit target, or one transient name - not several"),
                                       reportedPassName(pass)));
        }
        if (!pass->declaration().output.isValid()) {
            noteProblem(compileProblem(u8"engine.pass.output_invalid",
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       String(u8"pass '") + reportedPassName(pass) +
                                           String(u8"' promises both a whole target and one image for its output; "
                                                  u8"one name covers one claim"),
                                       reportedPassName(pass)));
        }

        // Drawing into a target fills it: every attachment it has holds this pass' content
        // afterwards. This is the physical fact the input rules below are answered from.
        if (raw_ptr<RenderTarget> drawn_into = pass->renderTarget(); drawn_into != nullptr) {
            noteFilled(Declaration{ nullptr, drawn_into, pass, index });
        }

        // A content pass whose mask selects nothing. Legal (see PassDesc::layer_mask), and nearly always a
        // mistake, so it is said once per episode instead of showing up later as an empty picture with no
        // reason attached. The sentence states the declaration, not an outcome: what a pass does with a
        // content scene is the pass' own business (a screen pass uses one for its lights, not for a walk).
        if (pass->layerMask() == 0 && pass->content() != nullptr) {
            noteProblem(compileProblem(u8"engine.pass.layer_mask_empty",
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       String(u8"pass '") + reportedPassName(pass) +
                                           String(u8"' has a layer mask of 0, so no layer of its content is selected —"
                                                  u8" state the layers it draws (RenderPass::setLayerMask / Node::setLayers)"),
                                       reportedPassName(pass)));
        }

        if (!pass->isEnabled()) {
            continue;
        }

        // A promise that names a WHOLE target: it claims every image of the target the pass DRAWS INTO (see
        // Output::whole_target). A window pass has no target, so its promise is about nothing and is reported.
        if (pass->declaration().output.whole_target && pass->renderTarget() == nullptr) {
            noteProblem(compileProblem(u8"engine.pass.output_target_mismatch",
                                       vn::graphics::DiagnosticCategory::ContentSkipped,
                                       String(u8"pass '") + reportedPassName(pass) +
                                           String(u8"' promises the whole of its target as its output but draws into "
                                                  u8"the window, so the promise is about content this pass never writes"),
                                       reportedPassName(pass)));
        }
        if (raw_ptr<RenderTarget> promised = pass->outputTarget(); promised != nullptr) {
            noteFilled(Declaration{ nullptr, promised, pass, index });

            std::vector<OutputIdentity>& promised_images = wiring_promised_images_;
            promised_images.clear();
            promised_images.reserve(static_cast<std::size_t>(promised->colorCount()) + 1);
            for (int attachment = 0; attachment < promised->colorCount(); ++attachment) {
                promised_images.push_back(OutputIdentity::colorOf(*promised, attachment));
            }
            if (promised->hasDepth()) {
                // The depth of a promised target is part of the promise, and it is a DIFFERENT
                // image from colour attachment 0 (which is why the identity carries the kind).
                promised_images.push_back(OutputIdentity::depthOf(*promised));
            }

            for (const auto& identity : promised_images) {
                const Declaration* entry = findClaimed(identity);
                if (entry == nullptr) {
                    claimed.push_back(Declaration{ nullptr, promised, pass, index, identity });
                    continue;
                }
                if (entry->pass == pass) {
                    continue; // one pass claiming twice is one claim
                }
                noteColliding(identity);
                {
                    noteProblem(compileProblem(u8"engine.pass.output_collision",
                                               vn::graphics::DiagnosticCategory::ContentSkipped,
                                               String(u8"two passes ('") + reportedPassName(entry->pass) + String(u8"' and '") + reportedPassName(pass) +
                                                   String(u8"') claim ") + identity_name(identity) +
                                                   String(u8" as their output, so a consumer of it gets whichever pass"
                                                          u8" runs last"),
                                               String{}));
                }
            }
        }

        // A fine promise that names ONE image: the address is the claim, so two passes that each
        // declared their OWN ImageRef for one image are caught as well — the host may build its
        // wiring out of two references to the same attachment and never notice.
        if (raw_ptr<ImageRef> output = pass->output(); output != nullptr) {
            // An UNBOUND promise cannot be checked at all: there is no target to compare with the
            // one this pass renders into, so a promise about nothing would pass unnoticed — and no
            // consumer could resolve it either (an unbound identity has no address). Reported once
            // per image, here (the producer side) so the consumer side below stays quiet about the
            // same object.
            if (!output->isBound()) {
                // Recorded, not reported yet: if another pass also claims this image, the collision
                // report below names both passes and the image, and binding the image alone would not
                // fix a double claim — one mistake, one message. Phase 4 reports the rest.
                noteUnbound(output, pass, true);
            }
            // Same rule as the coarse form: a fine promise is about a target this pass has to write.
            raw_ptr<RenderTarget> promised_target = output->target();
            if (promised_target != nullptr && pass->renderTarget() != promised_target) {
                const OutputIdentity identity = OutputIdentity::of(*output);
                {
                    noteProblem(compileProblem(u8"engine.pass.output_image_mismatch",
                                               vn::graphics::DiagnosticCategory::ContentSkipped,
                                               String(u8"pass '") + reportedPassName(pass) + String(u8"' promises the image '") + output->label() +
                                                   String(u8"' of target '") + target_name(promised_target) + String(u8"' as its output but renders into '") +
                                                   drawn_into_name(pass) + String(u8"', so the promise is about content this pass never writes"),
                                               reportedPassName(pass)));
                }
            }
            const OutputIdentity identity = OutputIdentity::of(*output);
            const Declaration*   entry    = findClaimed(identity);
            if (entry == nullptr) {
                claimed.push_back(Declaration{ output, output->target(), pass, index, identity });
            }
            else if (entry->pass != pass) {
                noteColliding(identity);
                {
                    const String first  = reportedPassName(entry->pass);
                    const String second = reportedPassName(pass);
                    // One format string per branch: the cases do not print the same thing (one image
                    // declared twice, two images that turn out to be one attachment, or an image
                    // claimed next to a whole target).
                    const String message =
                        (entry->image == output) ? String(u8"image '") + output->label() + String(u8"' is declared as the output of two passes ('") + first +
                                                       String(u8"' and '") + second + String(u8"'); a consumer of it gets whichever pass runs last")
                        : (entry->image != nullptr)
                            ? String(u8"two passes ('") + first + String(u8"' and '") + second + String(u8"') declared DIFFERENT images ('") +
                                  entry->image->label() + String(u8"' and '") + output->label() +
                                  String(u8"') that are the same attachment of one target; a consumer of it"
                                         u8" gets whichever pass runs last")
                            : String(u8"two passes ('") + first + String(u8"' and '") + second + String(u8"') claim the image '") + output->label() +
                                  String(u8"' of target '") + target_name(output->target()) +
                                  String(u8"', one of them as part of the whole target; a consumer of it gets"
                                         u8" whichever pass runs last");
                    noteProblem(detail::engineProblem(u8"engine.pass.output_collision", vn::graphics::DiagnosticCategory::ContentSkipped, message));
                }
            }
        }
    }

    // Phase 2: every declared input its consumer cannot draw from — nobody fills the image (or the
    // whole target), or the image asked for is a depth the target does not have. WHERE the producer was
    // registered is no longer a wiring fact: the scheduler orders a later-announced producer first (see
    // scheduleRows), so registration order says nothing about execution order.
    for (const Slot& slot : slots_) {
        RenderPass* pass = slot.pass.get();
        if (pass == nullptr) {
            continue;
        }

        for (const auto& target : pass->inputTargets()) {
            if (target == nullptr) {
                continue;
            }
            const OutputIdentity identity = OutputIdentity::targetOf(*target);
            const Declaration*   entry    = findFilled(target.get());
            // A pass reading what it draws into is a feedback loop the backend supports on purpose
            // (an overlay reading the frame it is about to composite into): never a bogus wire.
            if (entry != nullptr && entry->pass == pass) {
                continue;
            }
            if (entry != nullptr) {
                continue; // someone fills it; the scheduler puts that pass before this one
            }
            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                const String message  = String(u8"pass '") + consumer + String(u8"' declares the input target '") + target_name(target.get()) +
                                        String(u8"' but no pass writes it, so nothing fills it");
                noteProblem(detail::engineProblem(u8"engine.pass.input_target_not_produced", vn::graphics::DiagnosticCategory::ContentSkipped, message));
            }
        }

        for (const ImageInput& input : pass->inputs()) {
            raw_ptr<ImageRef> image = input.image();
            if (image == nullptr) {
                continue; // an imported texture has no producer/wiring rule to check
            }
            const OutputIdentity identity = OutputIdentity::of(*image);
            // An UNBOUND image can never be delivered (resolveDeclaredImage returns null for it, and
            // deliberately says nothing — declaring it is what is wrong). Reported once per IMAGE:
            // by the producer side above when a pass declares it as its output, otherwise here.
            if (!image->isBound()) {
                noteUnbound(image, pass, false);
                continue;
            }
            // An unbound image has no address, so no target account can say anything about it: only
            // a promise of that same object can.
            const Declaration* claim = findClaimed(identity);
            const Declaration* entry = (identity.target != nullptr) ? findFilled(identity.target) : nullptr;

            // A pass that promises the very image it reads, or draws into its target, is a feedback
            // loop the backend supports on purpose (an overlay reading the frame it composites
            // into): never a bogus wire.
            if ((claim != nullptr && claim->pass == pass) || (entry != nullptr && entry->pass == pass)) {
                continue;
            }

            // Whoever can fill the image: a pass that promises this very image, a pass that draws into its target
            // (drawing into a target fills all of its images), or the host (a host binding is filled by the host).
            // WHERE that pass sits in registration order is not a question any more: the scheduler orders it.
            const bool has_filler = claim != nullptr || entry != nullptr;

            if (has_filler && (!identity.depth || identity.target->hasDepth())) {
                continue; // filled, by a pass the scheduler orders before this one (or by the host)
            }

            // The structural check speaks for this wire; the collect keeps its subject so the RESOLUTION of the
            // same input stays quiet about it (see unusableInputReported / reportUnproducedInput).
            unusable_inputs_this_frame_.emplace(pass, identity);
            {
                const String consumer = reportedPassName(pass);
                // Two problems, two fixes: nobody declares it, or the target simply has no depth.
                const String message  = !has_filler ? String(u8"pass '") + consumer + String(u8"' declares the input image '") + image->label() +
                                                          String(u8"' but no pass declares it as an output, so nothing fills it")
                                                    : String(u8"pass '") + consumer + String(u8"' declares the input image '") + image->label() +
                                                          String(u8"' (the depth of target '") + target_name(identity.target) +
                                                          String(u8"') but that target has no depth attachment, so nothing fills it");
                noteProblem(detail::engineProblem(u8"engine.pass.input_image_not_produced", vn::graphics::DiagnosticCategory::ContentSkipped, message));
            }
        }
    }

    // Phase 3: a ScreenPass that cannot draw what it declares. Every one of these is a property of
    // the DECLARATION (nothing else can see them: the backend only ever sees "a pass with nothing to
    // draw"), they are found by the compile (so the frame they belong to is the frame that says so), and
    // they are checked in the order a host has to fix them — each check `continue`s, because the next one is
    // moot until this one is answered and one mistake should read as one message.
    for (const auto& slot : slots_) {
        auto* pass = obj_cast<ScreenPass>(slot.pass.get());
        if (pass == nullptr || !pass->isEnabled()) {
            continue;
        }

        // 1. No program: the shading is a CHOICE the host makes (a ScreenPass has no implicit one —
        // there is no "plain copy" a backend fills in), so a pass without a program draws nothing.
        // This is the first thing to answer, and it is the one a host is most likely to hit: the
        // picture a pass draws is exactly what it named.
        if (pass->program() == nullptr) {
            {
                noteProblem(detail::engineProblem(u8"engine.screen.no_program",
                                                  vn::graphics::DiagnosticCategory::ContentSkipped,
                                                  String(u8"pass '") + reportedPassName(pass) +
                                                      String(u8"' is a ScreenPass with no program, so it can never draw: name one"
                                                             u8" (BuiltinShaders::screenCopyProgram() for a plain copy of the"
                                                             u8" source's colour attachment, deferredLightProgram() for deferred"
                                                             u8" lighting, or your own fragment stage)")));
            }
            continue;
        }

        // 2. No input at all (no image, no target, no name): it samples one input and returns without
        // it. Not "an input that failed to resolve", nor "an input nobody fills" (phase 2).
        if (pass->inputs().empty() && pass->inputTargets().empty() && pass->inputNames().empty()) {
            {
                noteProblem(detail::engineProblem(u8"engine.screen.no_input",
                                                  vn::graphics::DiagnosticCategory::ContentSkipped,
                                                  String(u8"pass '") + reportedPassName(pass) +
                                                      String(u8"' is a ScreenPass that declares no input, so it can never draw"
                                                             u8" (give it addInput / addInputName)")));
            }
            continue;
        }

        // 3. No camera: the fullscreen path builds the pass' view from it and pushes the scene's
        // lights through that view space, so without one the pass describes nothing and the frame hands the
        // backend no draw at all. The fix is static (setCamera), and a host would otherwise see a
        // post-process that never appears with no reason for it.
        if (pass->camera() == nullptr) {
            {
                noteProblem(detail::engineProblem(u8"engine.screen.no_camera",
                                                  vn::graphics::DiagnosticCategory::ContentSkipped,
                                                  String(u8"pass '") + reportedPassName(pass) +
                                                      String(u8"' has a fullscreen program but no camera, so it can never draw: the"
                                                             u8" fullscreen path builds its view from the pass camera (and pushes the"
                                                             u8" scene's lights through it) — give the pass a camera (setCamera)")));
            }
        }
    }

    // Phase 4: a declared image nobody bound to a target. An unbound identity has no address, so
    // nothing can be delivered through it (resolveDeclaredImage() returns null and says nothing,
    // because declaring it is what is wrong) and the promise check had nothing to compare with the
    // target the pass renders into. One report per IMAGE: a producer and its consumers share the same
    // unbound object. An image whose double claim the collision check already reported is skipped —
    // that message names both passes and the fix, and one mistake gets one message.
    for (const UnboundDeclaration& unbound : unbound_declared) {
        if (isColliding(OutputIdentity::of(*unbound.image))) {
            continue;
        }
        noteProblem(detail::engineProblem(
            u8"engine.pass.image_unbound",
            vn::graphics::DiagnosticCategory::ContentSkipped,
            unbound.as_output ? String(u8"pass '") + reportedPassName(unbound.pass) + String(u8"' declares the output image '") + unbound.image->label() +
                                    String(u8"' but that image is not bound to a target, so no consumer can resolve"
                                           u8" it and the promise cannot be checked against what the pass renders"
                                           u8" into — bind it (ImageRef::bind(target, attachment))")
                              : String(u8"pass '") + reportedPassName(unbound.pass) + String(u8"' declares the input image '") + unbound.image->label() +
                                    String(u8"' but that image is not bound to a target, so nothing can fill it —"
                                           u8" bind it (ImageRef::bind(target, attachment))")));
    }
}

} // namespace detail

VN_GRAPHICS_NS_END
