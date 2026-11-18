#include <vine/appfw/ProgressHost.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>

#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// Guards the active-host registry (hosts register/deregister on any thread).
std::mutex s_mutex;
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

/// Hands every host a distinct identity.
///
/// A presenter has to tell "a new operation owns the bar" from "the same one reported again", and it may not
/// do that by pointer: the host can be destroyed on another thread. The number is relayed in Snapshot::id.
std::atomic<std::uint64_t> s_next_host_id{ 1 };

/// Returns the next host identity.
///
/// @return A number no other host of this process has.
std::uint64_t nextHostId() noexcept
{
    return s_next_host_id.fetch_add(1, std::memory_order_relaxed);
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
  : ProgressHost(std::stop_source{})
{}

ProgressHost::ProgressHost(std::stop_source source)
  : stop_source_(std::move(source))
  , indicator_(stop_source_.get_token())
  , id_(nextHostId())
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
        auto            it = std::find(s_active.begin(), s_active.end(), this);
        if (it != s_active.end()) {
            s_active.erase(it);
        }
        // Removing from the stack restores the previous foreground host.
        removeFromForegroundStack(this);
    }

    // Signalled after the host left the registry, so an observer that samples it in its handler sees the
    // restored foreground instead of this host. The signal carries no payload for the same reason.
    changed().trigger();
}

ProgressHost::Snapshot ProgressHost::snapshotLocked() const
{
    Snapshot snapshot;
    snapshot.id           = id_;
    snapshot.position     = indicator_.position();
    snapshot.label        = label();
    snapshot.cancelSource = stop_source_;
    return snapshot;
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

std::optional<ProgressHost::Snapshot> ProgressHost::currentSnapshot()
{
    std::lock_guard lock(s_mutex);
    if (s_foreground_stack.empty()) {
        return std::nullopt;
    }
    return s_foreground_stack.back()->snapshotLocked();
}

std::vector<ProgressHost::Snapshot> ProgressHost::foregroundSnapshots()
{
    std::lock_guard lock(s_mutex);
    std::vector<Snapshot> result;
    result.reserve(s_foreground_stack.size());
    for (const ProgressHost* host : s_foreground_stack) {
        result.push_back(host->snapshotLocked());
    }
    return result;
}

std::size_t ProgressHost::activeHostCount()
{
    std::lock_guard lock(s_mutex);
    return s_active.size();
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

progress::ProgressScope ProgressHost::scope(const String& name, double max)
{
    // The root range from range() is consumed by exactly this scope, so the
    // caller never touches the one-shot ProgressRange directly. The progress
    // module below appfw is Qt-free and spells its labels as UTF-8 std::string.
    return progress::ProgressScope(range(), name.as_std_str(), max);
}

void ProgressHost::setLabel(const String& label)
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

String ProgressHost::label() const
{
    std::lock_guard lock(label_mutex_);
    return label_;
}

VN_APPFW_NS_END
