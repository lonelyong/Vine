#pragma once

#include <vine/appfw/PluginManager.hpp>

#include <vine/appfw/gui/Window.hpp>

V_APPFWGUI_NS_BEGIN

/**
 * @brief Plugin manager dialog: left plugin list, right detail page.
 *
 * The left pane lists every discovered plugin (PluginManager::pluginEntries),
 * including the ones that are disabled or failed to load; selecting one shows
 * its detail on the right: static metadata (description, vendor, dependencies,
 * library path), its runtime state, plus the commands and config items it
 * registered (empty while the plugin is not loaded).
 *
 * The dialog offers four actions: "load" runs a plugin library immediately for a
 * trial (not persisted), "install" registers a plugin location so it is scanned
 * again after a restart (that is how a plugin living outside the application
 * directory joins the program), "disable/enable" persists the preference, and a
 * right-click menu additionally removes a registered location.
 *
 * The disable/enable action is offered only while it can take effect: a plugin
 * that ships with the application, and a plugin the host skips
 * (PluginManager::setSkipList), are not the user's to toggle, so the button and the
 * menu entry are hidden for them instead of being shown as dead controls.
 */
class V_APPFW_API PluginManagerDialog : public Window {
    V_OBJECT_META_DECL;

  public:
    explicit PluginManagerDialog(vine::appfw::PluginManager* manager);
    ~PluginManagerDialog() override;

  public:
    /// Rebuilds the plugin list from the manager.
    void refresh();

  private:
    /// Opens a file picker and loads the selected plugin library.
    void loadPlugin();

    /// Opens a file picker and registers the selected plugin location.
    ///
    /// @param scope Scope to register in: PluginScope::User needs no privileges,
    ///              PluginScope::AllUsers needs administrator rights.
    void installPlugin(vine::appfw::PluginScope scope);

    /// Removes the registered location the selected plugin was discovered from.
    void uninstallSelectedPlugin();

    /// Enables or disables the selected plugin and persists the preference.
    void togglePluginEnabled();

    /// Fills the right-hand detail page for the given plugin (empty hides it).
    void showDetail(const vine::String& name);

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
