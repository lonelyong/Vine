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

V_CORE_NS_BEGIN

/**
 * @brief Thread-safe signal: any thread may subscribe, unsubscribe and fire.
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
 * subscribe() hands back an owning Subscription that cancels the subscription in its
 * destructor - the same shape, the same method names and the same entry point name as
 * appfw's EventBus::subscribe() - so a subscription kept in a member needs no manual
 * teardown, and one that outlives the Signal is inert instead of dangling.
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
     * and keeps it alive - while unsubscribe() flips the flag that stops it from being
     * called. The flag is atomic because it is read by trigger(), which holds no lock, and
     * written by the mutators, which only hold a lock the reader does not take. A plain
     * bool would need a lock around every check in the firing loop, and the release/acquire
     * pair makes the marking visible promptly instead of letting it be cached.
     */
    struct Slot {
        Handler           callback;
        std::atomic<bool> alive{ true };
    };

    /// The slot table: immutable once published, replaced as a whole by every mutation, and
    /// in subscription order, so it is only ever appended to and walked.
    ///
    /// A vector of pointers, not a map: the copy a mutator pays is then a contiguous
    /// pointer copy instead of a red-black tree rebuild, and the firing loop walks it
    /// sequentially instead of chasing tree nodes. Measured at 20 handlers - firing
    /// (std::map) 62.6 -> 24.8 ns, subscribe plus unsubscribe 538 -> 147 ns per pair.
    using Table = std::vector<std::shared_ptr<Slot>>;

  public:
    /**
     * @brief RAII subscription handle: cancels the subscription when it is destroyed.
     *
     * Same shape, and the same method names, as appfw's EventBus handle, so the two read
     * alike. Move-only, so exactly one owner cancels the subscription, and it refers to the
     * slot weakly: a handle that outlives its Signal is inert instead of dangling, and
     * isActive() reports false once the Signal is gone. Handing a member to one of these is
     * what removes the manual unsubscribe from the owner's teardown path.
     */
    class Subscription {
      public:
        /// @brief Creates a handle that owns nothing.
        Subscription() noexcept = default;

        /**
         * @brief Takes the subscription over from another handle.
         *
         * @param other Handle to move from; it stops owning the subscription.
         */
        Subscription(Subscription&& other) noexcept : slot_(std::move(other.slot_)) { }

        /// @brief Cancels what this handle owns. Destroying an empty handle does nothing.
        ~Subscription() { unsubscribe(); }

        /**
         * @brief Cancels the current subscription, then takes over another one.
         *
         * Assigning is how a member subscription is replaced without a manual unsubscribe
         * first: the previous subscription is cancelled here.
         *
         * @param other Handle to move from; it stops owning its subscription.
         * @return Reference to this handle.
         */
        Subscription& operator=(Subscription&& other) noexcept
        {
            if (this != &other) {
                unsubscribe();
                slot_ = std::move(other.slot_);
            }
            return *this;
        }

        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;

        /**
         * @brief Cancels the subscription.
         *
         * Idempotent, and safe to call from inside a handler: after it returns no further
         * invocation of this handler starts, while one already running completes.
         */
        void unsubscribe() noexcept
        {
            if (const auto slot = slot_.lock()) {
                slot->alive.store(false, std::memory_order_release);
            }
            slot_.reset();
        }

        /**
         * @brief Stops managing the subscription without cancelling it.
         *
         * For a handler that should stay subscribed for as long as the Signal lives, such
         * as a widget wiring an internal behaviour onto its own signal.
         */
        void release() noexcept { slot_.reset(); }

        /**
         * @brief Reports whether the handler can still be called.
         *
         * @return true while the slot exists and has not been cancelled.
         */
        bool isActive() const noexcept
        {
            const auto slot = slot_.lock();
            return slot != nullptr && slot->alive.load(std::memory_order_acquire);
        }

      private:
        friend class Signal;

        /**
         * @brief Wraps a freshly published slot; only the Signal creates these.
         *
         * @param slot Slot to own.
         */
        explicit Subscription(std::shared_ptr<Slot> slot) noexcept : slot_(std::move(slot)) { }

        std::weak_ptr<Slot> slot_;
    };

  public:
    Signal()              = default;
    Signal(const Signal&) = delete;
    Signal(Signal&&)      = delete;

  public:
    /**
     * @brief Subscribes a handler and returns the handle that owns the subscription.
     *
     * Safe from any thread, including from inside a handler and while another thread is
     * firing. Subscribing replaces the slot table rather than editing it, because a firing
     * thread may be walking the published one; the copy is contiguous and cheap, but it is
     * still the reason subscriptions belong at setup. The handler keeps running until the
     * returned handle is destroyed, unsubscribe() is called on it, or the Signal goes away.
     *
     * @param handler Callback to append; called with the signal's arguments.
     * @return Handle that cancels the subscription when it is destroyed; keep it (or call
     *         release() on it) unless the subscription is meant to be cancelled right away.
     */
    [[nodiscard]] Subscription subscribe(Handler handler)
    {
        return Subscription(addSlot(std::move(handler)));
    }

    /**
     * @brief Sets whether firing is currently suppressed.
     *
     * @param val true to suppress firing.
     * @return The previous state.
     */
    bool setBlocked(bool val)
    {
        return is_blocked_.exchange(val, std::memory_order_acq_rel);
    }

    /**
     * @brief Returns whether firing is suppressed.
     *
     * @return true when trigger() does nothing.
     */
    bool isBlocked() const
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
        // concurrent subscribe() can do nothing worse than publish a new table and flip a
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
     * @brief Unsubscribes every handler; every handle becomes inactive.
     *
     * Safe from any thread; handlers already running finish, and a firing that
     * already snapshotted the table stops calling the rest.
     */
    void unsubscribeAll()
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

        // Cancelled slots - a Subscription that already destroyed itself - are dropped while
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

    /// Current slot table; replaced as a whole by subscribe()/unsubscribeAll().
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

V_CORE_NS_END
