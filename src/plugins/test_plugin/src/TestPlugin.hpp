#pragma once

#include <vine/appfw/Plugin.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Test plugin that depends on the plugin_manager plugin.
 */
class TestPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    TestPlugin();

  public:
    /**
     * @brief Loads the plugin, unless the test hook asks it to fail.
     *
     * When the host configuration has the boolean key "plugins.test_plugin.fail_load"
     * set, the load throws instead of succeeding. The key is absent in a normal
     * run, and the plugin manager tests use it to exercise the rollback path of
     * PluginManager::loadAll() with a real plugin library.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the plugin is loaded.
     */
    vn::async::Task<void> load(vn::appfw::PluginLoadContext* context) override;

    /**
     * @brief Registers one command at run time, through the load context.
     *
     * A statically declared command (VN_DECLARE_COMMAND) is attributed by the manager's own scope; this one exists so
     * the attribution path a hook has to use (PluginLoadContext::registerCommand) is exercised against a real plugin
     * library. The name is distinct from test_hello on purpose: the module command is registered for the first, this
     * one for the second, and both must end up under the same plugin.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the plugin has contributed its command.
     */
    vn::async::Task<void> postLoad(vn::appfw::PluginLoadContext* context) override;
};

VN_APPFW_NS_END
