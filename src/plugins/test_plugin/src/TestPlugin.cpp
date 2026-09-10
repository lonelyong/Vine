#include "TestPlugin.hpp"

#include <stdexcept>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/plugin_export.hpp>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(TestPlugin, Plugin)

TestPlugin::TestPlugin() = default;

void TestPlugin::load(PluginLoadContext* context)
{
    // Test hook (see the header): a normal run never sets this key.
    if (context != nullptr) {
        if (raw_ptr<Application> app = context->application(); app != nullptr) {
            if (ConfigManager* config = app->configManager(); config != nullptr) {
                if (config->getBool(u8"plugins.test_plugin.fail_load", false)) {
                    throw std::runtime_error("test_plugin: plugins.test_plugin.fail_load is set");
                }
            }
        }
    }

    Plugin::load(context);
}

V_DECLARE_PLUGIN(TestPlugin, u8"8581747e-3f23-4da0-8fea-c0dfdb78e098", u8"test_plugin", u8"测试插件", u8"1.0.0", u8"测试插件：依赖应用外壳", u8"Vine",
                  u8"dev@vine.example", u8"https://github.com/vine/test_plugin", u8"", { u8"app_shell" })

V_APPFW_NS_END
