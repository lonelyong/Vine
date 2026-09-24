#include <vine/vsg/core/Protocol.hpp>

VN_VSG_NS_BEGIN

namespace core
{

Decision Protocol::onCall(CallKind kind) noexcept
{
    switch (kind)
    {
    case CallKind::BeginFrame:
        // A frame may not open inside another one: the state a frame owns (its announced passes, the
        // swapchain image it acquired) would have no single owner.
        if (state_ != State::Idle)
        {
            return refuse(nesting_reported_);
        }
        state_         = State::InFrame;
        no_scope_reported_ = false;
        nesting_reported_  = false;
        rearmScopeEpisodes();
        return {};

    case CallKind::BeginPass:
        // A pass announces state and draws; with no frame open there is nothing for either to belong
        // to, so this is judged like a drawing call with no scope.
        if (state_ == State::Idle)
        {
            return refuse(no_scope_reported_);
        }
        if (state_ == State::InPass)
        {
            // Nested scope: the inner pass does NOT open (the outer scope keeps its state, and the
            // matching endPass will close the outer one). Reported, because the caller's bookkeeping
            // is off by one scope from here on.
            return refuse(nesting_reported_);
        }
        state_ = State::InPass;
        rearmScopeEpisodes();
        return {};

    case CallKind::EndPass:
        if (state_ != State::InPass)
        {
            return refuse(nesting_reported_);
        }
        state_ = State::InFrame;
        // The scope's announcement goes with the scope: unconsumed state is discarded here.
        announced_target_          = nullptr;
        announced_target_released_ = false;
        rearmScopeEpisodes();
        return {};

    case CallKind::SetScopeAttribute:
        // Setters outside a scope are deliberately inert, NOT violations: the host may announce state
        // before it opens a scope, and the next beginPass() starts from an empty request.
        if (state_ != State::InPass)
        {
            return drop(false);
        }
        if (announced_target_released_)
        {
            // The attribute is for a target that no longer exists; nothing it configures can reach a
            // draw this scope, so it is dropped and the scope reports the condition once.
            const bool first_of_scope = !dead_scope_reported_;
            dead_scope_reported_      = true;
            return drop(first_of_scope);
        }
        return {};

    case CallKind::Draw:
        if (state_ != State::InPass)
        {
            // Refused rather than drawn with leftover state: reported once per frame, because a host
            // that lost track of its scopes hits this on every pass.
            return refuse(no_scope_reported_);
        }
        if (announced_target_released_)
        {
            // Skipped, NOT redirected to the default framebuffer: drawing the content where the caller
            // never asked for it is a wrong picture instead of a missing one. Once per scope.
            return refuse(dead_scope_reported_);
        }
        return {};

    case CallKind::EndFrame:
        if (state_ != State::InFrame)
        {
            return refuse(nesting_reported_);
        }
        return {};

    case CallKind::SwapBuffers:
        // The frame's last call, and the only one that presents: it may not land while a scope is
        // still open, and it may not land with no frame open (the deferral rings advance on committed
        // frames, so a swap with no frame would advance clocks nothing accounted for).
        if (state_ == State::InPass)
        {
            return refuse(nesting_reported_);
        }
        if (state_ != State::InFrame)
        {
            return refuse(no_scope_reported_);
        }
        state_ = State::Idle;
        no_scope_reported_ = false;
        nesting_reported_  = false;
        rearmScopeEpisodes();
        return {};

    case CallKind::ReleaseRenderTarget:
        // Always legal, whenever it arrives: the caller is telling us a target is going away, and the
        // announcement it may still be holding is dropped by noteTargetReleased().
        return {};
    }
    return {};
}

void Protocol::noteAnnouncedTarget(const void* target) noexcept
{
    announced_target_          = target;
    announced_target_released_ = false;
    // A new announcement is a new episode: the same condition may report again for the new target.
    dead_scope_reported_ = false;
}

void Protocol::noteTargetReleased(const void* target) noexcept
{
    if (announced_target_ != target)
    {
        return;
    }
    // Drop the announcement (the contract forbids keeping the pointer) and remember WHY, so the rest
    // of the scope is refused rather than drawn into the wrong place.
    announced_target_          = nullptr;
    announced_target_released_ = true;
}

bool Protocol::scopeOpen() const noexcept
{
    return state_ == State::InPass;
}

bool Protocol::frameOpen() const noexcept
{
    return state_ != State::Idle;
}

bool Protocol::announcedTargetReleased() const noexcept
{
    return announced_target_released_;
}

const void* Protocol::announcedTarget() const noexcept
{
    return announced_target_;
}

std::uint64_t Protocol::refusalCount() const noexcept
{
    return refusals_;
}

std::uint64_t Protocol::droppedCount() const noexcept
{
    return drops_;
}

Decision Protocol::drop(bool report) noexcept
{
    ++drops_;
    return {Verdict::Drop, report};
}

Decision Protocol::refuse(bool& episode) noexcept
{
    ++refusals_;
    const bool first_of_episode = !episode;
    episode                     = true;
    return {Verdict::Refuse, first_of_episode};
}

void Protocol::rearmScopeEpisodes() noexcept
{
    dead_scope_reported_ = false;
}

}  // namespace core

VN_VSG_NS_END
