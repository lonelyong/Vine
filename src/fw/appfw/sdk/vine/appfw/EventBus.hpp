#pragma once

#include "appfw_global.hpp"

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

#include <vine/Object.hpp>

V_APPFW_NS_BEGIN

class EventBus;
class MainThreadDispatcher;

/**
 * @brief Thread on which a subscription's handler is delivered.
 *
 * Declared per subscription; different subscribers of the same event may use
 * different modes. Aligned with greenrobot ThreadMode / Qt ConnectionType.
 */
enum class SubscriptionThreadMode
{
    Current, ///< Call synchronously on the publishing thread (POSTING / DirectConnection).
    Main,    ///< Post to the main event loop so it runs on the main thread (MAIN / QueuedConnection).
    Auto,    ///< Current when publishing on the main thread, else Main (AutoConnection).
};

/**
 * @brief Description of a subscriber that threw while an event was delivered.
 *
 * Handed to the handler installed with EventBus::setErrorHandler(), so that an
 * application can route the failure to crash reporting, metrics or test
 * assertions instead of losing it in a log line.
 */
struct EventBusError {
    const vine::Type*      event_type    = nullptr;                          ///< Runtime type of the event being delivered.
    std::size_t            subscriber_id = 0;                                ///< Id of the subscription that threw; unique per event type.
    String                 tag;                                              ///< Label the subscriber passed to subscribe(), empty when it passed none.
    std::exception_ptr     error;                                            ///< Rethrowable exception; null for non-standard ones.
    SubscriptionThreadMode mode          = SubscriptionThreadMode::Current;  ///< Thread mode of that subscription.
    bool                   deferred      = false;                            ///< true when it ran from the event loop, on the main thread.
};

/**
 * @brief Handler called when a subscriber throws.
 *
 * Must not throw: the bus invokes it from the delivery path, outside every lock,
 * and discards anything a misbehaving handler does throw.
 */
using EventBusErrorHandler = std::function<void(const EventBusError&)>;

/**
 * @brief RAII subscription handle returned by EventBus::subscribe().
 *
 * Cancels the subscription when the last handle is destroyed, so a handler is
 * never invoked after the subscriber dies (an invocation already running is not
 * interrupted). The handle references only the subscription's own state, never
 * the bus: one that outlives the bus becomes inert instead of dangling, and
 * isActive() reports false once the bus is shut down or destroyed.
 * Move-only; copy is disabled to prevent double cancellation, and a moved-from
 * handle is inactive and a no-op.
 */
class V_APPFW_API Subscription {
  public:
    Subscription() noexcept = default;

    /// Cancels the subscription.
    ~Subscription();

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// Transfers the subscription; the moved-from handle becomes inactive.
    Subscription(Subscription&& other) noexcept;

    /// Cancels the current subscription first, then takes over the other handle.
    Subscription& operator=(Subscription&& other) noexcept;

    /**
     * @brief Cancels the subscription.
     *
     * Idempotent, never blocks, never waits for a delivery in progress and is
     * safe to call from inside a handler: after it returns, no further handler
     * invocation for this subscription starts (one already running completes).
     */
    void unsubscribe() noexcept;

    /**
     * @brief Reports whether the subscription can still receive events.
     *
     * @return true while the subscription is registered and the bus is running,
     *         false after unsubscribe(), shutdown() or bus destruction.
     */
    bool isActive() const noexcept;

  private:
    friend class EventBus;

    struct Control;

    explicit Subscription(std::shared_ptr<Control> control) noexcept;

    std::shared_ptr<Control> control_;
};

/**
 * @brief In-process, type-driven publish/subscribe bus with polymorphic events.
 *
 * Complements member signals (Event<TSender, TEventArgs>): a publisher posts an
 * event and every subscriber registered for that type or any base type receives
 * it. Events must derive from Object; the bus dispatches along the runtime
 * class hierarchy (Type::parent()), so subscribing to a base type also
 * receives derived events (greenrobot-style polymorphism).
 *
 * Semantics:
 * - Events are keyed by their Type (TEvent::desc()); publish() delivers to a
 *   subscription exactly when obj_cast<TEvent>(event) would succeed, i.e. when
 *   the event is of that type, derives from it, or implements it as an
 *   interface. Delivery order is most-derived class first, then the interfaces
 *   that class declares (transitively, in declaration order), then the base
 *   classes and their interfaces; within one type the subscriptions run in
 *   subscription order, and every type is delivered at most once. The interface
 *   order therefore follows the order the type metadata declares the interfaces.
 * - Delivery follows each subscription's ThreadMode: Current runs synchronously
 *   on the publishing thread; Main parks the delivery in the event loop so it
 *   runs later on the main thread; Auto chooses by the publishing thread.
 * - publish() builds one consistent plan per call: every matching subscription is
 *   snapshotted up front, so subscribing or unsubscribing during a dispatch never
 *   affects that dispatch, only later ones.
 * - Each invocation is additionally guarded by its subscription's active flag,
 *   checked immediately before the call. That check is the cancellation point:
 *   an invocation that passed it counts as started and is allowed to finish, an
 *   invocation that has not reached it is skipped. So unsubscribe(), destroying a
 *   handle, shutdown() and destroying the bus cancel every invocation that has not
 *   started, and none of them ever waits for one that has.
 * - A queued (Main) delivery keeps its event and its subscription state alive
 *   through a detached registry that references neither the bus nor the channel.
 *   It therefore stays safe after the bus is destroyed. shutdown() and destroying
 *   the bus cancel the deliveries that have not started by dropping them from that
 *   registry: the already-posted callback stays in the event loop but becomes an
 *   inert no-op.
 * - A throwing subscriber is caught and logged; the remaining subscribers still
 *   run, so one bad handler cannot break a publish. When a handler is installed
 *   with setErrorHandler() the failure is also reported with its context - event
 *   type, subscription id and the subscription's tag - for deferred deliveries
 *   too. Neither the log call nor the handler is allowed to escape: the
 *   observation path never becomes a new failure source.
 * - Handlers are stored type-erased (shared_ptr<const Object>) in an internal
 *   channel (EventBus.cpp); subscribe<TEvent> restores the concrete type, so
 *   subscribers keep a strongly typed handler while the public API stays tiny.
 * - The bus never keeps itself alive through a handle or a pending delivery, and
 *   it does not look the Application up globally: the marshaller is injected at
 *   construction and must outlive the bus. Without a marshaller - or without a
 *   QCoreApplication to queue onto - Main/Auto run on the publishing thread.
 * - Admission: publish() and subscribe() first admit themselves into the bus.
 *   Once the bus has started stopping, admission is refused, so they become a
 *   no-op (publish) or return an inert handle (subscribe). Admission and the
 *   caller count share one mutex, so a call can never slip into the bus behind a
 *   shutdown that already observed an empty bus.
 * - Thread safety: publish/subscribe/shutdown may be called from any thread at any
 *   time, and one Subscription handle belongs to one thread. Destroying the bus
 *   while another thread is inside a member is a contract violation, but the bus
 *   turns it into a wait instead of a dangling object: shutdownGracefully()
 *   returning true proves that nobody is inside any more, and ~EventBus() waits
 *   for the calls that were already admitted.
 */
class V_APPFW_API EventBus {
  public:
    /**
     * @brief Creates a bus that marshals Main/Auto deliveries with dispatcher.
     *
     * @param dispatcher Main-thread marshaller, or nullptr for a bus that always
     *                   delivers on the publishing thread; must outlive the bus.
     */
    explicit EventBus(MainThreadDispatcher* dispatcher = nullptr);

    /**
     * @brief Stops the bus and waits for the calls that are still inside.
     *
     * Cancels the subscriptions and drops the pending deliveries first, so that an
     * admitted call finishes promptly, then waits for those calls to leave before
     * the implementation is released. The bus must not be destroyed from inside a
     * handler, and no handler may need the destroying thread to make progress.
     */
    ~EventBus();

    /**
     * @brief Subscribes a handler for events of type TEvent (and derived types).
     *
     * @tparam TEvent Event type to receive; must be described by the runtime type
     *                system, i.e. an Object-derived class or an interface declared
     *                with V_DECLARE_INTERFACE. A class type matches the event and
     *                its derived types, an interface type matches every event that
     *                implements it.
     * @param handler Called with each delivered TEvent.
     * @param mode Delivery thread for this subscription.
     * @param tag Free-form label reported with a failure through
     *            EventBusError::tag, e.g. the module or window that subscribes.
     *            Copied into the subscription; may be empty.
     * @return A handle that cancels the subscription when it is destroyed; an
     *         inert one when the bus is already stopping.
     * @throws std::bad_alloc if the handle cannot be allocated; the registration
     *         is then cancelled as well, so no subscription is left behind that
     *         nobody could unsubscribe.
     */
    template <TypeDescribed TEvent>
    Subscription subscribe(std::function<void(const TEvent&)> handler,
                           SubscriptionThreadMode             mode = SubscriptionThreadMode::Current,
                           const String&                      tag  = {});

    /**
     * @brief Publishes an event to all matching subscribers.
     *
     * Dispatches by the runtime class of the posted event, so a Derived event
     * also reaches subscribers of its base types. No matching subscribers, a null
     * value, or a bus that has started stopping all make this a no-op.
     *
     * @param event Event to broadcast; kept alive for the whole delivery, or an
     *              empty pointer to publish nothing.
     * @throws std::bad_alloc if the delivery plan cannot be built; nothing is
     *         delivered in that case.
     */
    void publish(const std::shared_ptr<const Object>& event);

    /**
     * @brief Stops the bus and cancels every subscription.
     *
     * Call it once the application's main loop has stopped and before the
     * subscribers (windows, plugins) are torn down: afterwards publish() is a
     * no-op, subscribe() returns an inert token, and every queued Main delivery
     * that has not started is dropped. Pending deliveries are discarded rather
     * than drained.
     *
     * A Subscription token or a queued delivery that outlives the bus is inert
     * with or without this call; shutdown() only makes the point where delivery
     * stops explicit, and releases the retained handlers and events instead of
     * waiting for the bus itself to be destroyed.
     *
     * When another thread is already stopping the bus, this call waits for that
     * stop to complete, so that the caller never destroys the bus while the other
     * thread is still inside it. It does not wait for the calls that are already
     * inside the bus: use shutdownGracefully() to drain them, or destroy the bus,
     * which waits for them itself.
     */
    void shutdown();

    /**
     * @brief Stops the bus, first delivering what is already parked.
     *
     * Use it in a shutdown sequence when events published just before the loop
     * stopped should still arrive. While it runs the bus admits no new call, then
     * it waits up to timeout for the admitted calls to leave, then it runs the
     * parked deliveries (only possible when called on the application thread) and
     * finally cancels whatever is left, exactly as shutdown() does.
     *
     * The drain dispatches queued callbacks, which may run handlers and therefore
     * cause reentrant EventBus activity; those calls find a stopping bus, so
     * publish() is a no-op and subscribe() returns an inert handle. Calling it from
     * inside a handler does not wait for that handler's own call.
     *
     * Completion: when it returns, the bus is permanently stopped and - if the
     * result is true - every admitted call has left, so the bus may be destroyed.
     * Concurrent callers wait for the same completion and all report the same
     * result. A false result means the drain could not finish before the deadline,
     * the caller is not the application thread, or another thread stopped the bus
     * with shutdown()/~EventBus() and so cancelled the remaining work: that stop
     * never drained, and reporting true for it would claim work ran that never
     * did. The bus is stopped either way, and the remaining subscriptions and
     * deliveries are cancelled.
     *
     * @param timeout Upper bound for the whole wait; 0 means do not wait.
     * @return true if every admitted call finished and every parked delivery ran
     *         before the timeout, false if some work had to be dropped.
     */
    bool shutdownGracefully(std::chrono::milliseconds timeout = gracefulShutdownTimeout());

    /**
     * @brief Reports whether the bus has started stopping.
     *
     * @return true from the moment the stop begins, i.e. already while a graceful
     *         stop is still draining: admission is refused from that point on.
     */
    bool isShutDown() const noexcept;

    /**
     * @brief Installs the handler called when a subscriber throws.
     *
     * Optional observation hook: without it a throwing subscriber is only logged.
     * The handler runs on the delivering thread - the publishing thread for
     * Current, the application thread for a deferred delivery - outside every
     * lock, and a handler that throws is ignored. A deferred delivery reports to
     * the handler that was installed when it was posted. Installing a handler is
     * independent of the bus lifecycle: it is honoured until it is replaced or
     * removed, and installations are safe from any thread.
     *
     * @param handler Handler to install, or an empty one to remove the current one.
     */
    void setErrorHandler(EventBusErrorHandler handler);

    /**
     * @brief Returns how many deliveries are parked in the event loop.
     *
     * Counts Main-mode (and off-main Auto) deliveries that were handed to the
     * event loop and have not run yet; shutdown() and destroying the bus drop
     * them, and a delivery that ran is removed when its task is destroyed.
     *
     * @return The number of pending deliveries.
     */
    std::size_t pendingDeliveryCount() const;

    /**
     * @brief Returns the default upper bound for the graceful shutdown at the end of run().
     *
     * @return The default graceful-shutdown timeout.
     */
    static constexpr std::chrono::milliseconds gracefulShutdownTimeout() noexcept { return std::chrono::milliseconds{ 200 }; }

  private:
    /// Type-erased registration used by subscribe<TEvent>.
    Subscription subscribeErased(vine::TypeId type, std::function<void(const std::shared_ptr<const Object>&)> handler, SubscriptionThreadMode mode, const String& tag);

    /// Cancels every subscription and drops the deliveries that are still parked.
    void cancelSubscriptions();

    /// Runs the parked deliveries until the deadline; false when they could not run.
    bool drainPendingDeliveries(std::chrono::steady_clock::time_point deadline);

    struct Impl;

    std::unique_ptr<Impl>       d;
    MainThreadDispatcher* const dispatcher_;
};

template <TypeDescribed TEvent>
Subscription EventBus::subscribe(std::function<void(const TEvent&)> handler, SubscriptionThreadMode mode, const String& tag)
{
    auto erased = [h = std::move(handler)](const std::shared_ptr<const Object>& event) { h(obj_cast<TEvent>(*event)); };
    return subscribeErased(TEvent::desc(), std::move(erased), mode, tag);
}

V_APPFW_NS_END
