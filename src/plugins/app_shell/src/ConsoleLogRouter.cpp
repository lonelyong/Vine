#include "ConsoleLogRouter.hpp"

#include <string>
#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigStandard.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

V_APPFW_NS_BEGIN

namespace
{

/**
 * @brief Process-wide toggle, initialized to enabled.
 */
std::shared_ptr<std::atomic<bool>>& consoleLogState()
{
    static std::shared_ptr<std::atomic<bool>> s_state = std::make_shared<std::atomic<bool>>(true);
    return s_state;
}

/// Sink installed by installConsoleLogSink().
///
/// The sink is intentionally leaked (heap-allocated, never freed): the default
/// logger owns a copy of it, and destroying a FunctionSink (which holds a user
/// callback capturing application objects) during static teardown crashes when
/// background logging threads race the logger destruction. Keeping it alive for
/// the process lifetime mirrors the previous plugin-member lifetime.
logging::LogSink*& installedConsoleSink()
{
    static logging::LogSink* s_sink = nullptr;
    return s_sink;
}

/// ConfigManager::changed handler id installed by installConsoleLogSink().
std::size_t& installedConfigHandlerId()
{
    static std::size_t s_id = 0;
    return s_id;
}

/**
 * @brief Maps a log level to the console's semantic message type.
 *
 * @param level Log level.
 * @return The corresponding console message type.
 */
gui::ConsoleMessageType toConsoleType(logging::LogLevel level)
{
    switch (level) {
    case logging::LogLevel::Warn:     return gui::ConsoleMessageType::Warning;
    case logging::LogLevel::Error:
    case logging::LogLevel::Critical: return gui::ConsoleMessageType::Error;
    default:                          return gui::ConsoleMessageType::Normal;
    }
}

} // namespace

std::shared_ptr<std::atomic<bool>> consoleLogEnabledState()
{
    return consoleLogState();
}

const String& consoleLogConfigKey()
{
    static const String s_key = String(u8"logging.console_enabled");
    return s_key;
}

void installConsoleLogSink(gui::ConsolePanel* panel, PluginLoadContext* context)
{
    if (panel == nullptr) {
        return;
    }

    // Register the routing toggle so it appears in the configuration window
    // (standard "logging" category -> "console" group).
    if (context != nullptr) {
        ConfigItem item(consoleLogConfigKey(), u8"日志输出到控制台", ConfigItemType::Bool);
        item.description(u8"开启后，默认日志输出到控制台面板").defaultValue(true);
        context->registerConfigItem(StandardCategory::Logging, StandardGroup::Console, item);
    }

    // The toggle's "source of truth" is the ConfigManager (the configuration
    // window edits it); the atomic is only the sink's lock-free cache. Initialize
    // it from config and keep it in sync on every change.
    auto* app = Application::current();
    if (app != nullptr && app->configManager() != nullptr) {
        auto*       cfg = app->configManager();
        const String key = consoleLogConfigKey();
        if (!cfg->contains(key)) {
            cfg->setBool(key, true);
        }
        consoleLogEnabledState()->store(cfg->getBool(key, true), std::memory_order_relaxed);
        installedConfigHandlerId() = cfg->changed.addHandler(
            [alive = consoleLogEnabledState(), key](ConfigManager& mgr, ConfigChangedEventArgs& args) {
                if (args.key() == key) {
                    alive->store(mgr.getBool(key, true), std::memory_order_relaxed);
                }
            });
    }

    // Records may arrive from any thread; the callback only touches the lock-free
    // toggle and marshals the actual UI update to the main thread.
    auto* sink = installedConsoleSink();
    if (sink == nullptr) {
        sink = new logging::LogSink(logging::LogSink::function(
            [panel, alive = consoleLogEnabledState()](logging::LogLevel level, const std::string& message) {
                if (!alive->load(std::memory_order_relaxed)) {
                    return;
                }
                String text(message.begin(), message.end());
                const auto type = toConsoleType(level);
                if (auto* app = Application::current(); app != nullptr && app->mainThreadDispatcher() != nullptr) {
                    auto* dispatcher = app->mainThreadDispatcher();
                    if (dispatcher->isMainThread() || !dispatcher->hasEventLoop()) {
                        // Main-thread record, or no event loop to marshal onto:
                        // append synchronously so it stays in order relative to
                        // command output (e.g. "Executing ..." appears before the
                        // command prints its result).
                        panel->append(type, text);
                    }
                    else {
                        dispatcher->postToMain(
                            [panel, type, text = std::move(text)]() mutable { panel->append(type, text); });
                    }
                }
            }));
        installedConsoleSink() = sink;
    }
    logging::defaultLogger().addSink(*sink);
}

void uninstallConsoleLogSink()
{
    if (auto* app = Application::current(); app != nullptr && app->configManager() != nullptr && installedConfigHandlerId() != 0) {
        app->configManager()->changed.removeHandler(installedConfigHandlerId());
        installedConfigHandlerId() = 0;
    }
    consoleLogEnabledState()->store(false, std::memory_order_relaxed);
}

V_APPFW_NS_END
