#include <vine/appfw/ProgressHost.hpp>

#include <algorithm>

#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

V_APPFW_NS_BEGIN

namespace
{

/// Guards the active-host registry (hosts register/deregister on any thread).
std::mutex                 s_mutex;
/// All active hosts (foreground stack + background), registration order.
std::vector<ProgressHost*> s_active;
/// The foreground stack: nested LongRunning hosts, outermost first.
std::vector<ProgressHost*> s_foreground_stack;

/// Removes this host from the foreground stack if present.
///
/// @param host Host to remove.
/// @return true if the host was in the stack.
bool removeFromForegroundStack(ProgressHost* host)
{
    auto it = std::find(s_foreground_stack.begin(), s_foreground_stack.end(), host);
    if (it == s_foreground_stack.end()) {
        return false;
    }
    s_foreground_stack.erase(it);
    return true;
}

} // namespace

Signal<>& ProgressHost::changed()
{
    // Function-local static: the signal is built on first use, so a host
    // constructed from a worker thread - the first thing an operation typically
    // does - cannot race a static initialization at program start.
    static Signal<> signal;
    return signal;
}

ProgressHost::ProgressHost()
  : indicator_(stop_source_.get_token())
{
    indicator_.setPositionCallback([this] { changed().trigger(); });

    {
        std::lock_guard lock(s_mutex);
        s_active.push_back(this);
    }

    changed().trigger();
}

ProgressHost::ProgressHost(std::stop_source source)
  : stop_source_(std::move(source))
  , indicator_(stop_source_.get_token())
{
    indicator_.setPositionCallback([this] { changed().trigger(); });

    {
        std::lock_guard lock(s_mutex);
        s_active.push_back(this);
    }

    changed().trigger();
}

ProgressHost::~ProgressHost()
{
    {
        std::lock_guard lock(s_mutex);
        auto it = std::find(s_active.begin(), s_active.end(), this);
        if (it != s_active.end()) {
            s_active.erase(it);
        }
        // Removing from the stack restores the previous foreground host.
        removeFromForegroundStack(this);
    }

    // Signalled while this host's members are still alive, so an observer that
    // re-samples the registry sees the restored foreground rather than this host,
    // and one still holding this host from an older sample of the label reads live
    // state instead of freed state.
    changed().trigger();
}

ProgressHost* ProgressHost::current()
{
    std::lock_guard lock(s_mutex);
    return s_foreground_stack.empty() ? nullptr : s_foreground_stack.back();
}

bool ProgressHost::isActive()
{
    std::lock_guard lock(s_mutex);
    return !s_active.empty();
}

std::vector<ProgressHost*> ProgressHost::activeHosts()
{
    std::lock_guard lock(s_mutex);
    return s_active;
}

std::vector<ProgressHost*> ProgressHost::foregroundStack()
{
    std::lock_guard lock(s_mutex);
    return s_foreground_stack;
}

void ProgressHost::setForeground(bool fg)
{
    bool stack_changed = false;
    {
        std::lock_guard lock(s_mutex);
        if (fg) {
            auto it = std::find(s_foreground_stack.begin(), s_foreground_stack.end(), this);
            if (it == s_foreground_stack.end()) {
                s_foreground_stack.push_back(this);
                stack_changed = true;
            }
        }
        else {
            stack_changed = removeFromForegroundStack(this);
        }
    }

    // Promoting an already-foreground host, or demoting a background one, leaves
    // the observable state alone and stays silent.
    if (stack_changed) {
        changed().trigger();
    }
}

bool ProgressHost::isForeground() const
{
    std::lock_guard lock(s_mutex);
    return std::find(s_foreground_stack.begin(), s_foreground_stack.end(), this) != s_foreground_stack.end();
}

progress::ProgressIndicator& ProgressHost::indicator()
{
    return indicator_;
}

const progress::ProgressIndicator& ProgressHost::indicator() const
{
    return indicator_;
}

std::stop_source& ProgressHost::cancelSource()
{
    return stop_source_;
}

const std::stop_source& ProgressHost::cancelSource() const
{
    return stop_source_;
}

progress::ProgressRange ProgressHost::range()
{
    return indicator_.start();
}

progress::ProgressScope ProgressHost::scope(const std::string& name, double max)
{
    // The root range from range() is consumed by exactly this scope, so the
    // caller never touches the one-shot ProgressRange directly.
    return progress::ProgressScope(range(), name, max);
}

void ProgressHost::setLabel(const std::string& label)
{
    {
        std::lock_guard lock(label_mutex_);
        if (label_ == label) {
            return;
        }
        label_ = label;
    }

    changed().trigger();
}

std::string ProgressHost::label() const
{
    std::lock_guard lock(label_mutex_);
    return label_;
}

V_APPFW_NS_END
