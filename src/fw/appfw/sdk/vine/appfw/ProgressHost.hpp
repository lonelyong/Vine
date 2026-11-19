#pragma once

#include "appfw_global.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <vine/Signal.hpp>
#include <vine/String.hpp>

#include <vine/progress/ProgressIndicator.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Ambient per-operation progress host (RAII).
 *
 * A host wraps the progress state of one long-running operation: a
 * ProgressIndicator (the progress arithmetic lives in the progress module), an
 * owned cancellation source and an optional stage label. It registers itself in a
 * process-wide registry for the duration of the operation, so code running inside
 * the operation can reach it and the UI can observe all running operations. That
 * registry, the foreground stack and the change signal are application state, which
 * is why the host lives here rather than next to the arithmetic it drives.
 *
 * Multiple hosts may be active concurrently. LongRunning commands form a
 * foreground stack: nested LongRunning commands push their host on top of the
 * parent's, so the innermost running command drives the main progress bar, and
 * when it ends the parent's host is restored automatically. Hosts outside the
 * stack are background hosts that run in parallel and are shown as a compact
 * indicator. New hosts are background by default; the command layer promotes
 * LongRunning commands with setForeground(true).
 *
 * The host performs no presentation; a presenter subscribes to changed() and
 * renders currentSnapshot()/foregroundSnapshots() when it is called, so nothing has to poll.
 * Position updates are thread-safe, so the operation may report progress from a
 * worker thread while the presenter renders on the main thread.
 *
 * Operations report progress by carving scopes out of the root scale. Prefer
 * the scope() helper, which hides the one-shot ProgressRange entirely:
 *
 *     if (auto* host = ProgressHost::current()) {
 *         ProgressScope scope = host->scope(u8"Exporting", count);
 *         for (...) { scope.next(1); ... }
 *     }
 */
class VN_APPFW_API ProgressHost {
  public:
    /**
     * @brief Value snapshot of one host: what a presenter draws, with nothing borrowed.
     *
     * @details
     * - An observer may run on the application thread while the operation - and with it its host - is
     *   destroyed on a worker thread. A pointer into the registry cannot survive that, so a presenter takes
     *   a snapshot under the registry lock and draws from the copy.
     *
     * @note `id` names the host instance without pointing at it: a presenter uses it to tell "a new
     *       operation owns the bar" from "the same one reported again".
     */
    struct Snapshot {
        /// Identity of the host instance, unique in the process and stable for its lifetime.
        std::uint64_t id{ 0 };
        /// Overall progress of that host in [0, 1].
        double position{ 0.0 };
        /// Stage label at the time of the snapshot; empty when unset.
        String label;
        /// Shared cancellation state: it stays valid (and request_stop() stays meaningful) after the host is gone.
        std::stop_source cancelSource;
    };

  public:
    /**
     * @brief Constructs a background host owning its own cancellation source.
     */
    ProgressHost();

    /**
     * @brief Constructs a background host observing an external source.
     *
     * The indicator binds to the source's token, and cancelSource() refers to
     * the same shared stop state, so requesting a stop through either the
     * source or the host cancels the operation. Used by the command layer so
     * the UI cancel button and the command's own stop token stay in sync.
     *
     * @param source Cancellation source shared with the operation.
     */
    explicit ProgressHost(std::stop_source source);

    ~ProgressHost();

    ProgressHost(const ProgressHost&)            = delete;
    ProgressHost& operator=(const ProgressHost&) = delete;

    /**
     * @brief Returns the top of the foreground stack, or nullptr when empty.
     *
     * This is the innermost running LongRunning operation - the one whose progress drives the main bar. Safe to
     * call from any thread.
     *
     * @note The pointer is BORROWED: it is alive only while its operation runs, which is why this is the
     *       operation's own entry point ("my host, while I report"). An observer, which outlives the operation
     *       it draws, takes currentSnapshot() / foregroundSnapshots() instead.
     *
     * @return The current foreground host, or nullptr.
     */
    static ProgressHost* current();

    /**
     * @brief Returns whether any host is currently active.
     *
     * Includes foreground-stack and background hosts.
     *
     * @return true if at least one operation is running.
     */
    static bool isActive();

    /**
     * @brief Returns a value snapshot of the foreground host, or nothing when the stack is empty.
     *
     * This is how an OBSERVER reads the foreground state: the copy is taken under the registry lock, so it is
     * safe while the operation destroys its host on another thread. current() stays the operation's own entry
     * point (its host is alive exactly while it runs) and the identity a test asserts on.
     *
     * @return The foreground snapshot, or std::nullopt when nothing is on the stack.
     */
    static std::optional<Snapshot> currentSnapshot();

    /**
     * @brief Returns a value snapshot of the whole foreground stack, outermost first.
     *
     * The stack is the chain of nested LongRunning commands; a presenter uses the labels to render the chain
     * breadcrumb. Like currentSnapshot(), the copy is what makes the read safe across a concurrent host
     * destruction.
     *
     * @return One snapshot per foreground host, outermost first; empty when nothing is on the stack.
     */
    static std::vector<Snapshot> foregroundSnapshots();

    /**
     * @brief Returns how many hosts are registered, foreground and background together.
     *
     * A presenter derives "operations running beside the foreground chain" from this count and the size of
     * foregroundSnapshots().
     *
     * @return The number of active hosts.
     */
    static std::size_t activeHostCount();

    /**
     * @brief Returns the signal fired whenever the observable state changes.
     *
     * Fired when a host registers or unregisters, when a host is pushed on or
     * removed from the foreground stack, when a stage label is set, and when an
     * indicator's position crosses a percent step (or reaches its end, or is reset
     * by a new run). Position changes are coalesced at the source for exactly that
     * reason - reporting one item at a time must not cost one notification per
     * item.
     *
     * The signal carries no payload on purpose: the unregister notification is
     * fired while that host is still being destroyed, so naming it would hand
     * observers a dying object. A handler re-samples currentSnapshot() /
     * foregroundSnapshots() instead - a value copy taken under the registry lock,
     * which costs no more than a look at the registry it was about to read anyway.
     *
     * Fired on the thread that changed the state, which is not necessarily the
     * application thread, so a presentation handler marshals itself. It is fired
     * with no lock held, and firing it costs nothing while nobody subscribed.
     *
     * @return The change signal; subscribe with the returned handle in a member.
     */
    static Signal<>& changed();

    /**
     * @brief Returns a snapshot of all active hosts.
     *
     * Order is unspecified but stable for the duration of the snapshot; hosts
     * in the foreground stack and background hosts are all included.
     *
     * @note Borrowed like current(): an observer uses activeHostCount() or
     *       foregroundSnapshots() instead of dereferencing these pointers later.
     *
     * @return All active hosts.
     */
    static std::vector<ProgressHost*> activeHosts();

    /**
     * @brief Returns the foreground stack, outermost first.
     *
     * The stack contains the chain of nested LongRunning commands; hosts in it
     * are part of one operation, not parallel background work. The UI uses it
     * to tell in-chain hosts apart from genuine background hosts and to render
     * a chain breadcrumb.
     *
     * @note Borrowed like current(): a presenter renders foregroundSnapshots().
     *
     * @return The foreground stack, outermost first.
     */
    static std::vector<ProgressHost*> foregroundStack();

    /**
     * @brief Pushes this host onto / removes it from the foreground stack.
     *
     * Promoting pushes on top of the current foreground (so a nested child
     * takes over the bar); demoting removes it from the stack. The host
     * destructor also removes it, restoring the previous foreground.
     *
     * @param fg true to push, false to remove.
     */
    void setForeground(bool fg = true);

    /**
     * @brief Returns whether this host is part of the foreground stack.
     *
     * true for in-chain LongRunning hosts; false for background hosts.
     *
     * @return true if this host is in the foreground stack.
     */
    bool isForeground() const;

    /**
     * @brief Returns the progress indicator backing this host.
     *
     * @return The indicator.
     */
    progress::ProgressIndicator& indicator();

    /**
     * @brief Returns the progress indicator backing this host.
     *
     * @return The indicator.
     */
    const progress::ProgressIndicator& indicator() const;

    /**
     * @brief Returns the cancellation source of this operation.
     *
     * The UI cancel button calls request_stop() on it; the operation observes
     * the request through ProgressScope::isCancelled() or indicator().token().
     *
     * @return The cancellation source.
     */
    std::stop_source& cancelSource();

    /**
     * @brief Returns the cancellation source of this operation.
     *
     * @return The cancellation source.
     */
    const std::stop_source& cancelSource() const;

    /**
     * @brief Returns the root range covering the whole [0, 1] scale.
     *
     * Pass it to a ProgressScope to map the operation's stages onto the global
     * progress. The range is a move-only handle: exactly one ProgressScope can take it over (by std::move), and
     * every call resets the indicator. Call it once at the start of the operation
     * and prefer scope() for the common top-level-stage case.
     *
     * @return The root progress range.
     */
    progress::ProgressRange range();

    /**
     * @brief Creates the top-level scope covering the whole [0, 1] scale.
     *
     * Preferred over range(): it hides the one-shot ProgressRange entirely, so
     * the operation cannot trip over its move-only semantics. The returned
     * scope maps its local [0, max] range onto the global scale; nest further
     * scopes inside it for sub-stages.
     *
     * @param name Stage label.
     * @param max Local range length.
     * @return The operation's top-level progress scope.
     */
    progress::ProgressScope scope(const String& name = {}, double max = 1.0);

    /**
     * @brief Sets a human-readable label describing the current stage.
     *
     * Setting the label it already has changes nothing and does not notify, so a
     * stage that re-announces its name is free.
     *
     * @param label Stage label, may be empty.
     */
    void setLabel(const String& label);

    /**
     * @brief Returns the current stage label.
     *
     * Returned by value on purpose: the label is written by the operation, which
     * may be any thread, while observers read it from the thread they run on. The
     * copy is what makes the read safe without making the caller hold a lock.
     *
     * @return The stage label.
     */
    String label() const;

  private:
    /// Builds the value a presenter reads; the caller holds the registry lock.
    ///
    /// Taking the label mutex here is the one nesting this class has (registry -> label), which is safe
    /// because nothing takes the label mutex before the registry one.
    ///
    /// @return This host as a value snapshot.
    Snapshot snapshotLocked() const;

    // stop_source_ is declared first so the indicator can bind to its token.
    std::stop_source            stop_source_;
    progress::ProgressIndicator indicator_;

    /// Identity of this host instance; a snapshot names the host by it without pointing at it.
    const std::uint64_t id_;

    /// Guards label_, which the operation writes and observers read.
    mutable std::mutex label_mutex_;
    String             label_;
};

VN_APPFW_NS_END
