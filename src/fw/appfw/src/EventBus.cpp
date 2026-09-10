#include <vine/appfw/EventBus.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/logging/Log.hpp>

V_APPFW_NS_BEGIN

namespace
{

/// Channel holding the type-erased handlers for one event type.
class EventChannel {
  public:
    using Handler = std::function<void(const std::shared_ptr<const Object>&)>;

    std::size_t subscribe(Handler handler, SubscriptionThreadMode mode);
    bool        unsubscribe(std::size_t id);
    bool        isSubscribed(std::size_t id) const;
    void        publish(const std::shared_ptr<const Object>& event, MainThreadDispatcher* dispatcher);

  private:
    struct Entry {
        std::size_t            id;
        SubscriptionThreadMode mode;
        Handler                handler;
    };

    void deliver(std::size_t id, const Handler& handler, const std::shared_ptr<const Object>& event);

    std::vector<Entry> handlers_;
    std::size_t        last_id_ = 0;
    mutable std::mutex mutex_;
};

void reportHandlerError(std::size_t id, const std::exception* error)
{
    if (error != nullptr) {
        V_LOGE("EventBus: subscriber '{}' threw: {}", id, error->what());
    }
    else {
        V_LOGE("EventBus: subscriber '{}' threw a non-standard exception", id);
    }
}

std::size_t EventChannel::subscribe(Handler handler, SubscriptionThreadMode mode)
{
    std::lock_guard lock(mutex_);
    const auto      id = ++last_id_;
    handlers_.push_back(Entry{ id, mode, std::move(handler) });
    return id;
}

bool EventChannel::unsubscribe(std::size_t id)
{
    std::lock_guard lock(mutex_);
    const auto      it = std::ranges::find_if(handlers_, [id](const Entry& e) { return e.id == id; });
    if (it == handlers_.end()) {
        return false;
    }
    handlers_.erase(it);
    return true;
}

bool EventChannel::isSubscribed(std::size_t id) const
{
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(handlers_, [id](const Entry& e) { return e.id == id; });
}

void EventChannel::publish(const std::shared_ptr<const Object>& event, MainThreadDispatcher* dispatcher)
{
    // CopyOnWrite snapshot of an ordered handler list (CopyOnWriteArrayList
    // style): handlers added or removed during dispatch do not affect this
    // delivery, and iterating the copy can never be invalidated.
    std::vector<Entry> snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = handlers_;
    }
    const bool on_main = dispatcher != nullptr && dispatcher->isMainThread();
    for (const auto& entry : snapshot) {
        const bool deliver_now = entry.mode == SubscriptionThreadMode::Current || (entry.mode == SubscriptionThreadMode::Auto && on_main);
        if (deliver_now) {
            deliver(entry.id, entry.handler, event);
        }
        else if (dispatcher != nullptr) {
            // Main, or Auto on a non-main thread: queue to the main thread.
            dispatcher->postToMain([this, id = entry.id, handler = entry.handler, event] {
                if (isSubscribed(id)) {
                    deliver(id, handler, event);
                }
            });
        }
        else {
            // Main/Auto without a dispatcher: degrade to synchronous delivery.
            deliver(entry.id, entry.handler, event);
        }
    }
}

void EventChannel::deliver(std::size_t id, const Handler& handler, const std::shared_ptr<const Object>& event)
{
    try {
        handler(event);
    }
    catch (const std::exception& error) {
        reportHandlerError(id, &error);
    }
    catch (...) {
        reportHandlerError(id, nullptr);
    }
}

} // namespace

struct EventBus::Impl {
    // One concrete EventChannel per subscribed event type.
    std::map<vine::TypeId, EventChannel> channels;
    // Shared lock: publish reads the map concurrently; subscribe inserts.
    mutable std::shared_mutex mutex;
};

EventBus::EventBus()
  : d(new Impl)
{}

EventBus::~EventBus() = default;

Subscription EventBus::subscribeErased(vine::TypeId type, std::function<void(const std::shared_ptr<const Object>&)> handler, SubscriptionThreadMode mode)
{
    std::unique_lock lock(d->mutex);
    auto*            channel = &d->channels[type]; // default-constructs if missing
    const auto       id      = channel->subscribe(std::move(handler), mode);
    return Subscription([channel, id] { channel->unsubscribe(id); });
}

void EventBus::publish(const std::shared_ptr<const Object>& event)
{
    if (!event) {
        return;
    }
    // Collect the channels for the runtime class chain under the lock, then
    // dispatch without the lock (handlers may subscribe/publish). Map nodes
    // are stable, so the collected pointers stay valid. The main-thread
    // marshaller is the Application-owned one, if any.
    const auto* const           app        = Application::current();
    MainThreadDispatcher* const dispatcher = app != nullptr ? app->mainThreadDispatcher() : nullptr;
    std::vector<EventChannel*>  channels;
    {
        // Shared read lock: concurrent publishes only read the map and may run
        // in parallel; subscribe's exclusive lock blocks this only briefly.
        std::shared_lock lock(d->mutex);
        for (vine::TypeId cls = event->getType(); cls != nullptr; cls = cls->parent()) {
            auto it = d->channels.find(cls);
            if (it != d->channels.end()) {
                channels.push_back(&it->second);
            }
        }
    }
    for (auto* channel : channels) {
        channel->publish(event, dispatcher);
    }
}

V_APPFW_NS_END
