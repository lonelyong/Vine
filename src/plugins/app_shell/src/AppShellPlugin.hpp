#pragma once

#include <vine/appfw/Plugin.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief App shell plugin: provides base GUI functionality (plugin info
 * viewer, configuration window) and adds two Ribbon buttons on the main
 * window to trigger them.
 *
 * The plugin is a thin orchestrator: the Ribbon/workspace UI is built by
 * AppShellUi, the console-log routing (config item, config sync, sink) is
 * owned by ConsoleLogRouter.
 */
class AppShellPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    AppShellPlugin();

  public:
    /**
     * @brief Builds the shell's interface: the Ribbon, the workspace docks and the console binding.
     *
     * Four stretches with a turn of the event loop in between (see the implementation). Every one of them is
     * application-thread work - widgets, graphics objects, a render session - so the boot staying responsive comes from
     * the plugin handing the thread back, not from the work moving off it. The demo's content (its 1.7 s of reading and
     * decoding) is not in here at all: that half is pure data and goes to the pool (see AppShellDemo).
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the shell's interface is up.
     */
    vn::async::Task<void> load(PluginLoadContext* context) override;
    void                  unload(PluginLoadContext* context) override;
};

VN_APPFW_NS_END
