#pragma once

#include <vine/appfw/Plugin.hpp>

#include "AppShellUi.hpp"

VN_APPFW_NS_BEGIN

/**
 * @brief App shell plugin: provides base GUI functionality (plugin info
 * viewer, configuration window) and adds two Ribbon buttons on the main
 * window to trigger them.
 *
 * The plugin is a thin orchestrator: the Ribbon/workspace UI is built by
 * AppShellUi, the console-log routing (config item, config sync, sink) is
 * owned by ConsoleLogRouter.
 *
 * It owns the SHELL and nothing that is drawn inside it: a plugin that contributes content to the central 3D view does
 * that from its own load() (the view is published by buildAppShellDock()), and the render session comes up in
 * postLoad(), once every plugin has loaded.
 */
class AppShellPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    AppShellPlugin();

  public:
    /**
     * @brief Builds the shell's interface: the Ribbon, the workspace docks and the console binding.
     *
     * Three stretches with a turn of the event loop in between (see the implementation). Every one of them is
     * application-thread work - widgets and graphics objects - so the boot staying responsive comes from the plugin
     * handing the thread back, not from the work moving off it.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the shell's interface is up.
     */
    vn::async::Task<void> load(PluginLoadContext* context) override;

    /**
     * @brief Brings the shell's render session up - after every plugin has loaded.
     *
     * WHY HERE: the pipelines are built from the passes the content plugins register, so the session cannot come up
     * before they have all had their load(). postLoad() is the first moment that is true (the manager runs it once the
     * whole load pass is done) and it is still adjacent to the main window going up, which the attach needs.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the session is attached (or was reported as not attachable yet).
     */
    vn::async::Task<void> postLoad(PluginLoadContext* context) override;

    void unload(PluginLoadContext* context) override;

  private:
    /// Dock layout built by load() and brought up by postLoad() (owned by the window's dock manager).
    AppShellDock dock_;
};

VN_APPFW_NS_END
