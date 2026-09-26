#include <vine/appfw/StartupProgress.hpp>

#include <atomic>

#include <vine/appfw/ProgressHost.hpp>
#include <vine/logging/Log.hpp>
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
    // The contract is "at most one per process": a second sink takes over (reporting from the first one still reaches
    // the same presenters, since both report into the progress registry), and the overlap is reported rather than
    // silently resolved - a boot that nests in another boot is a bug in the caller.
    if (StartupProgress* const previous = s_current.exchange(this, std::memory_order_acq_rel); previous != nullptr) {
        VN_LOGW("a second StartupProgress took over while another one was alive: the first one's reports are shadowed");
    }

    // Promoting the host is what makes the startup frame, the status bar and the console show the boot instead of
    // reporting "one more background task is running".
    d->host.setForeground(true);
}

StartupProgress::~StartupProgress()
{
    // Only clear the slot when this sink is the one in it: a sink that was already taken over must not erase its
    // successor (which is exactly what an unconditional store would do).
    StartupProgress* expected = this;
    static_cast<void>(s_current.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel));

    // The host leaves the foreground stack by itself, so a presenter re-sampling the registry sees the operation that
    // was running underneath (normally none) rather than this dying sink.
}

StartupProgress* StartupProgress::current()
{
    return s_current.load(std::memory_order_acquire);
}

std::stop_token StartupProgress::stopToken() const
{
    // The host owns the source; the token follows it, so a boot that is cancelled through the presenter (the status
    // bar's Cancel button calls the same source) is seen here too.
    return d->host.cancelSource().get_token();
}

void StartupProgress::requestCancel()
{
    d->host.cancelSource().request_stop();
}

void StartupProgress::stage(const String& name)
{
    // Ending the counted stage advances the bar to its end, which the presenters then hide behind a busy bar until the
    // next counted stage starts over.
    d->stage_scope.reset();
    d->done = 0.0;
    d->counted.store(false, std::memory_order_release);
    d->host.setLabel(name);
}

void StartupProgress::stage(const String& name, double total)
{
    if (!(total > 0.0)) {
        stage(name);
        return;
    }

    d->stage_scope.reset();
    d->done = 0.0;
    d->counted.store(true, std::memory_order_release);

    // range() resets the scale, so every counted stage starts at zero: the bar reads as this stage's own progress.
    d->stage_scope = std::make_unique<progress::ProgressScope>(d->host.range(), name.as_std_str(), total);
    d->host.setLabel(name);
}

void StartupProgress::setDone(double done)
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

void StartupProgress::setLabel(const String& text)
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

String StartupProgress::label() const
{
    return d->host.label();
}

std::optional<double> StartupProgress::fraction() const
{
    if (!d->counted.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return d->host.indicator().position();
}

VN_APPFW_NS_END
