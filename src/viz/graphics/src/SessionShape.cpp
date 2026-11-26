#include "SessionShape.hpp"

#include <algorithm>
#include <span>
#include <typeinfo>
#include <utility>
#include <vector>

#include <vine/graphics/PassStage.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/FullscreenPass.hpp>

#include "EngineDiagnostics.hpp"
#include "FrameResources.hpp"
#include "FrameScheduler.hpp"
#include "PublicationStore.hpp"

VN_GRAPHICS_NS_BEGIN

namespace detail
{

SessionShape::SessionShape(Diagnostics& diagnostics, PublicationStore& publications, FrameResources& resources) noexcept
  : diagnostics_(diagnostics)
  , publications_(publications)
  , resources_(resources)
  , scheduler_(std::make_unique<FrameScheduler>())
{}

SessionShape::~SessionShape() = default;

std::span<const SessionShape::Slot> SessionShape::slots() const noexcept
{
    return std::span<const Slot>(slots_.data(), slots_.size());
}

std::size_t SessionShape::passCount() const noexcept
{
    return slots_.size();
}

raw_ptr<RenderPass> SessionShape::pass(const String& name) const
{
    for (const Slot& slot : slots_) {
        if (slot.pass != nullptr && slot.pass->name() == name) {
            return slot.pass.get();
        }
    }
    return nullptr;
}

raw_ptr<RenderPass> SessionShape::passFor(const String& name, const void* owner) const
{
    if (owner == nullptr) {
        return pass(name);
    }
    for (const Slot& slot : slots_) {
        if (slot.pass != nullptr && slot.owner == owner && slot.pass->name() == name) {
            return slot.pass.get();
        }
    }
    return nullptr;
}

std::optional<int> SessionShape::passOrder(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass == pass) {
            return slot.order;
        }
    }
    return std::nullopt;
}

Plan SessionShape::planFor(const void* owner) const
{
    if (owner == nullptr) {
        return plan();
    }
    Plan shape;
    for (const Slot& slot : slots_) {
        if (slot.pass == nullptr || slot.owner != owner) {
            continue;
        }
        PassDesc desc = slot.pass->declaration();
        desc.order    = slot.order;
        shape.passes.push_back(std::move(desc));
    }
    return shape;
}

Plan SessionShape::plan() const
{
    Plan shape;
    shape.passes.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        if (slot.pass == nullptr) {
            continue;
        }
        PassDesc desc = slot.pass->declaration();
        // The EFFECTIVE order, stated: the shape read back is the shape that reproduces it, even when the
        // declaration only named a stage.
        desc.order    = slot.order;
        shape.passes.push_back(std::move(desc));
    }
    return shape;
}

FrameTable SessionShape::frameTable(std::span<const PassCost> answered) const
{
    FrameTable table;
    table.passes.reserve(slots_.size());
    for (const Slot& slot : slots_) {
        const RenderPass* const pass = slot.pass.get();
        if (pass == nullptr) {
            continue;
        }
        PassRow row;
        row.pass          = pass;
        row.order         = slot.order;
        row.name          = pass->name();
        row.content       = pass->content();
        row.camera        = pass->camera();
        row.layer_mask    = pass->layerMask();
        row.target        = pass->renderTarget();
        row.enabled       = pass->isEnabled();
        row.clears_color  = pass->isClearEnabled();
        row.clear_color   = pass->clearColor();
        row.clears_depth  = pass->shouldClearDepth();
        // The WHOLE declaration list, spelled for a reader: an input stated by object (a shadow map, an
        // MRT g-buffer) is as much a fact of the frame as one stated by name (see ImageInput::description).
        row.inputs.reserve(pass->inputs().size());
        for (const ImageInput& input : pass->inputs()) {
            row.inputs.push_back(input.description());
        }
        row.output = pass->outputName();
        // THE COST IS THE ANSWERED FRAME'S, JOINED BY PASS IDENTITY: a row only takes a number that names THIS pass, so
        // a pass registered or re-ordered after that frame answers 0 rather than inheriting a neighbour's cost (see
        // detail::PassCost). One linear scan over the answered frame's entries - a read path, not a per-frame one.
        for (const PassCost& cost : answered) {
            if (cost.pass == pass) {
                row.record_micros = cost.micros;
                break;
            }
        }
        if (pass->hasViewport()) {
            row.viewport = pass->viewport();
        }
        row.depth_range = pass->depthRange();
        table.passes.push_back(std::move(row));
    }
    return table;
}

PlanResult SessionShape::applyPlan(const Plan& plan, const void* owner, RenderBackend* device)
{
    // The host's own bindings serve names too (see RenderEngine::publish), so validation must know them or it would
    // refuse a plan that reads a picture the host hands in. A SCOPED plan also reads names its siblings publish: those
    // rows are not in this plan, so they are external inputs exactly like a host binding.
    std::vector<String> external = hostOutputNames();
    if (owner != nullptr) {
        for (const Slot& slot : slots_) {
            if (slot.pass == nullptr || slot.owner == owner) {
                continue;
            }
            // A sibling's PASS name is a valid input name, and so is the OUTPUT name it publishes: a scoped plan may
            // read either without owning the row.
            external.push_back(slot.pass->name());
            if (!slot.pass->outputName().empty()) {
                external.push_back(slot.pass->outputName());
            }
        }
    }

    PlanResult result;
    result.problems = validatePlan(plan, std::span<const String>(external.data(), external.size()));
    if (!result.problems.empty()) {
        // ALL OR NOTHING: a half-applied shape would be a shape nobody declared. The answer is the return value; the
        // diagnostic is for the log, one per problem per CALL (a host call, not a frame, so the call is the episode).
        for (const PlanProblem& problem : result.problems) {
            diagnostics_.report(detail::engineProblem(u8"engine.compile.plan_refused",
                                                      vn::graphics::DiagnosticCategory::PassProtocolViolation,
                                                      String(u8"plan refused [") + planCodeText(problem.code) + String(u8"]: ") + problem.detail));
        }
        result.outcome = PlanResult::Outcome::Invalid;
        return result;
    }

    applyPlanRows(plan, owner, device);
    return result;
}

void SessionShape::applyPlanRows(const Plan& plan, const void* owner, RenderBackend* device)
{
    // PHASE 1 - REMOVE what the plan no longer names. Collected first: removePass() mutates the table and releases
    // backend state, so it must run once per pass and never while the table is being walked.
    //
    // ONLY THE WHOLE-SESSION FORM REMOVES: a scoped plan states one contributor's rows and never speaks for the
    // session, so "not in my plan" there means "not mine", not "gone" (see RenderEngine::applyPlan's note).
    std::vector<raw_ptr<RenderPass>> doomed;
    for (const Slot& slot : slots_) {
        if (slot.pass == nullptr || owner != nullptr) {
            continue; // a scoped plan states rows; it never speaks for the session
        }
        const String name  = slot.pass->name();
        const bool   named = std::any_of(plan.passes.begin(), plan.passes.end(), [&name](const PassDesc& desc) { return desc.name == name; });
        if (!named) {
            doomed.push_back(slot.pass.get());
        }
    }
    for (raw_ptr<RenderPass> pass : doomed) {
        removePass(pass, device);
    }

    // PHASE 2 - UPDATE the names both shapes state, ADD the ones only the plan states.
    for (const PassDesc& desc : plan.passes) {
        const int order = desc.order.value_or(passStageOrder(desc.stage));

        // A scoped plan speaks only for its own rows: its identity is (owner, name), so two contributors may each
        // state a pass of the same name (two views of one engine, each with its own "main").
        Slot* existing = nullptr;
        for (Slot& slot : slots_) {
            if (slot.pass == nullptr || (owner != nullptr && slot.owner != owner)) {
                continue;
            }
            if (slot.pass->name() == desc.name) {
                existing = &slot;
                break;
            }
        }

        if (existing != nullptr && existing->pass->declaration().kind == desc.kind) {
            // IN PLACE: the node survives, so everything the backend keyed by it survives too - and a declaration
            // that only changed its target is an update, not a remove plus an add (see P7).
            detail::PassNode::adopt(*existing->pass, desc);
            existing->order = order;
            continue;
        }

        // A NAME THAT CHANGED KIND cannot become the other object: replace it, keeping who owned it.
        const void* existing_owner = nullptr;
        if (existing != nullptr) {
            existing_owner = existing->owner;
            removePass(existing->pass.get(), device);
        }

        Slot slot;
        slot.pass  = makePassNode(desc);
        slot.order = order;
        slot.owner = owner != nullptr ? owner : existing_owner;
        insertSlot(std::move(slot));
    }

    // PHASE 3 - the table is ordered by the orders the plan just stated (an update can move a pass in the frame).
    std::stable_sort(slots_.begin(), slots_.end(), [](const Slot& left, const Slot& right) { return left.order < right.order; });
}

bool SessionShape::bindPassContent(raw_ptr<RenderPass> pass, intrusive_ptr<Scene> content)
{
    for (auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            // The declaration owns it (PassDesc::content), so there is one storage and the compile reads the same
            // scene this call bound. A CONTENT BINDING IS ONE DECLARATION DIFF, not a setter: the node adopts the
            // declaration it already stated with the content field replaced.
            PassDesc desc  = slot.pass->declaration();
            desc.content   = std::move(content);
            detail::PassNode::adopt(*slot.pass, std::move(desc));
            return true;
        }
    }
    // An unregistered pass (or null) is not a binding: say so instead of reporting success for a change that did
    // not happen. The caller can still register it and bind again.
    return false;
}

raw_ptr<Scene> SessionShape::contentOf(raw_ptr<RenderPass> pass) const
{
    for (const auto& slot : slots_) {
        if (slot.pass.get() == pass) {
            return slot.pass->content();
        }
    }
    return nullptr;
}

void SessionShape::removePassesOwnedBy(const void* owner, RenderBackend* device) noexcept
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
        removePass(pass, device);
    }
}

raw_ptr<const RenderPass> SessionShape::firstEnabledPassWithATarget() const noexcept
{
    for (const Slot& slot : slots_) {
        if (slot.pass != nullptr && slot.pass->isEnabled() && slot.pass->renderTarget() != nullptr) {
            return slot.pass.get();
        }
    }
    return nullptr;
}

intrusive_ptr<RenderPass> SessionShape::insertSlot(Slot slot)
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
    // by the order a plan states.
    const int  order      = slot.order;
    const auto it         = std::find_if(slots_.begin(), slots_.end(), [order](const Slot& existing) { return existing.order > order; });
    const auto registered = slots_.insert(it, std::move(slot));
    return registered->pass;
}

intrusive_ptr<RenderPass> SessionShape::makePassNode(const PassDesc& desc)
{
    // The declaration is turned into the execution node of its kind ONCE, and the node then IS the declaration
    // (RenderPass::setDesc): a descriptor-declared pass and a hand-built one cannot drift apart, because by the time
    // the engine holds it, it is a hand-built pass (which is also what keeps the escape hatch the same thing).
    intrusive_ptr<RenderPass> node;
    if (desc.kind == DrawKind::Fullscreen) {
        node = make_intrusive<FullscreenPass>();
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
                diagnostics_.report(detail::engineProblem(u8"engine.compile.layered_depth_refused",
                                                          vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                                          u8"a pass names a layered depth view; layered targets are not built yet, so "
                                                          u8"the declaration is refused"));
            }
            adopted.depth_view.reset();
        }
        else if (adopted.target.target != nullptr && declared.isValid()) {
            const DepthView current = adopted.target.target->sharedDepth();
            if (current.isValid() && current.target() != declared.target()) {
                if (depth_view_conflicts_.insert(std::make_pair(adopted.target.target.get(), declared.target())).second) {
                    diagnostics_.report(detail::engineProblem(u8"engine.compile.depth_view_conflict",
                                                              vn::graphics::DiagnosticCategory::UnsupportedRequest,
                                                              u8"two passes name different depth views for one target; the first "
                                                              u8"declaration stands and this one is refused"));
                }
                adopted.depth_view.reset();
            }
        }
    }
    detail::PassNode::adopt(*node, std::move(adopted));
    return node;
}

void SessionShape::removePass(raw_ptr<RenderPass> pass, RenderBackend* device)
{
    // THE INVOCATION MAY BE THE PASS' LAST REFERENCE (its slot is one, and a host that just added a pass and forgot
    // its handle has nothing else), so hold it for the length of this call: the erase below can drop the last count,
    // and everything after it reads the pass - the same rule clearPasses states for the same reason (measured: a
    // heap-use-after-free of the pass' render target, found by the thread sanitizer because it checks liveness too).
    const intrusive_ptr<RenderPass> keep_alive{ pass };
    const auto                      old_size = slots_.size();
    // The order the pass ran at is not kept anywhere: the slot list IS the answer (see passOrder), and a pass
    // that is gone has none.
    raw_ptr<RenderTarget> transient_owned = nullptr;
    for (const Slot& slot : slots_) {
        if (slot.pass.get() == pass) {
            transient_owned = slot.transient.get();
            break;
        }
    }
    slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [pass](const Slot& slot) { return slot.pass.get() == pass; }), slots_.end());

    // A pass is registered at most once, so any removal drops its only user:
    // release the backend state it retained — keyed by the pass itself (see
    // RenderDevice::releasePass) — plus any off-screen target the pass owns. A
    // TRANSIENT it declared is not its own (this engine owns it, and the next compile lets it go when nothing
    // declares it any more), so it is not released here: that would hand the backend a target the engine still
    // holds for another pass declaring the same one.
    if (device != nullptr && pass != nullptr && slots_.size() != old_size) {
        device->releasePass(pass);
        if (raw_ptr<RenderTarget> target = pass->renderTarget(); target != nullptr && target != transient_owned) {
            device->releaseRenderTarget(target);
        }
    }
}

std::vector<String> SessionShape::hostOutputNames() const
{
    std::vector<String> names;
    names.reserve(resources_.hostBindings().size());
    for (const auto& binding : resources_.hostBindings()) {
        names.push_back(binding.first);
    }
    return names;
}

} // namespace detail

VN_GRAPHICS_NS_END
