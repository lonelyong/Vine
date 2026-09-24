#include <vine/progress/ProgressIndicator.hpp>

#include <utility>

#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_PROGRESS_NS_BEGIN

namespace
{

/// Notification granularity: observers hear about the position at most once per
/// percent of the scale. Finer than any presentation, coarser than a work item.
constexpr double kAnnounceStep = 0.01;

} // namespace

void ProgressIndicator::announce()
{
    if (on_position_) {
        on_position_();
    }
}

ProgressIndicator::ProgressIndicator(std::stop_token token)
  : token_(std::move(token))
{
    root_scope_ = new ProgressScope(this);
}

ProgressIndicator::~ProgressIndicator()
{
    // Disarm the root scope so its destructor does not call increment() on a
    // partially destroyed indicator.
    root_scope_->indicator_ = nullptr;
    root_scope_->active_    = false;
    delete root_scope_;
}

ProgressRange ProgressIndicator::start()
{
    position_.store(0.0, std::memory_order_relaxed);

    // The announced position has to travel back with it: a fresh run whose
    // announcer still sat at the old end would look like a backwards step on every
    // single item and announce the whole run item by item.
    const bool restart = announced_.exchange(0.0, std::memory_order_relaxed) > 0.0;

    root_scope_->local_pos_ = 0.0;

    if (restart) {
        // A visible change: observers looking at a finished bar must see it reset.
        announce();
    }

    return root_scope_->next();
}

void ProgressIndicator::setPositionCallback(std::function<void()> callback)
{
    on_position_ = std::move(callback);
}

double ProgressIndicator::position() const
{
    return position_.load(std::memory_order_relaxed);
}

bool ProgressIndicator::isCancelled() const
{
    return token_.stop_requested();
}

std::stop_token ProgressIndicator::token() const
{
    return token_;
}

void ProgressIndicator::increment(double step)
{
    double current = position_.load(std::memory_order_relaxed);
    for (;;) {
        const double next = (current > 1.0 - step) ? 1.0 : current + step;
        if (position_.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
            announceIfSignificant(next);
            return;
        }
    }
}

void ProgressIndicator::announceIfSignificant(double new_position)
{
    const double announced = announced_.load(std::memory_order_relaxed);

    // The end of the scale always counts, however small the step that reached it
    // was; a backwards step means the position was reset behind the announcer's
    // back, and waiting for a whole percent of silence would be worse than telling
    // observers twice.
    const bool at_end   = new_position >= 1.0 && announced < 1.0;
    const bool worth_it = new_position < announced || at_end || (new_position - announced) >= kAnnounceStep;
    if (!worth_it) {
        return;
    }

    // Two threads reporting at once may both announce, or one may overwrite the
    // other's mark and leave an update for the next percent step: the mark is a
    // coalescing hint, not a lock, and the position itself is already published.
    announced_.store(new_position, std::memory_order_relaxed);
    announce();
}

VN_PROGRESS_NS_END
