#pragma once

#include "appfw_global.hpp"

#include <functional>
#include <memory>
#include <utility>

#include <vine/Object.hpp>

V_APPFW_NS_BEGIN

class EventBus;

/**
 * @brief Thread on which a subscription's handler is delivered.
 *
 * Declared per subscription; different subscribers of the same event may use
 * different modes. Aligned with greenrobot ThreadMode / Qt ConnectionType.
 */
enum class SubscriptionThreadMode
{
    Current, ///< Call synchronously on the publishing thread (POSTING / DirectConnection).
    Main,    ///< Post to the main-thread queue (MAIN / QueuedConnection).
    Auto,    ///< Current when publishing on the main thread, else Main (AutoConnection).
};

/**
 * @brief RAII subscription token returned by EventBus::subscribe().
 *
 * Unsubscribes from the bus when destroyed, so a handler can never outlive its
 * subscriber. Move-only; copy is disabled to prevent double unsubscribe.
 * Default-constructed and moved-from tokens are inactive and are no-ops.
 */
class Subscription {
  public:
    Subscription() = default;

    /// Unsubscribes from the bus if the token is active.
    ~Subscription();

    Subscription(const Subscription&)            = delete;
    Subscription& operator=(const Subscription&) = delete;

    /// Moves ownership; the moved-from token becomes inactive.
    Subscription(Subscription&& other) noexcept;

    /// Unsubscribes the current token first, then takes ownership of other.
    Subscription& operator=(Subscription&& other) noexcept;

    /**
     * @brief Unsubscribes from the bus.
     *
     * Idempotent; safe to call from inside a handler during dispatch.
     */
    void unsubscribe();

    /// true while the token is still subscribed.
    bool isActive() const;

  private:
    friend class EventBus;
    explicit Subscription(std::function<void()> unsubscribe);

    std::function<void()> unsubscribe_;
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
 * - Events are keyed by their Type (TEvent::desc()); publish() walks the
 *   runtime hierarchy of the posted event and delivers most-derived first.
 * - Delivery follows each subscription's ThreadMode: Current runs synchronously
 *   on the publishing thread in subscription order; Main posts to the main
 *   thread; Auto chooses by the publishing thread. The bus is thread-safe for
 *   concurrent publish; subscribe/unsubscribe are also safe.
 * - Dispatch iterates a CopyOnWrite snapshot per channel, so subscribing or
 *   unsubscribing from inside a handler is safe and only affects the next
 *   delivery. Queued (Main) deliveries re-check the subscription before
 *   running, so unsubscribing cancels pending delivery.
 * - A throwing subscriber is caught and logged; remaining subscribers still
 *   run, so one bad handler cannot break a publish.
 * - Handlers are stored type-erased (shared_ptr<const Object>) in an internal
 *   channel (EventBus.cpp); subscribe<TEvent> restores the concrete type, so
 *   subscribers keep a strongly typed handler while the public API stays tiny.
 * - The bus must outlive every Subscription token and every pending Main
 *   delivery. The main-thread marshaller is reached internally via
 *   MainThreadDispatcher::current() (owned by Application); if none is set,
 *   Main/Auto degrade to synchronous delivery.
 */
class V_APPFW_API EventBus {
  public:
    EventBus();
    ~EventBus();

    /**
     * @brief Subscribes a handler for events of type TEvent (and derived types).
     *
     * @tparam TEvent Event type to receive; must derive from Object.
     * @param handler Called with each delivered TEvent.
     * @param mode Delivery thread for this subscription.
     * @return An RAII token; the subscription ends when it is destroyed.
     */
    template <ObjectBased TEvent>
    Subscription subscribe(std::function<void(const TEvent&)> handler, SubscriptionThreadMode mode = SubscriptionThreadMode::Current);

    /**
     * @brief Publishes an event to all matching subscribers.
     *
     * Dispatches by the runtime class of the posted event, so a Derived event
     * also reaches subscribers of its base types. No matching subscribers is a
     * no-op.
     *
     * @param event Event to broadcast; kept alive for the whole delivery.
     */
    void publish(const std::shared_ptr<const Object>& event);

  private:
    /// Type-erased registration used by subscribe<TEvent>.
    Subscription subscribeErased(vine::TypeId type, std::function<void(const std::shared_ptr<const Object>&)> handler, SubscriptionThreadMode mode);

    struct Impl;
    std::unique_ptr<Impl> d;
};

inline Subscription::~Subscription()
{
    unsubscribe();
}

inline Subscription::Subscription(Subscription&& other) noexcept
  : unsubscribe_(std::move(other.unsubscribe_))
{
    other.unsubscribe_ = {};
}

inline Subscription& Subscription::operator=(Subscription&& other) noexcept
{
    if (this != &other) {
        unsubscribe();
        unsubscribe_       = std::move(other.unsubscribe_);
        other.unsubscribe_ = {};
    }
    return *this;
}

inline void Subscription::unsubscribe()
{
    if (unsubscribe_) {
        unsubscribe_();
        unsubscribe_ = {};
    }
}

inline bool Subscription::isActive() const
{
    return static_cast<bool>(unsubscribe_);
}

inline Subscription::Subscription(std::function<void()> unsubscribe)
  : unsubscribe_(std::move(unsubscribe))
{}

template <ObjectBased TEvent>
Subscription EventBus::subscribe(std::function<void(const TEvent&)> handler, SubscriptionThreadMode mode)
{
    auto erased = [h = std::move(handler)](const std::shared_ptr<const Object>& event) { h(obj_cast<TEvent>(*event)); };
    return subscribeErased(TEvent::desc(), std::move(erased), mode);
}

V_APPFW_NS_END
