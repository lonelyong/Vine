# appfw EventBus backlog

> Open items only; without a trigger condition it is not done. Closed items are deleted.

## 1. Actionable Now (No Trigger)

(none)

## 2. Trigger-gated

| # | Item | What | Trigger | Source |
|---|---|---|---|---|
| T1 | O(N²) drain on a large pending set | `DeliveryRegistry::remove` scans the pending vector, so draining N parked deliveries - one per `deliverPostedCalls()` round - costs O(N²) | the event loop stalls with tens of thousands of Main deliveries parked at once, or a profile shows the drain dominating a shutdown | `../design/appfw-eventbus.md` §9.2 |
| T2 | `SubscriptionThreadMode` states no underlying type | the enum appears in the public `EventBusError` value, so its width is part of that struct's layout; declaring it `: std::uint8_t` is an ABI change | the next `VN_APPFW_PLUGIN_ABI_VERSION` batch, i.e. a plugin ABI break already in progress | `../design/appfw-eventbus.md` §9.2 |
