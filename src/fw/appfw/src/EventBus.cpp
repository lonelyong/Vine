#include <vine/appfw/EventBus.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/logging/Log.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// Views a String's UTF-8 bytes without allocating.
/// The calls that use it sit inside catch blocks, where an allocation or a logger failure would
/// turn a handled subscriber failure into a std::terminate().
std::string_view toUtf8View(const String& s) noexcept
{
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

/**
 * @brief Visits a class's interfaces and, transitively, the interfaces they extend.
 *
 * Iterative and pruned: the walk must not depend on how deep interfaces nest, and
 * every type is visited at most once, so a diamond hierarchy costs O(V + E)
 * instead of one walk per path through it (which is exponential in the number of
 * nested diamonds). A type that was already visited has its whole subtree skipped.
 *
 * @param roots Direct interfaces of one class, in declaration order.
 * @param visited Types already visited; shared across the whole walk and grown here.
 * @param visit Callable invoked once per type, an interface before its parents.
 */
template <typename Visit>
void forEachInterface(std::span<const vn::Type* const> roots, std::vector<const vn::Type*>& visited, Visit&& visit)
{
    // Explicit stack, filled in reverse so that the interface declared first is
    // visited first: the visit order is part of the delivery contract.
    std::vector<const vn::Type*> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const vn::Type* itf = stack.back();
        stack.pop_back();
        if (itf == nullptr || std::ranges::find(visited, itf) != visited.end()) {
            continue; // already visited (or malformed metadata): prune the subtree
        }
        visited.push_back(itf);
        visit(itf);
        const auto bases = itf->interfaces();
        stack.insert(stack.end(), bases.rbegin(), bases.rend());
    }
}

/**
 * @brief One subscription: its handler and everything the bus has to know about it.
 *
 * Holds no pointer back to the bus or to its channel, so nothing that leaves the
 * bus - a Subscription handle, a parked delivery - can dangle when the bus is
 * destroyed. Only active_ is mutable, so cancelling a subscription never races
 * with an invocation that is already running.
 *
 * The delivery thread lives here rather than beside the entry in the channel: a
 * subscription's properties belong to the subscription, and a parked delivery
 * then reads the mode it was queued with from the one object it already holds.
 */
class SubscriptionState {
  public:
    using Handler = std::function<void(const std::shared_ptr<const Object>&)>;

    SubscriptionState(std::size_t id, Handler handler, SubscriptionThreadMode mode, String tag)
      : id_(id)
      , handler_(std::move(handler))
      , mode_(mode)
      , tag_(std::move(tag))
    {}

    /// Unique within the subscription's channel, i.e. within one event type.
    std::size_t id() const noexcept
    {
        return id_;
    }

    /// Delivery thread the subscriber asked for.
    SubscriptionThreadMode mode() const noexcept
    {
        return mode_;
    }

    /// Free-form label the subscriber passed to subscribe(); may be empty.
    const String& tag() const noexcept
    {
        return tag_;
    }

    bool isActive() const noexcept
    {
        return active_.load(std::memory_order_acquire);
    }

    void deactivate() noexcept
    {
        active_.store(false, std::memory_order_release);
    }

    void invoke(const std::shared_ptr<const Object>& event) const
    {
        handler_(event);
    }

  private:
    const std::size_t            id_;
    const Handler                handler_;
    const SubscriptionThreadMode mode_;
    const String                 tag_;
    std::atomic<bool>            active_{ true };
};

/// Logs a subscriber failure - the only report when no hook is installed - and
/// hands it to the installed hook. Never throws: the observation path must not
/// become a new failure source.
void reportFailure(const std::shared_ptr<const EventBusErrorHandler>& handler,
                   const std::shared_ptr<SubscriptionState>&          state,
                   const std::shared_ptr<const Object>&               event,
                   bool                                               deferred,
                   std::exception_ptr                                 error) noexcept
{
    // The message belongs to the caught exception, and rethrowing is the only way
    // back to what() once its catch clause has been left. Neither log call throws.
    try {
        std::rethrow_exception(error);
    }
    catch (const std::exception& caught) {
        VN_LOGE("EventBus: subscriber {} of '{}' threw: {}", state->id(), toUtf8View(event->getType()->name()), caught.what());
    }
    catch (...) {
        VN_LOGE("EventBus: subscriber {} of '{}' threw a non-standard exception", state->id(), toUtf8View(event->getType()->name()));
    }

    if (handler == nullptr) {
        return;
    }
    // Building the context allocates, and this function is noexcept: a failure here
    // must not become a std::terminate in the middle of the error path.
    try {
        EventBusError context;
        context.event_type    = event->getType();
        context.subscriber_id = state->id();
        context.tag           = state->tag();
        context.error         = std::move(error);
        context.mode          = state->mode();
        context.deferred      = deferred;
        (*handler)(context);
    }
    catch (...) {
        // Documented as not throwing; ignore it if it does anyway.
    }
}

/// Delivers one event to one subscription and contains whatever the handler does.
///
/// Never throws, so one bad subscriber cannot break a publish and the subscribers
/// after it still run. The event is never null: publish() refuses an empty one
/// before it plans anything.
void deliver(const std::shared_ptr<SubscriptionState>&          state,
             const std::shared_ptr<const Object>&               event,
             const std::shared_ptr<const EventBusErrorHandler>& error_handler,
             bool                                               deferred) noexcept
{
    try {
        state->invoke(event);
    }
    catch (...) {
        reportFailure(error_handler, state, event, deferred, std::current_exception());
    }
}

/**
 * @brief One delivery parked in the event loop: which subscription, and which event.
 *
 * Immutable, so it needs no synchronization; a DeliveryRegistry owns it for
 * exactly as long as the delivery is parked. It keeps the event alive and only
 * observes the subscription, so cancelling a subscription still releases its
 * handler while a delivery for it is queued.
 */
class PendingDelivery {
  public:
    PendingDelivery(std::weak_ptr<SubscriptionState> state, std::shared_ptr<const Object> event, std::shared_ptr<const EventBusErrorHandler> error_handler)
      : state_(std::move(state))
      , event_(std::move(event))
      , error_handler_(std::move(error_handler))
    {}

    /// The subscription to deliver to, or nullptr once nothing owns it any more.
    std::shared_ptr<SubscriptionState> lockState() const noexcept
    {
        return state_.lock();
    }

    /// The event; never null, because publish() refuses an empty one before it
    /// plans anything.
    const std::shared_ptr<const Object>& event() const noexcept
    {
        return event_;
    }

    /// The failure hook that was installed when the delivery was parked.
    const std::shared_ptr<const EventBusErrorHandler>& errorHandler() const noexcept
    {
        return error_handler_;
    }

  private:
    const std::weak_ptr<SubscriptionState>            state_;
    const std::shared_ptr<const Object>               event_;
    const std::shared_ptr<const EventBusErrorHandler> error_handler_;
};

/**
 * @brief Registry of the deliveries currently parked in the event loop.
 *
 * Deliberately detached from the bus: a queued task keeps the registry itself
 * alive and only ever touches the registry and its own delivery, so it stays
 * valid after the bus is destroyed. Dropping the deliveries (clear()) releases
 * the events they hold immediately, without waiting for the event loop.
 */
class DeliveryRegistry {
  public:
    void add(std::shared_ptr<PendingDelivery> pending)
    {
        std::lock_guard lock(mutex_);
        pending_.push_back(std::move(pending));
    }

    void remove(const std::weak_ptr<PendingDelivery>& pending) noexcept
    {
        const auto locked = pending.lock();
        if (locked == nullptr) {
            return; // already dropped by clear()
        }
        std::shared_ptr<PendingDelivery> removed;
        {
            std::lock_guard lock(mutex_);
            const auto      it = std::ranges::find(pending_, locked);
            if (it != pending_.end()) {
                removed = std::move(*it);
                pending_.erase(it);
            }
        }
        // `removed` is destroyed here, outside the lock: releasing a delivery can
        // destroy its event, i.e. run arbitrary user code.
    }

    void clear() noexcept
    {
        std::vector<std::shared_ptr<PendingDelivery>> dropped;
        {
            std::lock_guard lock(mutex_);
            dropped.swap(pending_);
        }
        // Dropped outside the lock: releases every event held for a parked delivery.
    }

    std::size_t pendingCount() const
    {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

  private:
    mutable std::mutex                            mutex_;
    std::vector<std::shared_ptr<PendingDelivery>> pending_;
};

/**
 * @brief State of one queued task; lives exactly as long as the task in the event loop.
 *
 * Owns nothing from the bus beyond the shared registry, and unregisters its
 * delivery when the task dies - whether it ran, was cancelled, or was dropped by
 * the event loop. Held behind a shared_ptr so that Qt's copies of the task share
 * one instance and therefore one unregistration.
 */
class DeliveryTask {
  public:
    DeliveryTask(std::shared_ptr<DeliveryRegistry> registry, std::shared_ptr<PendingDelivery> pending)
      : registry_(std::move(registry))
      , pending_(std::move(pending))
    {}

    ~DeliveryTask()
    {
        registry_->remove(pending_);
    }

    DeliveryTask(const DeliveryTask&)            = delete;
    DeliveryTask& operator=(const DeliveryTask&) = delete;

    /// Delivers unless the subscription is gone or the bus dropped the delivery;
    /// never throws.
    void run() noexcept
    {
        const auto pending = pending_.lock();
        if (pending == nullptr) {
            return; // dropped by shutdown()/~EventBus
        }
        const auto state = pending->lockState();
        if (state == nullptr || !state->isActive()) {
            return; // subscription cancelled, or the bus was stopped
        }
        deliver(state, pending->event(), pending->errorHandler(), /* deferred */ true);
    }

  private:
    const std::shared_ptr<DeliveryRegistry> registry_;
    const std::weak_ptr<PendingDelivery>    pending_;
};

/// Parks one delivery in the event loop.
///
/// The task keeps only the registry and its own delivery alive, so it neither
/// references nor keeps alive the bus: shutdown() releases the event by dropping
/// the delivery from the registry, and a task that is still queued then runs as
/// a no-op.
///
/// @param registry      Registry owned by the bus the delivery belongs to.
/// @param state         Subscription to deliver to; observed weakly.
/// @param event         Event to deliver; never null (see publish()).
/// @param error_handler Failure hook installed when the delivery was parked.
void parkDelivery(const std::shared_ptr<DeliveryRegistry>&           registry,
                  const std::shared_ptr<SubscriptionState>&          state,
                  const std::shared_ptr<const Object>&               event,
                  const std::shared_ptr<const EventBusErrorHandler>& error_handler)
{
    auto pending = std::make_shared<PendingDelivery>(state, event, error_handler);
    auto task    = std::make_shared<DeliveryTask>(registry, pending);
    registry->add(std::move(pending));
    if (!MainThreadDispatcher::postToMainThread([task] { task->run(); })) {
        // The refused task went away with the lambda, and its destructor already
        // unregistered the delivery: nothing is left behind.
        VN_LOGW("EventBus: dropping a Main delivery, the event loop refused the task");
    }
}

/// The subscriptions of one event type, in subscription order.
///
/// Two locks, two facts: Impl::channels_mutex decides which channels exist - only
/// subscribe() inserts into the map, so only it needs the exclusive lock - and a
/// channel's mutex decides its own list. The channel lock is always taken while the
/// map lock is held, so the order is fixed and there is no cycle; that is also what
/// lets two concurrent publications reap the same list without racing.
class EventChannel {
  public:
    /// Registers a subscription and hands the cancelled entries to the caller,
    /// which destroys them once every lock has been released.
    std::shared_ptr<SubscriptionState>
    subscribe(SubscriptionState::Handler handler, SubscriptionThreadMode mode, String tag, std::vector<std::shared_ptr<SubscriptionState>>& garbage);

    /// Appends a snapshot of the live subscriptions; cancelled entries are reaped
    /// into garbage so that the caller destroys their closures outside every lock.
    void collect(std::vector<std::shared_ptr<SubscriptionState>>& out, std::vector<std::shared_ptr<SubscriptionState>>& garbage);

    /// Deactivates every subscription and moves the entries to garbage.
    void takeAll(std::vector<std::shared_ptr<SubscriptionState>>& garbage);

  private:
    void reap(std::vector<std::shared_ptr<SubscriptionState>>& garbage);

    std::vector<std::shared_ptr<SubscriptionState>> handlers_;
    std::size_t                                     last_id_ = 0;
    mutable std::mutex                              mutex_;
};

std::shared_ptr<SubscriptionState>
EventChannel::subscribe(SubscriptionState::Handler handler, SubscriptionThreadMode mode, String tag, std::vector<std::shared_ptr<SubscriptionState>>& garbage)
{
    std::lock_guard                    lock(mutex_);
    std::shared_ptr<SubscriptionState> state = std::make_shared<SubscriptionState>(++last_id_, std::move(handler), mode, std::move(tag));
    reap(garbage);
    handlers_.push_back(state);
    return state;
}

void EventChannel::collect(std::vector<std::shared_ptr<SubscriptionState>>& out, std::vector<std::shared_ptr<SubscriptionState>>& garbage)
{
    std::lock_guard lock(mutex_);
    reap(garbage);
    out.insert(out.end(), handlers_.begin(), handlers_.end());
}

void EventChannel::takeAll(std::vector<std::shared_ptr<SubscriptionState>>& garbage)
{
    std::lock_guard lock(mutex_);
    for (const auto& state : handlers_) {
        state->deactivate();
    }
    // The copies in garbage keep every state - and therefore every handler
    // closure - alive, so clearing the channel runs no user code under the lock.
    garbage.insert(garbage.end(), handlers_.begin(), handlers_.end());
    handlers_.clear();
}

void EventChannel::reap(std::vector<std::shared_ptr<SubscriptionState>>& garbage)
{
    // Cancelling a subscription only flips its state's atomic flag; the entry is
    // reaped here, and handed to the caller so that the handler closure is
    // destroyed outside the channel lock and outside the map lock.
    std::erase_if(handlers_, [&garbage](const std::shared_ptr<SubscriptionState>& state) {
        if (state->isActive()) {
            return false;
        }
        garbage.push_back(state);
        return true;
    });
}

/**
 * @brief Admission and the stop handshake: who enters the bus, who stops it, and
 *        who waits for whom.
 *
 * One mutex guards one decision - whether a call is still admitted - and that is
 * what makes "a graceful stop returned true ⇒ nobody is still inside" hold: a
 * call is either counted before the state leaves Running or refused, and the two
 * cannot cross. Pure observers of the decision (isStopping(), the admission fast
 * path) read the atomic mirror instead of taking the lock.
 *
 * A thread's own admitted calls are excluded from every wait, because they cannot
 * finish before the wait returns. That count is kept per gate: a call this thread
 * makes through another bus says nothing about this one.
 */
class StopGate {
  public:
    /// A wait bound; `Deadline::max()` waits without one.
    using Deadline = std::chrono::steady_clock::time_point;

    /// What a stop request found.
    enum class Order
    {
        Perform,   // this call performs the stop and must then call finish()
        Completed, // another thread's stop is over; result() says what it reported
        Reentrant, // the caller is running that stop and cannot wait for itself
        TimedOut,  // another thread's stop is still running and the deadline passed
    };

    /**
     * @brief One admitted call: the gate admits work for as long as a ticket lives.
     */
    class Ticket {
      public:
        /**
         * @brief Admits one call, or builds a ticket that is not admitted.
         *
         * @param gate Gate to enter; must outlive the ticket.
         */
        explicit Ticket(StopGate& gate)
          : gate_(&gate)
        {
            if (gate_->isStopping()) {
                return; // fast reject: a gate that is stopping never admits again
            }
            gate_->enterThreadCall(); // may allocate: kept outside the count
            std::lock_guard lock(gate_->mutex_);
            if (gate_->state_ != State::Running) {
                gate_->leaveThreadCall();
                return;
            }
            ++gate_->active_calls_;
            admitted_ = true;
        }

        ~Ticket()
        {
            if (!admitted_) {
                return;
            }
            gate_->leaveThreadCall();
            std::lock_guard lock(gate_->mutex_);
            --gate_->active_calls_;
            gate_->cv_.notify_all();
        }

        Ticket(const Ticket&)            = delete;
        Ticket& operator=(const Ticket&) = delete;

        /// false when the gate was already stopping: the call must do nothing.
        bool admitted() const noexcept
        {
            return admitted_;
        }

      private:
        StopGate* const gate_;
        bool            admitted_ = false;
    };

    /// true from the moment the stop begins, i.e. already while a graceful stop is
    /// still draining.
    bool isStopping() const noexcept
    {
        return stopping_.load(std::memory_order_acquire);
    }

    /// The result the last completed stop reported; meaningful after Order::Completed.
    bool result() const noexcept
    {
        std::lock_guard lock(mutex_);
        return result_;
    }

    /**
     * @brief Asks to stop the bus and reports who owns the stop now.
     *
     * @details
     * - `Perform`: this caller owns the Running → Stopping transition; it runs the
     *   cancel (and drain) pass and then calls finish().
     * - `Completed`: a stop is already over, and result() is that stop's answer.
     * - `Reentrant`: this thread is the one running the stop; waiting would be
     *   waiting for itself, so the caller returns and reports failure if it must
     *   report something.
     * - `TimedOut`: a stop runs elsewhere and the deadline passed.
     *
     * @param deadline How long to wait for a stop that is already running;
     *                 `Deadline::max()` for no bound.
     * @return What this request found.
     */
    Order request(Deadline deadline) noexcept
    {
        std::unique_lock lock(mutex_);
        if (state_ == State::Running) {
            // The stopper is published with the state, under the same lock, so no
            // reader can see Stopping with a thread id that does not belong to it.
            stopper_ = std::this_thread::get_id();
            state_   = State::Stopping;
            stopping_.store(true, std::memory_order_release);
            return Order::Perform;
        }
        if (state_ == State::Stopping && stopper_ == std::this_thread::get_id()) {
            return Order::Reentrant;
        }
        if (!cv_.wait_until(lock, deadline, [this] { return state_ == State::Stopped; })) {
            return Order::TimedOut;
        }
        return Order::Completed;
    }

    /**
     * @brief Completes the stop this caller performed and publishes its outcome.
     *
     * @param clean true only when the caller waited for the admitted calls and ran
     *              every parked delivery; a stop that dropped the remaining work
     *              passes false, so that a later graceful caller is not told that
     *              everything ran.
     */
    void finish(bool clean) noexcept
    {
        std::lock_guard lock(mutex_);
        result_ = clean;
        state_  = State::Stopped;
        cv_.notify_all();
    }

    /**
     * @brief Waits until no thread other than this one is inside an admitted call.
     *
     * @param deadline How long to wait; `Deadline::max()` for no bound.
     * @return true when only this thread's own calls were left.
     */
    bool awaitCallers(Deadline deadline)
    {
        const int self = threadCallDepth();
        std::unique_lock lock(mutex_);
        return cv_.wait_until(lock, deadline, [this, self] { return active_calls_ <= self; });
    }

  private:
    /// Lifecycle of the gate: Running admits calls, Stopping refuses new ones while
    /// the stop in progress drains, Stopped means no caller is inside any more.
    enum class State
    {
        Running,
        Stopping,
        Stopped,
    };

    /// Admitted calls this thread holds on one gate.
    struct ThreadCall {
        const StopGate* gate;
        int             depth;
    };

    /// The gates this thread is inside, one entry each; an entry disappears when
    /// its depth returns to zero, so no stale key can outlive its gate.
    static std::vector<ThreadCall>& threadCalls() noexcept;

    /// Records one admitted call of this thread; may allocate.
    void enterThreadCall();
    /// Drops one admitted call of this thread.
    void leaveThreadCall() noexcept;
    /// Admitted calls this thread holds on this gate (0 when it holds none).
    int threadCallDepth() const noexcept;

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    int                     active_calls_ = 0;
    State                   state_        = State::Running;
    bool                    result_       = false; // meaningful once state_ == Stopped
    /// Thread performing the Stopping → Stopped transition; guarded by mutex_ and
    /// only meaningful while state_ == Stopping. A reentrant stop request from that
    /// same thread (a handler closure's destructor running inside a cancel pass)
    /// must not wait for a stop it is itself holding up.
    std::thread::id stopper_;
    /// Mirror of state_ != Running: keeps isStopping() and the admission fast path
    /// lock-free. Written with the transition, under mutex_.
    std::atomic<bool> stopping_{ false };
};

std::vector<StopGate::ThreadCall>& StopGate::threadCalls() noexcept
{
    static thread_local std::vector<ThreadCall> calls;
    return calls;
}

void StopGate::enterThreadCall()
{
    auto&      calls = threadCalls();
    const auto entry = std::ranges::find(calls, this, &ThreadCall::gate);
    if (entry != calls.end()) {
        ++entry->depth;
        return;
    }
    calls.push_back(ThreadCall{ this, 1 });
}

void StopGate::leaveThreadCall() noexcept
{
    auto&      calls = threadCalls();
    const auto entry = std::ranges::find(calls, this, &ThreadCall::gate);
    if (entry == calls.end()) {
        return;
    }
    if (--entry->depth == 0) {
        calls.erase(entry);
    }
}

int StopGate::threadCallDepth() const noexcept
{
    const auto& calls = threadCalls();
    const auto  entry = std::ranges::find(calls, this, &ThreadCall::gate);
    return entry != calls.end() ? entry->depth : 0;
}

} // namespace

/// Token state: owns the subscription and cancels it when the handle dies.
struct Subscription::Control {
    explicit Control(std::shared_ptr<SubscriptionState> state) noexcept
      : state_(std::move(state))
    {}

    ~Control()
    {
        state_->deactivate();
    }

    bool isActive() const noexcept
    {
        return state_->isActive();
    }

    std::shared_ptr<SubscriptionState> state_;
};

Subscription::Subscription(std::shared_ptr<Control> control) noexcept
  : control_(std::move(control))
{}

Subscription::~Subscription() = default;

Subscription::Subscription(Subscription&& other) noexcept
  : control_(std::move(other.control_))
{}

Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other) {
        // Guarded on purpose: a self-move of std::shared_ptr would empty it, and
        // releasing the previous control cancels the previous subscription.
        control_ = std::move(other.control_);
    }
    return *this;
}

void Subscription::unsubscribe() noexcept
{
    control_.reset();
}

bool Subscription::isActive() const noexcept
{
    return control_ != nullptr && control_->isActive();
}

struct EventBus::Impl {
    /// Subscriptions per event type. The map's lock decides which channels exist -
    /// shared for every reader, exclusive only for subscribe(), which inserts; a
    /// channel's own lock decides its contents and is always taken while the map
    /// lock is held, so the order is fixed and there is no cycle.
    std::map<vn::TypeId, EventChannel> channels;
    mutable std::shared_mutex          channels_mutex;

    /// The deliveries parked in the event loop. Shared with their queued tasks so
    /// that the registry stays valid after the bus is destroyed.
    std::shared_ptr<DeliveryRegistry> deliveries{ std::make_shared<DeliveryRegistry>() };

    /// Admission and the stop handshake.
    StopGate gate;

    /// Optional observation hook for subscriber exceptions. A delivery only pays a
    /// single load unless a handler is installed, so the happy path stays as is.
    std::atomic<bool>                           has_error_handler{ false };
    mutable std::mutex                          error_mutex;
    std::shared_ptr<const EventBusErrorHandler> error_handler; // guarded by error_mutex

    /// Installs (or clears) the error handler.
    void setErrorHandler(EventBusErrorHandler handler);

    /// The installed handler, or nullptr; the snapshot stays valid if it is later
    /// replaced.
    std::shared_ptr<const EventBusErrorHandler> currentErrorHandler() const;

    /// Snapshots the subscriptions that match event, most derived type first (see
    /// EventBus::publish()); reaped handler closures are destroyed here, outside
    /// every lock.
    void collectPlan(const Object& event, std::vector<std::shared_ptr<SubscriptionState>>& plan);

    /// Cancels every subscription, then drops the deliveries that are still parked.
    void cancelSubscriptions();

    /// Runs the parked deliveries until the deadline; false when they could not all
    /// run, so nothing may claim they did.
    bool drainPendingDeliveries(MainThreadDispatcher* dispatcher, StopGate::Deadline deadline);
};

void EventBus::Impl::collectPlan(const Object& event, std::vector<std::shared_ptr<SubscriptionState>>& plan)
{
    // One consistent plan per publish: the most derived class first, then the
    // interfaces it declares, then the base classes and their interfaces - all
    // snapshotted under the map lock, so subscribing or unsubscribing during a
    // dispatch only affects later publications. `visited` prunes the interface walk,
    // so a diamond hierarchy is walked once.
    std::vector<std::shared_ptr<SubscriptionState>> garbage;
    std::vector<vn::TypeId>                         visited;
    {
        std::shared_lock lock(channels_mutex);
        const auto       consider = [&](const vn::Type* type) {
            if (const auto it = channels.find(type); it != channels.end()) {
                it->second.collect(plan, garbage);
            }
        };
        for (const vn::Type* cls = event.getType(); cls != nullptr; cls = cls->parent()) {
            consider(cls);
            forEachInterface(cls->interfaces(), visited, consider);
        }
    }
    // Reaped handler closures are destroyed here, outside every lock, so their
    // destructors may call back into the bus.
    garbage.clear();
}

void EventBus::Impl::cancelSubscriptions()
{
    // Cancel the subscriptions first, so a task that is being dequeued right now
    // sees an inactive subscription and skips its handler. The channels and the map
    // themselves stay alive (only ~Impl destroys them), so a publish() that already
    // took its plan cannot dangle: it either finds an empty channel or delivers what
    // it snapshotted.
    std::vector<std::shared_ptr<SubscriptionState>> garbage;
    {
        std::shared_lock lock(channels_mutex);
        for (auto& entry : channels) {
            entry.second.takeAll(garbage);
        }
    }
    // Handler closures are destroyed outside every lock, so their destructors may
    // call back into the bus.
    garbage.clear();

    // Finally drop the parked deliveries: this releases the events they hold
    // without waiting for the event loop, and makes their tasks no-ops.
    deliveries->clear();
}

bool EventBus::Impl::drainPendingDeliveries(MainThreadDispatcher* dispatcher, StopGate::Deadline deadline)
{
    if (deliveries->pendingCount() == 0) {
        return true;
    }
    if (dispatcher == nullptr) {
        return false; // nothing can marshal them: they will never run here
    }
    while (deliveries->pendingCount() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const auto before = deliveries->pendingCount();
        if (!dispatcher->deliverPostedCalls()) {
            return false; // only the application thread can pump the queue
        }
        if (deliveries->pendingCount() >= before) {
            // Nothing ran in this round: another thread may own the queue, so give
            // it a slot instead of spinning on it.
            std::this_thread::yield();
        }
    }
    return true;
}

void EventBus::Impl::setErrorHandler(EventBusErrorHandler handler)
{
    auto holder = handler ? std::make_shared<const EventBusErrorHandler>(std::move(handler)) : nullptr;
    {
        std::lock_guard lock(error_mutex);
        error_handler = std::move(holder);
        // Published after the pointer, so a reader that sees true reads the hook.
        has_error_handler.store(error_handler != nullptr, std::memory_order_release);
    }
}

std::shared_ptr<const EventBusErrorHandler> EventBus::Impl::currentErrorHandler() const
{
    std::lock_guard lock(error_mutex);
    return error_handler;
}

EventBus::EventBus(MainThreadDispatcher* dispatcher)
  : d(new Impl)
  , dispatcher_(dispatcher)
{}

EventBus::~EventBus()
{
    // Refuse admission and cancel what is pending, so a call that is already inside
    // finishes promptly, and only then let Impl go. The contract says the bus is
    // destroyed while nothing else calls it; this wait turns a violation into a short
    // delay instead of a dangling Impl.
    if (d->gate.request(StopGate::Deadline::max()) == StopGate::Order::Perform) {
        d->cancelSubscriptions();
        d->gate.finish(/* clean */ false);
    }
    // Completed: another thread's stop is over. Reentrant: the stopper is this very
    // thread - a handler, or a closure the stop just reaped, destroying the bus -
    // which no wait can repair, so it stays a documented contract violation.
    d->gate.awaitCallers(StopGate::Deadline::max());
}

void EventBus::shutdown()
{
    // request() waits out a stop that another thread is running; only Perform leaves
    // work for this call, and Reentrant (this thread is already stopping) has nothing
    // to wait for.
    if (d->gate.request(StopGate::Deadline::max()) == StopGate::Order::Perform) {
        d->cancelSubscriptions();
        d->gate.finish(/* clean */ false);
    }
}

bool EventBus::shutdownGracefully(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    switch (d->gate.request(deadline)) {
        case StopGate::Order::Completed:
            // Every caller of a finished stop reports that stop's answer.
            return d->gate.result();
        case StopGate::Order::Reentrant:
        case StopGate::Order::TimedOut:
            // This call drained nothing: it would have to wait for itself, or the stop
            // it waited for is still running elsewhere.
            return false;
        case StopGate::Order::Perform:
            break;
    }

    // Admission is refused from here on. Let the admitted callers finish, run what
    // they parked - a parked delivery still sees an active subscription - and cancel
    // only what is left.
    const bool callers    = d->gate.awaitCallers(deadline);
    const bool deliveries = d->drainPendingDeliveries(dispatcher_, deadline);
    d->cancelSubscriptions();

    const bool drained = callers && deliveries;
    d->gate.finish(drained);
    return drained;
}

bool EventBus::isShutDown() const noexcept
{
    return d->gate.isStopping();
}

std::size_t EventBus::pendingDeliveryCount() const
{
    return d->deliveries->pendingCount();
}

void EventBus::setErrorHandler(EventBusErrorHandler handler)
{
    d->setErrorHandler(std::move(handler));
}

Subscription
EventBus::subscribeErased(vn::TypeId type, std::function<void(const std::shared_ptr<const Object>&)> handler, SubscriptionThreadMode mode, const String& tag)
{
    const StopGate::Ticket ticket(d->gate);
    if (!ticket.admitted()) {
        return Subscription(); // stopping or stopped: never register again
    }

    std::shared_ptr<SubscriptionState>             state;
    std::vector<std::shared_ptr<SubscriptionState>> garbage;
    {
        std::unique_lock lock(d->channels_mutex);
        // Re-checked under the map lock: cancelSubscriptions() empties the channels
        // under the same lock, so a registration racing with the stop is refused
        // instead of surviving it. Together with the admission above this makes "no
        // live subscription exists once shutdown() has returned" hold.
        if (d->gate.isStopping()) {
            return Subscription();
        }
        state = d->channels[type].subscribe(std::move(handler), mode, tag, garbage);
    }
    // Reaped handler closures are destroyed here, outside every lock, so their
    // destructors may call back into the bus.
    garbage.clear();

    try {
        // The state is passed by value, so it is still valid if the allocation of the
        // control block throws.
        return Subscription(std::make_shared<Subscription::Control>(state));
    }
    catch (...) {
        // Registration succeeded but no handle could be built: make the orphan inert,
        // so a subscription nobody can cancel is at least never invoked, and let the
        // failure surface.
        state->deactivate();
        throw;
    }
}

void EventBus::publish(const std::shared_ptr<const Object>& event)
{
    if (!event) {
        return;
    }
    const StopGate::Ticket ticket(d->gate);
    if (!ticket.admitted()) {
        return; // stopping or stopped: no new work enters the bus
    }

    std::vector<std::shared_ptr<SubscriptionState>> plan;
    d->collectPlan(*event, plan);

    // Thread policy, resolved once per publish. Without a marshaller there is nowhere
    // to marshal to, so Main/Auto degrade to the publishing thread.
    const bool marshals = dispatcher_ != nullptr && MainThreadDispatcher::hasEventLoop();
    const bool on_main  = !marshals || MainThreadDispatcher::isMainThread();
    // Failure observation snapshot: one load when no handler is installed.
    const auto error_handler = d->has_error_handler.load(std::memory_order_acquire) ? d->currentErrorHandler() : nullptr;

    for (const auto& state : plan) {
        if (!state->isActive()) {
            continue; // cancelled by unsubscribe()/shutdown() before its turn
        }
        // Main, or Auto off the main thread, becomes a parked delivery - unless there is
        // nowhere to park it, in which case it runs here (see above).
        const auto mode = state->mode();
        const bool park = marshals && (mode == SubscriptionThreadMode::Main || (mode == SubscriptionThreadMode::Auto && !on_main));
        if (!park) {
            deliver(state, event, error_handler, /* deferred */ false);
            continue;
        }
        parkDelivery(d->deliveries, state, event, error_handler);
    }
}

VN_APPFW_NS_END
