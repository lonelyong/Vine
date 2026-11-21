#include <vine/backend_vsg/internal/FrameHead.hpp>

#include <vsg/nodes/Group.h>

VN_BACKEND_VSG_NS_BEGIN

void FrameHead::beginFrame(std::size_t reserve_passes)
{
    if (passes_.capacity() < reserve_passes) {
        passes_.reserve(reserve_passes);
    }
    // THE SLOTS ARE KEPT, THEIR CONTENTS ARE NOT: a plan outlives no frame, and what a slot held belongs to the
    // frame that resolved it (see the header's ownership note). The leases go with them: a producer is pinned
    // only while a frame's answers name it.
    for (Pass& slot : passes_) {
        slot.clear();
    }
    used_ = 0;
    halves_.reset();
}

FrameHead::Pass& FrameHead::beginPass(core::PassId pass, core::VariantPool& pool)
{
    if (used_ == passes_.size()) {
        passes_.emplace_back();
    }
    Pass& slot = passes_[used_];
    slot.clear();
    if (used_ >= registries_.size()) {
        registries_.push_back(std::make_unique<core::StateRegistry>(pool));
    }
    core::StateRegistry* const registry = registries_[used_].get();
    // A REGISTRY IS PER PASS *AND PER FRAME*: the state a recording issues exists in one frame's command buffer, so
    // what the previous frame left "bound" must not make this frame's first draw skip its own bind.
    registry->reset();
    slot.pass     = pass;
    slot.registry = registry;
    ++used_;
    return slot;
}

std::span<const FrameHead::Pass> FrameHead::passes() const noexcept
{
    return std::span<const Pass>(passes_.data(), used_);
}

void FrameHead::pin(Pin producer) noexcept
{
    if (producer != nullptr) {
        halves_ = std::move(producer);
    }
}

void FrameHead::clear() noexcept
{
    for (Pass& slot : passes_) {
        slot.clear();
    }
    used_ = 0;
    halves_.reset();
}

bool FrameHead::record(const Pass& pass, ::vsg::ref_ptr<::vsg::Node>& out)
{
    if (pass.registry == nullptr) {
        // A plan whose slot was never resolved here is a caller bug, not a content condition: the answer is an
        // empty node and false, with nothing reported (nothing in this type reports - see the header).
        out = ::vsg::Group::create();
        return false;
    }

    auto group    = ::vsg::Group::create();
    bool complete = true;
    for (const DrawPlan& call : pass.calls) {
        // THE TWO DRAW PACKETS, EACH THROUGH THE HALF THAT RESOLVED IT: the recorder is the half's own, and the
        // value is the plan's. Neither call touches a table - that is the property this type exists to give.
        ::vsg::ref_ptr<::vsg::StateGroup> node;
        if (call.fullscreen) {
            node = call.draws != nullptr ? call.draws->recordFullscreen(*pass.registry, call.fullscreen_draw) : nullptr;
        }
        else {
            node = call.draws != nullptr ? call.draws->record(*pass.registry, call.value()) : nullptr;
        }
        if (node == nullptr) {
            // The resolution ensured the identity, so this cannot happen for a call that resolved - and if it does
            // (a hand-built plan), the call is skipped rather than drawn with whatever was bound last.
            complete = false;
            continue;
        }
        group->addChild(node);
    }
    out = group;
    return complete;
}

VN_BACKEND_VSG_NS_END
