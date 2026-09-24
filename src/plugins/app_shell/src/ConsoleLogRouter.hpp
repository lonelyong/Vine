#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <atomic>
#include <memory>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

class PluginLoadContext;

namespace gui {
class ConsolePanel;
}

/**
 * @brief Shared state controlling whether default-logger records are routed
 * to the console panel.
 *
 * Owned by AppShellPlugin, which creates the sink, and shared with the toggle
 * command. The flag is an std::atomic so the sink callback checks it lock-free
 * from any thread.
 */
std::shared_ptr<std::atomic<bool>> consoleLogEnabledState();

/**
 * @brief Config key of the console-log routing toggle.
 *
 * @return The dotted ConfigManager path of the bool value.
 */
const String& consoleLogConfigKey();

/**
 * @brief Installs the console-log feature: config item, config sync and sink.
 *
 * Registers the routing-toggle config item (via the load context), initializes
 * and keeps the enabled flag in sync with the ConfigManager, and installs a
 * sink forwarding default-logger records to the console panel. Records may
 * arrive from any thread; the sink marshals UI updates to the main thread. The
 * sink is registered on the default logger for the process lifetime (the
 * logger owns it), so this is called once at plugin load.
 *
 * @param panel Console panel receiving the records; kept non-owning.
 * @param context Load context used to register the config item; may be null.
 */
void installConsoleLogSink(gui::ConsolePanel* panel, PluginLoadContext* context);

/**
 * @brief Undoes the config sync installed by installConsoleLogSink() and turns
 * the routing toggle off. The sink itself stays on the default logger.
 */
void uninstallConsoleLogSink();

VN_APPFW_NS_END
