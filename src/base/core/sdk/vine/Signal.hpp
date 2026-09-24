#pragma once
#include "core_global.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

VN_CORE_NS_BEGIN

template <typename... TArgs>
class Signal;

/**
 * @brief RAII connection handle: cancels the subscription when it is destroyed.
 *
 * The handle connect() hands back is the only owner of the subscription's lifetime:
 * destroying it, move-assigning over it or calling disconnect() cancels the subscription,
 * while detach() gives it up without cancelling it. Move-only, so exactly one owner
 * cancels the subscription.
 * It refers to the subscription's state weakly, so a handle that outlives its Signal is
 * inert instead of dangling, and isActive() reports false once the Signal is gone.
 * Handing a member to one of these is what removes the manual disconnect from the owner's
 * teardown path.
 *
 * The shape - a move-only handle carrying isActive()/disconnect() - is Qt's
 * QMetaObject::Connection, the same analogy the Signal below is built on.
 */
class Connection {
  private:
    template <typename...>
    friend class Signal;

    /**
     * @brief Shared state of one subscription: whether it is still wanted.
     *
     * Signal's slot derives from this, which is what lets the handle live outside the Signal:
     * the liveness flag sits in a type the handle can name, so the handle needs neither the
     * Signal's private slot type nor a complete Signal to be instantiated - and, being a plain
     * class, one Connection type serves every Signal.
     * The flag is atomic because it is read by trigger(), which holds no lock, and written by
     * the mutators, which only hold a lock the reader does not take. A plain bool would need a
     * lock around every check in the firing loop, and the release/acquire pair makes the
     * marking visible promptly instead of letting it be cached.
     */
    struct State {
        std::atomic<bool> alive{ true };
    };

  public:
    /// @brief Creates a handle that owns nothing.
    Connection() noexcept = default;

    /**
     * @brief Takes the subscription over from another handle.
     *
     * @param other Handle to move from; it stops owning the subscription.
     */
    Connection(Connection&& other) noexcept : state_(std::move(other.state_)) { }

    /// @brief Cancels what this handle owns. Destroying an empty handle does nothing.
    ~Connection() { disconnect(); }

    Connection(const Connection&)            = delete;
    Connection& operator=(const Connection&) = delete;

    /**
     * @brief Cancels the current subscription, then takes over another one.
     *
     * Assigning is how a member subscription is replaced without a manual disconnect first:
     * the previous subscription is cancelled here.
     *
     * @param other Handle to move from; it stops owning its subscription.
     * @return Reference to this handle.
     */
    Connection& operator=(Connection&& other) noexcept
    {
        if (this != &other) {
            disconnect();
            state_ = std::move(other.state_);
        }
        return *this;
    }

  public:
    /**
     * @brief Cancels the subscription.
     *
     * Idempotent, and safe to call from inside a handler: after it returns no further
     * invocation of this handler starts, while one already running completes.
     */
    void disconnect() noexcept
    {
        if (const auto state = state_.lock()) {
            state->alive.store(false, std::memory_order_release);
        }
        state_.reset();
    }

    /**
     * @brief Stops managing the subscription without cancelling it.
     *
     * For a handler that should stay subscribed for as long as the Signal lives, such as a
     * widget wiring an internal behaviour onto its own signal.
     */
    void detach() noexcept { state_.reset(); }

    /**
     * @brief Reports whether the handler can still be called.
     *
     * @return true while the slot exists and has not been cancelled.
     */
    [[nodiscard]] bool isActive() const noexcept
    {
        const auto state = state_.lock();
        return state != nullptr && state->alive.load(std::memory_order_acquire);
    }

    /**
     * @brief Reports whether the handler can still be called.
     *
     * @return true while the slot exists and has not been cancelled.
     */
    explicit operator bool() const noexcept { return isActive(); }

  private:
    /**
     * @brief Wraps a freshly published slot; only the Signal creates these.
     *
     * @param state Slot state to own.
     */
    explicit Connection(std::shared_ptr<State> state) noexcept : state_(std::move(state)) { }

    /// The subscription this handle owns, weakly: a handle that outlives its Signal is inert.
    std::weak_ptr<State> state_;
};

/**
 * @brief Thread-safe signal: any thread may connect, disconnect and fire.
 *
 * The subscription table is an immutable snapshot published atomically, so firing
 * takes no lock at all: it loads the published table, keeps it alive by holding a
 * reference while it walks, and skips every subscription that was cancelled in the
 * meantime.
 * That is the scheme Qt uses for its connection lists (an atomic pointer to
 * refcounted connection data plus a per-connection dead flag), and the read-mostly
 * container pattern (mutate by copy, publish, read the snapshot) that
 * folly/stdexec-style libraries use. Subscribing and unsubscribing replace the table
 * instead of editing it, because a firing thread may be walking the published one;
 * that copy is why subscriptions belong at setup rather than in a hot loop.
 *
 * Semantics, preserved from the single-threaded implementation and pinned by
 * tests:
 * - handlers run in subscription order;
 * - a handler subscribed while the signal is firing does not run in that firing
 *   (the snapshot was already taken), but does run in the next one;
 * - a handler unsubscribed while the signal is firing is not called any more,
 *   even when it had not been reached yet, while the one already running runs to
 *   its end;
 * - a handler may subscribe, unsubscribe, block or re-fire the signal.
 *
 * connect() hands back an owning Connection that cancels the subscription in its destructor,
 * so a subscription kept in a member needs no manual teardown, and one that outlives the
 * Signal is inert instead of dangling. The handle keeps the shape appfw's EventBus handle
 * has - move-only, cancelling on destruction - while the entry point is named after Qt's
 * QObject::connect(), which is also where the Connection handle comes from.
 *
 * Handlers are called with no lock held, so re-entering any of these methods
 * from a handler cannot deadlock.
 *
 * @tparam TArgs Handler argument types.
 */
template <typename... TArgs>
class Signal {
  public:
    using Handler = std::function<void(TArgs...)>;

  private:
    /**
     * @brief One slot in the table: the callback plus whether it is still wanted.
     *
     * Held by shared_ptr, so a firing signal keeps using the slot it took in its snapshot -
     * and keeps it alive - while disconnect() flips the flag that stops it from being called.
     * The flag lives in the inherited Connection::State, which is what the handle reaches;
     * the callback lives here, because only the Signal calls it.
     */
    struct Slot : Connection::State {
        Handler           callback;
    };

    /// The slot table: immutable once published, replaced as a whole by every mutation, and
    /// in subscription order, so it is only ever appended to and walked.
    ///
    /// A vector of pointers, not a map: the copy a mutator pays is then a contiguous
    /// pointer copy instead of a red-black tree rebuild, and the firing loop walks it
    /// sequentially instead of chasing tree nodes. Measured at 20 handlers - firing
    /// (std::map) 62.6 -> 24.8 ns, connect plus disconnect 538 -> 147 ns per pair.
    using Table = std::vector<std::shared_ptr<Slot>>;

  public:
    Signal()              = default;
    Signal(const Signal&) = delete;
    Signal(Signal&&)      = delete;

    Signal& operator=(const Signal&) = delete;
    Signal& operator=(Signal&&)      = delete;

  public:
    /**
     * @brief Connects a handler and returns the handle that owns the subscription.
     *
     * Safe from any thread, including from inside a handler and while another thread is
     * firing. Connecting replaces the slot table rather than editing it, because a firing
     * thread may be walking the published one; the copy is contiguous and cheap, but it is
     * still the reason connections belong at setup. The handler keeps running until the
     * returned handle is destroyed, disconnect() is called on it, or the Signal goes away.
     *
     * @param handler Callback to append; called with the signal's arguments.
     * @return Handle that cancels the subscription when it is destroyed; keep it (or call
     *         detach() on it) unless the subscription is meant to be cancelled right away.
     */
    [[nodiscard]] Connection connect(Handler handler)
    {
        return Connection(addSlot(std::move(handler)));
    }

    /**
     * @brief Sets whether firing is currently suppressed.
     *
     * @param val true to suppress firing.
     * @return The previous state.
     */
    bool setBlocked(bool val) noexcept
    {
        return is_blocked_.exchange(val, std::memory_order_acq_rel);
    }

    /**
     * @brief Returns whether firing is suppressed.
     *
     * @return true when trigger() does nothing.
     */
    [[nodiscard]] bool isBlocked() const noexcept
    {
        return is_blocked_.load(std::memory_order_acquire);
    }

    /**
     * @brief Calls every live handler, in subscription order.
     *
     * Runs on the calling thread with no lock held: handlers are user code and
     * may subscribe, unsubscribe or re-fire. Called are exactly the handlers
     * subscribed when firing started, minus those unsubscribed before they were
     * reached.
     *
     * @param args Arguments forwarded to every handler.
     */
    void trigger(TArgs... args)
    {
        if (is_blocked_.load(std::memory_order_acquire)) {
            return;
        }

        // No lock: the published table is immutable and the reference taken here is
        // what keeps it - and every slot in it - alive for the whole walk, so a
        // concurrent connect() can do nothing worse than publish a new table and flip a
        // slot's alive flag. A firing never waits for a subscription, which is what Qt's
        // connection lists guarantee too.
        const std::shared_ptr<const Table> snapshot = table_.load(std::memory_order_acquire);

        for (const auto& slot : *snapshot) {
            if (slot->alive.load(std::memory_order_acquire)) {
                slot->callback(args...);
            }
        }
    }

    /**
     * @brief Disconnects every handler; every handle becomes inactive.
     *
     * Safe from any thread; handlers already running finish, and a firing that
     * already snapshotted the table stops calling the rest.
     */
    void disconnectAll()
    {
        std::lock_guard<std::mutex> lock(mutation_mutex_);
        const auto                  current = table_.load(std::memory_order_acquire);
        for (const auto& slot : *current) {
            slot->alive.store(false, std::memory_order_release);
        }
        table_.store(std::make_shared<Table>(), std::memory_order_release);
    }

  private:
    /**
     * @brief Appends a slot and publishes the table that contains it.
     *
     * @param handler Callback to append; called with the signal's arguments.
     * @return The published slot.
     */
    std::shared_ptr<Slot> addSlot(Handler handler)
    {
        auto slot      = std::make_shared<Slot>();
        slot->callback = std::move(handler);

        std::lock_guard<std::mutex> lock(mutation_mutex_);

        auto next = std::make_shared<Table>(*table_.load(std::memory_order_acquire));

        // Cancelled slots - a Connection that already destroyed itself - are dropped while
        // the table is being copied anyway, so subscribing and unsubscribing in a loop does
        // not grow it.
        next->erase(std::remove_if(next->begin(), next->end(),
                                   [](const auto& existing) {
                                       return !existing->alive.load(std::memory_order_relaxed);
                                   }),
                    next->end());

        next->push_back(std::move(slot));
        const auto published = next->back();
        table_.store(std::move(next), std::memory_order_release);
        return published;
    }

    /// Guards the slot table. Every mutator holds it while it replaces the table;
    /// trigger() does not take it at all, and it is never held while a handler runs,
    /// so re-entering any of these methods from a handler is safe.
    std::mutex mutation_mutex_;

    /// Current slot table; replaced as a whole by connect()/disconnectAll().
    ///
    /// Atomic, although the mutators hold the mutex, because trigger() reads it
    /// WITHOUT that mutex - that is the point: a firing never waits for a subscription.
    /// Every access, including the ones inside the critical sections, has to go through
    /// the atomic: one thread reading a plain shared_ptr while another thread writes it
    /// is a data race even when the writer holds a lock the reader does not take, and
    /// the release/acquire pair is what publishes a new table - and the subscriptions in
    /// it - to a firing thread. std::atomic<std::shared_ptr<T>> is the standard-library
    /// form of Qt's hand-rolled atomic ConnectionData pointer; libstdc++ implements it
    /// with an internal spinlock, which can never block on user code and never deadlocks.
    /// Measured price of that guarantee, against copying the table under the mutex:
    /// empty firing 4.8 -> 13.2 ns, 20 handlers 27.2 -> 24.8 ns, subscribe plus
    /// unsubscribe +32 ns per pair.
    std::atomic<std::shared_ptr<const Table>> table_{ std::make_shared<const Table>() };

    /// Suppresses firing while set. Atomic because trigger() reads it without the
    /// mutex, like the slot table above.
    std::atomic<bool> is_blocked_{ false };
};

VN_CORE_NS_END
