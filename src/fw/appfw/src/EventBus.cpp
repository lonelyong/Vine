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
#include <thread>
#include <utility>
#include <vector>

#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/logging/Log.hpp>

V_APPFW_NS_BEGIN

namespace
{

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
void forEachInterface(std::span<const vine::Type* const> roots, std::vector<const vine::Type*>& visited, Visit&& visit)
{
    // Explicit stack, filled in reverse so that the interface declared first is
    // visited first: the visit order is part of the delivery contract.
    std::vector<const vine::Type*> stack(roots.rbegin(), roots.rend());
    while (!stack.empty()) {
        const vine::Type* itf = stack.back();
        stack.pop_back();
        if (itf == nullptr || std::ranges::find(visited, itf) != visited.end()) {
            continue;  // already visited (or malformed metadata): prune the subtree
        }
        visited.push_back(itf);
        visit(itf);
        const auto bases = itf->interfaces();
        stack.insert(stack.end(), bases.rbegin(), bases.rend());
    }
}

/**
 * @brief Per-subscription state shared by the channel, the RAII handle and queued deliveries.
 *
 * Holds no pointer back to the bus or to its channel, so nothing that leaves the
 * bus (a Subscription handle, a queued delivery) can dangle when the bus is
 * destroyed. Only active_ is mutable, so cancelling a subscription never races
 * with an invocation that is already running.
 */
class SubscriptionState {
  public:
    using Handler = std::function<void(const std::shared_ptr<const Object>&)>;

    SubscriptionState(std::size_t id, Handler handler, String tag)
      : id_(id)
      , handler_(std::move(handler))
      , tag_(std::move(tag))
    {}

    std::size_t id() const noexcept { return id_; }

    /// Free-form label the subscriber passed to subscribe(); may be empty.
    const String& tag() const noexcept { return tag_; }

    bool isActive() const noexcept { return active_.load(std::memory_order_acquire); }

    void deactivate() noexcept { active_.store(false, std::memory_order_release); }

    void invoke(const std::shared_ptr<const Object>& event) const { handler_(event); }

  private:
    const std::size_t id_;
    const Handler     handler_;
    const String      tag_;
    std::atomic<bool> active_{ true };
};

/// One entry of a channel's ordered subscription list.
struct SubscriptionEntry {
    SubscriptionThreadMode             mode;
    std::shared_ptr<SubscriptionState> state;
};

/// Reports a handler failure to the log and, when installed, to the error handler.
///
/// Never throws: the observation path must not become a new failure source, and
/// the caller is already handling an exception.
void reportHandlerError(const std::shared_ptr<const EventBusErrorHandler>& handler,
                        const std::shared_ptr<SubscriptionState>&     state,
                        const std::shared_ptr<const Object>&          event,
                        SubscriptionThreadMode                        mode,
                        bool                                          deferred,
                        const std::exception*                         error,
                        std::exception_ptr                            error_ptr) noexcept
{
    // Logging cannot throw (see Logger: every level function is noexcept and reports
    // a failure on stderr instead), so it needs no guard of its own here.
    if (error != nullptr) {
        V_LOGE("EventBus: subscriber '{}' threw: {}", state->id(), error->what());
    }
    else {
        V_LOGE("EventBus: subscriber '{}' threw a non-standard exception", state->id());
    }

    if (handler == nullptr) {
        return;
    }
    // Everything below can allocate, and this function is noexcept: a failure while
    // building the context must not become a std::terminate in the middle of the
    // error path.
    try {
        EventBusError context;
        context.event_type    = event != nullptr ? event->getType() : nullptr;
        context.subscriber_id = state->id();
        context.tag           = state->tag();
        context.error         = std::move(error_ptr);
        context.mode          = mode;
        context.deferred      = deferred;
        (*handler)(context);
    }
    catch (...) {
        // Documented as not throwing; ignore it if it does anyway.
    }
}

void deliver(const std::shared_ptr<SubscriptionState>&               state,
             const std::shared_ptr<const Object>&                    event,
             const std::shared_ptr<const EventBusErrorHandler>&      error_handler,
             SubscriptionThreadMode                                  mode,
             bool                                                    deferred) noexcept
{
    try {
        state->invoke(event);
    }
    catch (const std::exception& error) {
        reportHandlerError(error_handler, state, event, mode, deferred, &error, std::current_exception());
    }
    catch (...) {
        reportHandlerError(error_handler, state, event, mode, deferred, nullptr, std::current_exception());
    }
}

/**
 * @brief Payload of a queued (Main) delivery: which subscription, and which event.
 *
 * Fully immutable, so it needs no synchronization; a DeliveryRegistry owns it
 * for exactly as long as the delivery is parked in the event loop.
 */
class Payload {
  public:
    Payload(std::weak_ptr<SubscriptionState>           state,
            std::shared_ptr<const Object>              event,
            SubscriptionThreadMode                     mode,
            std::shared_ptr<const EventBusErrorHandler> error_handler)
      : state_(std::move(state))
      , event_(std::move(event))
      , mode_(mode)
      , error_handler_(std::move(error_handler))
    {}

    std::shared_ptr<SubscriptionState> lockState() const noexcept { return state_.lock(); }

    const std::shared_ptr<const Object>& event() const noexcept { return event_; }

    SubscriptionThreadMode mode() const noexcept { return mode_; }

    const std::shared_ptr<const EventBusErrorHandler>& errorHandler() const noexcept { return error_handler_; }

  private:
    const std::weak_ptr<SubscriptionState>           state_;
    const std::shared_ptr<const Object>              event_;
    const SubscriptionThreadMode                     mode_;
    const std::shared_ptr<const EventBusErrorHandler> error_handler_;
};

/**
 * @brief Registry of the deliveries currently parked in the event loop.
 *
 * Deliberately detached from the bus: a queued task keeps the registry itself
 * alive and only ever touches the registry and its own payload, so it stays
 * valid after the bus is destroyed. Dropping the payloads (clear()) releases the
 * events they hold immediately, without waiting for the event loop.
 */
class DeliveryRegistry {
  public:
    void add(std::shared_ptr<Payload> payload)
    {
        std::lock_guard lock(mutex_);
        payloads_.push_back(std::move(payload));
    }

    void remove(const std::weak_ptr<Payload>& payload) noexcept
    {
        const auto locked = payload.lock();
        if (locked == nullptr) {
            return;  // already dropped by clear()
        }
        std::shared_ptr<Payload> removed;
        {
            std::lock_guard lock(mutex_);
            const auto      it = std::ranges::find(payloads_, locked);
            if (it != payloads_.end()) {
                removed = std::move(*it);
                payloads_.erase(it);
            }
        }
        // `removed` is destroyed here, outside the lock: releasing the payload can
        // destroy the event, i.e. run arbitrary user code.
    }

    void clear() noexcept
    {
        std::vector<std::shared_ptr<Payload>> dropped;
        {
            std::lock_guard lock(mutex_);
            dropped.swap(payloads_);
        }
        // Dropped outside the lock: releases every event held for a pending delivery.
    }

    std::size_t pendingCount() const
    {
        std::lock_guard lock(mutex_);
        return payloads_.size();
    }

  private:
    mutable std::mutex                    mutex_;
    std::vector<std::shared_ptr<Payload>> payloads_;
};

/**
 * @brief State of one queued task; lives exactly as long as the task in the event loop.
 *
 * Owns nothing from the bus beyond the shared registry, and unregisters its
 * payload when the task dies - whether it ran, was cancelled, or was dropped by
 * the event loop. Held behind a shared_ptr so that Qt's copies of the task share
 * one instance and therefore one unregistration.
 */
class Delivery {
  public:
    Delivery(std::shared_ptr<DeliveryRegistry> registry, std::shared_ptr<Payload> payload)
      : registry_(std::move(registry))
      , payload_(std::move(payload))
    {}

    ~Delivery() { registry_->remove(payload_); }

    Delivery(const Delivery&)            = delete;
    Delivery& operator=(const Delivery&) = delete;

    /// Delivers unless the subscription is inactive or the bus dropped the
    /// payload; never throws.
    void run() noexcept
    {
        const auto payload = payload_.lock();
        if (payload == nullptr) {
            return;  // dropped by shutdown()/~EventBus
        }
        const auto state = payload->lockState();
        if (state == nullptr || !state->isActive()) {
            return;  // subscription cancelled, or the bus was stopped
        }
        deliver(state, payload->event(), payload->errorHandler(), payload->mode(), /* deferred */ true);
    }

  private:
    const std::shared_ptr<DeliveryRegistry> registry_;
    const std::weak_ptr<Payload>            payload_;
};

/// Channel holding the live subscriptions for one event type.
class EventChannel {
  public:
    /// Registers a subscription and hands the reaped entries back to the caller,
    /// which destroys them once every lock has been released.
    std::shared_ptr<SubscriptionState> subscribe(SubscriptionState::Handler handler, SubscriptionThreadMode mode, String tag, std::vector<SubscriptionEntry>& garbage);

    /// Appends a snapshot of the live subscriptions; reaped entries are moved to
    /// garbage so that the caller destroys their closures outside every lock.
    void collect(std::vector<SubscriptionEntry>& out, std::vector<SubscriptionEntry>& garbage);

    /// Deactivates every subscription and moves the entries to garbage.
    void takeAll(std::vector<SubscriptionEntry>& garbage);

  private:
    void reapLocked(std::vector<SubscriptionEntry>& garbage);

    std::vector<SubscriptionEntry> handlers_;
    std::size_t                    last_id_ = 0;
    mutable std::mutex             mutex_;
};

std::shared_ptr<SubscriptionState> EventChannel::subscribe(SubscriptionState::Handler handler, SubscriptionThreadMode mode, String tag, std::vector<SubscriptionEntry>& garbage)
{
    std::lock_guard                    lock(mutex_);
    std::shared_ptr<SubscriptionState> state = std::make_shared<SubscriptionState>(++last_id_, std::move(handler), std::move(tag));
    reapLocked(garbage);
    handlers_.push_back(SubscriptionEntry{ mode, state });
    return state;
}

void EventChannel::collect(std::vector<SubscriptionEntry>& out, std::vector<SubscriptionEntry>& garbage)
{
    std::lock_guard lock(mutex_);
    reapLocked(garbage);
    out.insert(out.end(), handlers_.begin(), handlers_.end());
}

void EventChannel::takeAll(std::vector<SubscriptionEntry>& garbage)
{
    std::lock_guard lock(mutex_);
    for (const auto& entry : handlers_) {
        entry.state->deactivate();
    }
    // The copies in garbage keep every state - and therefore every handler
    // closure - alive, so clearing the channel runs no user code under the lock.
    garbage.insert(garbage.end(), handlers_.begin(), handlers_.end());
    handlers_.clear();
}

void EventChannel::reapLocked(std::vector<SubscriptionEntry>& garbage)
{
    // Cancelling a subscription only flips its state's atomic flag; the entry is
    // reaped here, and handed to the caller so that the handler closure is
    // destroyed outside the channel lock and outside the map lock.
    std::erase_if(handlers_, [&garbage](const SubscriptionEntry& entry) {
        if (entry.state->isActive()) {
            return false;
        }
        garbage.push_back(entry);
        return true;
    });
}

} // namespace

/// Token state: owns the subscription and cancels it when the handle dies.
struct Subscription::Control {
    explicit Control(std::shared_ptr<SubscriptionState> state) noexcept
      : state_(std::move(state))
    {}

    ~Control() { state_->deactivate(); }

    bool isActive() const noexcept { return state_->isActive(); }

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

/// Lifecycle of the bus: Running admits calls, Stopping refuses new ones while the
/// shutdown in progress drains, Stopped means no caller is inside any more.
enum class BusState
{
    Running,
    Stopping,
    Stopped,
};

struct EventBus::Impl {
    /// Admits one call (publish/subscribe) and releases it on the way out.
    ///
    /// The state and the caller count live under one mutex, so "the bus is
    /// stopping" and "a caller entered the bus" can never cross each other: once
    /// the state left Running no further call is admitted, and every call that was
    /// admitted stays visible to the waiter. That is what makes the completion of
    /// a graceful shutdown a proof that no thread is inside the bus any more.
    class CallGuard {
      public:
        explicit CallGuard(Impl& impl)
          : impl_(&impl)
        {
            if (impl_->stopped.load(std::memory_order_acquire)) {
                return;  // fast reject: a stopping bus never admits again
            }
            Impl::enterCall(impl_);  // may allocate: keep it outside the count
            std::lock_guard lock(impl_->call_mutex);
            if (impl_->state != BusState::Running) {
                Impl::leaveCall(impl_);
                return;
            }
            ++impl_->active_calls;
            admitted_ = true;
        }

        ~CallGuard()
        {
            if (!admitted_) {
                return;
            }
            Impl::leaveCall(impl_);
            std::lock_guard lock(impl_->call_mutex);
            --impl_->active_calls;
            impl_->call_cv.notify_all();
        }

        CallGuard(const CallGuard&)            = delete;
        CallGuard& operator=(const CallGuard&) = delete;

        /// false when the bus was already stopping or stopped: the call must do nothing.
        bool admitted() const noexcept { return admitted_; }

      private:
        Impl* const impl_;
        bool        admitted_ = false;
    };

    // One concrete EventChannel per subscribed event type.
    std::map<vine::TypeId, EventChannel> channels;
    // Shared lock: publish reads the map concurrently; subscribe inserts.
    mutable std::shared_mutex mutex;
    // Deliveries currently parked in the event loop. Shared with the queued tasks
    // so that they stay valid after the bus is destroyed.
    std::shared_ptr<DeliveryRegistry> deliveries{ std::make_shared<DeliveryRegistry>() };

    // Admission and shutdown protocol (see CallGuard).
    mutable std::mutex      call_mutex;
    std::condition_variable call_cv;
    int                     active_calls    = 0;
    BusState                state           = BusState::Running;
    bool                    shutdown_result = false;  // meaningful once state == Stopped
    // Thread performing the Stopping -> Stopped transition; guarded by call_mutex and
    // only meaningful while state == Stopping. A reentrant stop request from that same
    // thread (a handler-closure destructor running inside cancelSubscriptions) must not
    // wait for a stop it is itself holding up.
    std::thread::id stopper;
    // Mirror of state != Running: keeps isShutDown() and the admission fast path
    // lock-free. Written together with the state transition, under call_mutex.
    std::atomic<bool> stopped{ false };

    // Optional observation hook for subscriber exceptions. A delivery only pays a
    // single load unless a handler is installed, so the happy path stays as is.
    std::atomic<bool>                           has_error_handler{ false };
    mutable std::mutex                          error_mutex;
    std::shared_ptr<const EventBusErrorHandler> error_handler;  // guarded by error_mutex

    /// Installs (or clears) the error handler.
    void setErrorHandler(EventBusErrorHandler handler);
    /// Returns the installed handler, or nullptr; the snapshot stays valid even if
    /// the handler is replaced later.
    std::shared_ptr<const EventBusErrorHandler> currentErrorHandler() const;

    /// Admitted calls the current thread holds on this bus (0 when it holds none).
    ///
    /// Kept per bus: a thread nested inside another bus's call must not make this
    /// bus's waiter exclude calls it does not own, or the shutdown could report
    /// "nobody inside" while another thread is still in here.
    struct CallDepth {
        const Impl* impl;
        int         depth;
    };

    static std::vector<CallDepth>& callDepths() noexcept;
    /// Admitted calls the current thread holds on impl (0 when it holds none).
    static int  callDepth(const Impl* impl) noexcept;
    /// Records one admitted call of the current thread; may allocate.
    static void enterCall(const Impl* impl);
    /// Drops one admitted call of the current thread.
    static void leaveCall(const Impl* impl) noexcept;

    /// Waits until no thread other than the caller is inside an admitted call.
    bool waitForCallers(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline);

    /// Same, without a deadline: used by the destructor, which must not let Impl
    /// disappear under an admitted call.
    void waitForCallersForever(std::unique_lock<std::mutex>& lock);

    /// Snapshots the subscriptions that match event, most derived type first (see
    /// EventBus::publish()). Reaped handler closures are destroyed here, outside
    /// every lock.
    void collectPlan(const Object& event, std::vector<SubscriptionEntry>& plan);

    /// Starts the stop, if it has not started yet. The caller holds call_mutex.
    ///
    /// @return true when this call performed the Running -> Stopping transition and
    ///         is therefore the one that has to finish it.
    bool beginStop() noexcept;

    /// Completes the stop this caller performed and returns the result that every
    /// caller of shutdownGracefully() then sees. The caller holds call_mutex.
    ///
    /// @param clean true only when the caller waited for the admitted calls and ran
    ///              every parked delivery: a stop that cancels the remaining work
    ///              instead passes false, so that a later graceful caller is not told
    ///              that everything ran.
    bool finishStop(bool clean) noexcept;

    /// Reports whether this very thread performs the stop in progress. Such a caller
    /// must not wait for the Stopped transition, because it is the one holding it up.
    /// The caller holds call_mutex.
    bool stoppingOnThisThread() const noexcept;

    /// Waits for the stop in progress to complete. The caller holds call_mutex.
    void awaitStopped(std::unique_lock<std::mutex>& lock);

    /// Same, bounded by deadline. The caller holds call_mutex.
    bool awaitStoppedUntil(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline);
};

std::vector<EventBus::Impl::CallDepth>& EventBus::Impl::callDepths() noexcept
{
    // One entry per bus this thread is currently inside; entries disappear when
    // the depth returns to zero, so no stale key can outlive its bus.
    static thread_local std::vector<CallDepth> depths;
    return depths;
}

int EventBus::Impl::callDepth(const Impl* impl) noexcept
{
    const auto& depths = callDepths();
    const auto  entry  = std::ranges::find(depths, impl, &CallDepth::impl);
    return entry != depths.end() ? entry->depth : 0;
}

void EventBus::Impl::enterCall(const Impl* impl)
{
    auto&      depths = callDepths();
    const auto entry  = std::ranges::find(depths, impl, &CallDepth::impl);
    if (entry != depths.end()) {
        ++entry->depth;
        return;
    }
    depths.push_back(CallDepth{ impl, 1 });
}

void EventBus::Impl::leaveCall(const Impl* impl) noexcept
{
    auto&      depths = callDepths();
    const auto entry  = std::ranges::find(depths, impl, &CallDepth::impl);
    if (entry == depths.end()) {
        return;
    }
    if (--entry->depth == 0) {
        depths.erase(entry);
    }
}

bool EventBus::Impl::waitForCallers(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline)
{
    // Calls made by this thread on this bus cannot finish before this function
    // returns, so they are deliberately not waited for.
    const int self = callDepth(this);
    return call_cv.wait_until(lock, deadline, [this, self] { return active_calls <= self; });
}

void EventBus::Impl::waitForCallersForever(std::unique_lock<std::mutex>& lock)
{
    const int self = callDepth(this);
    call_cv.wait(lock, [this, self] { return active_calls <= self; });
}

void EventBus::Impl::collectPlan(const Object& event, std::vector<SubscriptionEntry>& plan)
{
    // One consistent plan per publish: the most derived class first, then the
    // interfaces it declares, then the base classes and their interfaces - all
    // snapshotted under the map lock, so subscribing or unsubscribing during a
    // dispatch only affects later publications. `visited` prunes the interface walk,
    // so a diamond hierarchy is walked once.
    std::vector<SubscriptionEntry> garbage;
    std::vector<vine::TypeId>      visited;
    {
        std::shared_lock lock(mutex);
        const auto       consider = [&](const vine::Type* type) {
            if (const auto it = channels.find(type); it != channels.end()) {
                it->second.collect(plan, garbage);
            }
        };
        for (const vine::Type* cls = event.getType(); cls != nullptr; cls = cls->parent()) {
            consider(cls);
            forEachInterface(cls->interfaces(), visited, consider);
        }
    }
    // Reaped handler closures are destroyed here, outside every lock, so their
    // destructors may call back into the bus.
    garbage.clear();
}

bool EventBus::Impl::beginStop() noexcept
{
    if (state != BusState::Running) {
        return false;
    }
    // The stopper is published together with the state, so no reader can observe
    // Stopping with the wrong thread id.
    stopper = std::this_thread::get_id();
    state   = BusState::Stopping;
    stopped.store(true, std::memory_order_release);
    return true;
}

bool EventBus::Impl::finishStop(bool clean) noexcept
{
    shutdown_result = clean;
    state           = BusState::Stopped;
    call_cv.notify_all();
    return shutdown_result;
}

bool EventBus::Impl::stoppingOnThisThread() const noexcept
{
    return state == BusState::Stopping && stopper == std::this_thread::get_id();
}

void EventBus::Impl::awaitStopped(std::unique_lock<std::mutex>& lock)
{
    call_cv.wait(lock, [this] { return state == BusState::Stopped; });
}

bool EventBus::Impl::awaitStoppedUntil(std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline)
{
    return call_cv.wait_until(lock, deadline, [this] { return state == BusState::Stopped; });
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
    // Refuse admission, cancel what is pending - so an in-flight call finishes
    // promptly - and only then let Impl go. The contract says the bus is destroyed
    // while nothing else calls it; this wait turns a violation into a short delay
    // instead of a dangling Impl (a handler must therefore never need the
    // destroying thread to make progress, and must not destroy the bus itself).
    std::unique_lock lock(d->call_mutex);
    if (d->beginStop()) {
        lock.unlock();
        cancelSubscriptions();
        lock.lock();
        d->finishStop(/* clean */ false);
    }
    else {
        // Somebody else is stopping: their cancel pass must finish before Impl goes.
        d->awaitStopped(lock);
    }
    d->waitForCallersForever(lock);
}

void EventBus::shutdown()
{
    std::unique_lock lock(d->call_mutex);
    if (d->beginStop()) {
        // Refuse admission from here on, then cancel what is left.
        lock.unlock();
        cancelSubscriptions();
        lock.lock();
        d->finishStop(/* clean */ false);
        return;
    }
    // Somebody else is stopping: wait for that stop to complete instead of letting
    // the caller destroy the bus while it is still draining. A reentrant stop request
    // from the stopping thread itself has nothing to wait for (see
    // stoppingOnThisThread()).
    if (d->stoppingOnThisThread()) {
        return;
    }
    d->awaitStopped(lock);
}

bool EventBus::shutdownGracefully(std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    std::unique_lock lock(d->call_mutex);
    if (!d->beginStop()) {
        // Another stop is already in progress (or has completed): wait for it and
        // report its outcome, so that every caller of a completed shutdown sees the
        // same result. A stop this very thread is running cannot finish before this
        // call returns, so it is not waited for.
        if (d->stoppingOnThisThread()) {
            return false;
        }
        return d->awaitStoppedUntil(lock, deadline) && d->shutdown_result;
    }

    // Refuse admission, then drain what the admitted callers left behind, and only
    // then cancel: a parked delivery still sees an active subscription and runs.
    const bool callers = d->waitForCallers(lock, deadline);
    lock.unlock();
    const bool deliveries = drainPendingDeliveries(deadline);
    cancelSubscriptions();

    lock.lock();
    return d->finishStop(callers && deliveries);
}

bool EventBus::drainPendingDeliveries(std::chrono::steady_clock::time_point deadline)
{
    if (d->deliveries->pendingCount() == 0) {
        return true;
    }
    if (dispatcher_ == nullptr) {
        return false;  // nothing can marshal them: they will never run here
    }
    while (d->deliveries->pendingCount() != 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        const auto before = d->deliveries->pendingCount();
        if (!dispatcher_->deliverPostedCalls()) {
            return false;  // only the application thread can pump the queue
        }
        if (d->deliveries->pendingCount() >= before) {
            // Nothing ran in this round: another thread may own the queue, so give
            // it a slot instead of spinning on it.
            std::this_thread::yield();
        }
    }
    return true;
}

void EventBus::cancelSubscriptions()
{
    // Cancel the subscriptions first, so a task that is being dequeued right now
    // sees an inactive subscription and skips its handler. The channels and the
    // map themselves stay alive (only ~EventBus destroys them), so a publish()
    // that already planned its deliveries cannot dangle: it either finds an empty
    // channel or delivers from the plan it took earlier.
    std::vector<SubscriptionEntry> garbage;
    {
        std::shared_lock lock(d->mutex);
        for (auto& entry : d->channels) {
            entry.second.takeAll(garbage);
        }
    }
    // Handler closures are destroyed outside every lock, so their destructors may
    // call back into the bus.
    garbage.clear();

    // Finally drop the parked deliveries: this releases the events they hold
    // without waiting for the event loop, and makes their tasks no-ops.
    d->deliveries->clear();
}

bool EventBus::isShutDown() const noexcept
{
    return d->stopped.load(std::memory_order_acquire);
}

std::size_t EventBus::pendingDeliveryCount() const
{
    return d->deliveries->pendingCount();
}

void EventBus::setErrorHandler(EventBusErrorHandler handler)
{
    d->setErrorHandler(std::move(handler));
}

Subscription EventBus::subscribeErased(vine::TypeId type, std::function<void(const std::shared_ptr<const Object>&)> handler, SubscriptionThreadMode mode, const String& tag)
{
    const Impl::CallGuard guard(*d);
    if (!guard.admitted()) {
        return Subscription();  // stopping or stopped: never register again
    }

    std::shared_ptr<SubscriptionState> state;
    std::vector<SubscriptionEntry>     garbage;
    {
        std::unique_lock lock(d->mutex);
        // Re-checked under the map lock: shutdown() clears the channels with the
        // same lock, so a registration racing with the stop is refused instead of
        // surviving it. Together with the admission above this makes "no valid
        // subscription exists once shutdown() has returned" hold.
        if (d->stopped.load(std::memory_order_acquire)) {
            return Subscription();
        }
        state = d->channels[type].subscribe(std::move(handler), mode, tag, garbage);
    }
    // Reaped handler closures are destroyed here, outside every lock, so their
    // destructors may call back into the bus.
    garbage.clear();

    try {
        // The state is passed by value, so it is still valid if the allocation of
        // the control block throws.
        return Subscription(std::make_shared<Subscription::Control>(state));
    }
    catch (...) {
        // Registration succeeded but no handle could be built: make the orphan
        // inert, so a subscription nobody can cancel is at least never invoked,
        // and let the failure surface.
        state->deactivate();
        throw;
    }
}

void EventBus::publish(const std::shared_ptr<const Object>& event)
{
    if (!event) {
        return;
    }
    const Impl::CallGuard guard(*d);
    if (!guard.admitted()) {
        return;  // stopping or stopped: no new work enters the bus
    }

    std::vector<SubscriptionEntry> plan;
    d->collectPlan(*event, plan);

    // Thread policy, resolved once per publish. Without a marshaller there is
    // nowhere to marshal to, so Main/Auto degrade to the publishing thread.
    const bool marshaller_ready = dispatcher_ != nullptr && dispatcher_->hasEventLoop();
    const bool on_main_thread   = !marshaller_ready || dispatcher_->isMainThread();
    // Error observation snapshot: one load when no handler is installed.
    const auto error_handler = d->has_error_handler.load(std::memory_order_acquire) ? d->currentErrorHandler() : nullptr;

    for (const auto& entry : plan) {
        if (!entry.state->isActive()) {
            continue;  // cancelled by unsubscribe()/shutdown() before its turn
        }
        const bool defer = entry.mode == SubscriptionThreadMode::Main || (entry.mode == SubscriptionThreadMode::Auto && !on_main_thread);
        if (!defer || !marshaller_ready) {
            deliver(entry.state, event, error_handler, entry.mode, /* deferred */ false);
            continue;
        }
        // Main, or Auto off the main thread: park the delivery in the event loop.
        // The task keeps only the detached registry and its own payload alive, so
        // it neither references nor keeps alive the bus.
        auto payload  = std::make_shared<Payload>(entry.state, event, entry.mode, error_handler);
        auto delivery = std::make_shared<Delivery>(d->deliveries, payload);
        d->deliveries->add(std::move(payload));
        if (!dispatcher_->postToMain([delivery] { delivery->run(); })) {
            // The task was dropped and is already gone with the lambda, so its
            // Delivery destructor unregistered the payload again: nothing leaks.
            V_LOGW("EventBus: dropping a Main delivery, the event loop refused the task");
        }
    }
}

V_APPFW_NS_END
