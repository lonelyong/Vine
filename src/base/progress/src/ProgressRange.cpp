#include <vine/progress/ProgressRange.hpp>

#include <utility>

#include <vine/progress/ProgressIndicator.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_PROGRESS_NS_BEGIN

ProgressRange::ProgressRange(ProgressRange&& other) noexcept
{
    steal(std::move(other));
}

ProgressRange& ProgressRange::operator=(ProgressRange&& other)
{
    if (this != &other) {
        // Report what this handle still owes before its scope reference is overwritten: dropping it would make
        // the bar lose a step the operation really covered.
        complete();
        steal(std::move(other));
    }
    return *this;
}

ProgressRange::ProgressRange(const ProgressScope& parent, double start, double delta)
  : parent_scope_(&parent)
  , start_(start)
  , delta_(delta)
{}

ProgressRange::~ProgressRange()
{
    complete();
}

bool ProgressRange::isActive() const noexcept
{
    return parent_scope_ != nullptr && parent_scope_->indicator_ != nullptr;
}

bool ProgressRange::isCancelled() const
{
    return parent_scope_ != nullptr && parent_scope_->indicator_ != nullptr && parent_scope_->indicator_->isCancelled();
}

void ProgressRange::complete()
{
    if (!isActive()) {
        return;
    }

    parent_scope_->indicator_->increment(delta_);
    clear();
}

void ProgressRange::steal(ProgressRange&& other) noexcept
{
    parent_scope_ = other.parent_scope_;
    start_        = other.start_;
    delta_        = other.delta_;
    other.clear();
}

void ProgressRange::clear() noexcept
{
    parent_scope_ = nullptr;
    start_        = 0.0;
    delta_        = 0.0;
}

VN_PROGRESS_NS_END
