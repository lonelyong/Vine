#include <vine/appfw/StartupProgress.hpp>

#include <atomic>

#include <vine/appfw/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// The process-wide sink; the boot that reports into it owns it.
std::atomic<StartupProgress*> s_current{ nullptr };

} // namespace

struct StartupProgress::Impl {
    /// Foreground host: a boot is the foreground activity of its process, and this is what the presenters render.
    ProgressHost host;

    /// The current counted stage; empty while the current stage is indeterminate.
    std::unique_ptr<progress::ProgressScope> stage_scope;

    /// Units already reported for the current stage (reporting thread only).
    double done = 0.0;

    /// Whether the current stage is counted; presenters read it from their own thread.
    std::atomic<bool> counted{ false };
};

StartupProgress::StartupProgress()
  : d(new Impl())
{
    s_current.store(this, std::memory_order_release);

    // Promoting the host is what makes the startup frame, the status bar and the console show the boot instead of
    // reporting "one more background task is running".
    d->host.setForeground(true);
}

StartupProgress::~StartupProgress()
{
    // The host leaves the foreground stack by itself, so a presenter re-sampling the registry sees the operation that
    // was running underneath (normally none) rather than this dying sink.
    s_current.store(nullptr, std::memory_order_release);
}

StartupProgress* StartupProgress::current()
{
    return s_current.load(std::memory_order_acquire);
}

void StartupProgress::stage(const std::string& name)
{
    // Ending the counted stage advances the bar to its end, which the presenters then hide behind a busy bar until the
    // next counted stage starts over.
    d->stage_scope.reset();
    d->done = 0.0;
    d->counted.store(false, std::memory_order_release);
    d->host.setLabel(name);
}

void StartupProgress::stage(const std::string& name, double total)
{
    if (!(total > 0.0)) {
        stage(name);
        return;
    }

    d->stage_scope.reset();
    d->done = 0.0;
    d->counted.store(true, std::memory_order_release);

    // range() resets the scale, so every counted stage starts at zero: the bar reads as this stage's own progress.
    d->stage_scope = std::make_unique<progress::ProgressScope>(d->host.range(), name, total);
    d->host.setLabel(name);
}

void StartupProgress::advance(double done)
{
    if (d->stage_scope == nullptr) {
        return;
    }

    const double step = done - d->done;
    if (step <= 0.0) {
        return;
    }
    d->done = done;

    // A ProgressRange advances the indicator when it is completed, so taking one step and completing it is the whole
    // update: the scope keeps the stage's local position, the range carries the portion to the indicator.
    progress::ProgressRange step_range = d->stage_scope->next(step);
    step_range.complete();
}

void StartupProgress::setLabel(const std::string& text)
{
    d->host.setLabel(text);
}

void StartupProgress::complete()
{
    if (d->stage_scope != nullptr) {
        // Completing the scope advances the indicator to the end of the stage and notifies.
        d->stage_scope.reset();
    }
    else {
        // No counted stage to complete: the root range is what fills the bar, and completing it is what tells the
        // presenters the boot is over (a stage that reported no total would otherwise leave the bar where it was).
        progress::ProgressRange root = d->host.range();
        root.complete();
    }

    d->done = 0.0;
    d->counted.store(false, std::memory_order_release);
}

std::string StartupProgress::label() const
{
    return d->host.label();
}

bool StartupProgress::isCounted() const
{
    return d->counted.load(std::memory_order_acquire);
}

double StartupProgress::fraction() const
{
    return d->host.indicator().position();
}

VN_APPFW_NS_END
