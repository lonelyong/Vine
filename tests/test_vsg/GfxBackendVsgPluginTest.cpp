#include <gtest/gtest.h>

#include <QStandardPaths>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/Plugin.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#include <memory>

namespace
{

/**
 * @brief Boots an Application the same way app/src/main.cpp does, but headless.
 *
 * Mirrors the real startup: build the application through the builder, then
 * load all plugins. PluginManager discovers gfx_backend_vsg from the default
 * plugin directory; GfxBackendVsgPlugin::load() registers the "vsg" backend
 * into RenderBackendRegistry.
 *
 * The Application is created exactly once per test suite: QCoreApplication is
 * a Qt global singleton, so a second Application in the same process would
 * collide when init() constructs a new QCoreApplication.
 */
std::unique_ptr<vine::appfw::Application> bootApplication()
{
    // Nothing this suite writes may reach the user's real data directory: the
    // registration round trip installs a plugin, and installPlugin() writes into
    // <data>/installed.d. Test mode resolves QStandardPaths to a per-user test root
    // (the same trick test_gui uses), and has to be set before the application is
    // built, because the paths are read while it is constructed.
    QStandardPaths::setTestModeEnabled(true);

    static char arg0[] = "test_vsg";
    static char* argv[] = { arg0, nullptr };

    vine::appfw::AppConfig config;
    config.name = "Vine";
    // Unit tests must not read or write the user's real configuration: the
    // builder would otherwise apply Application::defaultConfigFile().
    config.persist_config = false;
    auto app = vine::appfw::createApplication(config, 1, argv);
    EXPECT_NE(app, nullptr);
    if (app != nullptr) {
        // The tests below assert on the discovered entries, so a failure here shows
        // up as a missing entry rather than as a thrown assertion.
        static_cast<void>(app->pluginManager()->loadAll());
    }
    return app;
}

class VsgBackendPluginTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        s_app = bootApplication();
    }

    static std::unique_ptr<vine::appfw::Application> s_app;
};

std::unique_ptr<vine::appfw::Application> VsgBackendPluginTest::s_app;

}  // namespace

TEST_F(VsgBackendPluginTest, PluginRegistersVsgBackend)
{
    ASSERT_NE(s_app, nullptr);

    auto& registry = vine::graphics::RenderBackendRegistry::instance();
    EXPECT_TRUE(registry.has(u8"vsg"))
        << "gfx_backend_vsg plugin should have registered the 'vsg' backend";
}

TEST_F(VsgBackendPluginTest, CreateBackendByName)
{
    ASSERT_NE(s_app, nullptr);

    auto& registry = vine::graphics::RenderBackendRegistry::instance();

    // The plugin path must produce a VsgRenderer instance. initialize() is
    // intentionally not called here: it creates a real Vulkan window/device,
    // which is unsuitable for a headless unit test (would block waiting for a
    // GPU surface). Backend init/rendering is exercised by the real app.
    auto backend = registry.create(u8"vsg");
    ASSERT_NE(backend, nullptr);
}

/**
 * @brief Verifies the shutdown sequence that makes a disable take effect on the
 *        next start.
 *
 * This suite is the only one that owns an Application, so it is the only place
 * where run() (and therefore Application::shutdown()) can be exercised: the
 * config is written to the file, and reading it back yields the disabled plugin
 * list that the next start resolves against.
 *
 * Runs last in this suite: shutdown() unloads the plugins.
 */
TEST_F(VsgBackendPluginTest, ConfigFileRoundTripPersistsDisabledPlugins)
{
    ASSERT_NE(s_app, nullptr);

    const auto path = std::filesystem::temp_directory_path() / "vine_config_roundtrip_test.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // A plugin location outside the program directory, registered like an
    // installed plugin would be.
    const auto user_plugins = std::filesystem::temp_directory_path() / "vine_roundtrip_plugins";
    std::filesystem::remove_all(user_plugins, ec);
    ASSERT_TRUE(std::filesystem::create_directories(user_plugins, ec));

    // A per-user preference: app_shell/test_plugin are application-provided and
    // cannot be disabled, so the round trip records a plugin name of its own.
    EXPECT_TRUE(s_app->pluginManager()->setPluginEnabled(u8"user_plugin_x", false));
    EXPECT_FALSE(s_app->pluginManager()->installPlugin(vine::String(user_plugins.u8string())).empty());
    ASSERT_TRUE(s_app->setConfigFile(path));

    // Stop the main loop from another thread (this suite does not link Qt), so
    // run() returns and performs the shutdown sequence: plugins unload, the bus
    // drains, and the configuration is written.
    std::thread quitter([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto* app = vine::appfw::Application::current(); app != nullptr) {
            app->exit(0);
        }
    });
    EXPECT_EQ(s_app->run(), 0);
    quitter.join();
    EXPECT_TRUE(std::filesystem::exists(path)) << "shutdown() must persist the configuration";

    // The next start reads the file back: the disabled plugin is listed there.
    vine::appfw::ConfigManager reloaded;
    ASSERT_TRUE(reloaded.load(vine::String(path.u8string())));
    const auto disabled = reloaded.getStringArray(vine::appfw::PluginManager::disabledConfigKey());
    ASSERT_EQ(disabled.size(), 1u);
    EXPECT_TRUE(disabled[0] == u8"user_plugin_x");

    // The registered location survives too: that is what makes a plugin
    // installed outside the application directory available after a restart.
    const auto installed = s_app->pluginManager()->pluginRegistrations();
    ASSERT_EQ(installed.size(), 1u);
    EXPECT_TRUE(installed[0].path == vine::String(user_plugins.u8string()));
    EXPECT_EQ(installed[0].scope, vine::appfw::PluginScope::User);

    // The preference and the registration survive; the unload kept the metadata
    // (the plugin list is still readable after shutdown).
    EXPECT_FALSE(s_app->pluginManager()->isPluginEnabled(u8"user_plugin_x"));
    EXPECT_TRUE(s_app->pluginManager()->plugin(u8"app_shell") == nullptr);
    const auto entries = s_app->pluginManager()->pluginEntries();
    const auto entry   = std::find_if(entries.begin(), entries.end(),
        [](const vine::appfw::PluginEntry& e) { return e.info.name == u8"app_shell"; });
    ASSERT_NE(entry, entries.end());
    EXPECT_EQ(entry->scope, vine::appfw::PluginScope::BuiltIn);
    EXPECT_FALSE(entry->loaded);

    std::filesystem::remove(path, ec);
    std::filesystem::remove_all(user_plugins, ec);
}

/**
 * @brief The plugins the application ships cannot be disabled or uninstalled.
 *
 * What ships with the application is the application's decision (it can use
 * PluginManager::setSkipList() or simply not build the plugin); a user cannot
 * turn it off or remove it, because that would leave them with a broken program
 * and no way back.
 */
TEST_F(VsgBackendPluginTest, BuiltInPluginsCannotBeDisabledOrUninstalled)
{
    ASSERT_NE(s_app, nullptr);

    auto* pm = s_app->pluginManager();
    ASSERT_NE(pm, nullptr);

    const auto entries = pm->pluginEntries();
    const auto shell   = std::find_if(entries.begin(), entries.end(),
        [](const vine::appfw::PluginEntry& entry) { return entry.info.name == u8"app_shell"; });
    ASSERT_NE(shell, entries.end()) << "app_shell 来自程序目录";
    EXPECT_EQ(shell->scope, vine::appfw::PluginScope::BuiltIn);
    EXPECT_TRUE(shell->enabled);
    EXPECT_FALSE(shell->info.uuid.isNull()) << "插件身份由 V_DECLARE_PLUGIN 硬编码";

    EXPECT_FALSE(pm->setPluginEnabled(u8"app_shell", false)) << "自带插件不能被禁用";
    EXPECT_TRUE(pm->isPluginEnabled(u8"app_shell"));
    EXPECT_FALSE(pm->uninstallPlugin(u8"app_shell", vine::appfw::PluginScope::User));
    EXPECT_FALSE(pm->uninstallPlugin(u8"app_shell", vine::appfw::PluginScope::AllUsers));
    EXPECT_TRUE(pm->installPlugin(u8"/tmp", vine::appfw::PluginScope::BuiltIn).empty())
        << "a plugin location cannot be registered as application-provided";
}
