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
};

VN_APPFW_NS_END
