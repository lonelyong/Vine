// ConfigTest.cpp
//
// 需要重启才能生效的配置项：策略是描述层的事实（`ConfigItem::restartRequired()`），"现在是否有改动待生效"是
// 运行期的事实（`Application::pendingRestartKeys()`，以 initialize() 时的值快照为基准）。
// 用例钉的是这条分工的两半：策略只描述"怎么读"，状态只回答"现在还待不待重启"，两者都不能替对方说话。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

#include <QStandardPaths>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/Translation.hpp>

namespace
{

/// Writes a language setting in the framework's typed-marker JSON, the way ConfigManager::toJson() would.
void writeLanguageConfig(const std::filesystem::path& file, const char* value)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << R"({"general":{"language":{"type":"string","value":")" << value << R"("}}})";
}

} // namespace

TEST(ConfigTest, OnlyRestartRequiredItemsCanBePending)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false; // 不碰开发机上的真实配置：本次运行的配置从空开始
    config.load_plugins   = false;

    auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 策略在描述层：语言项声明了它，而空配置本身就等价于"跟随系统"，所以启动时没有待生效的改动。
    const vn::appfw::ConfigItem* item = app->configRegistry()->item(vn::appfw::languageConfigKey());
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->restartRequired());
    EXPECT_TRUE(app->pendingRestartKeys().empty());

    // 改一个需要重启的项：只有它的 key 待生效。
    app->configManager()->setString(vn::appfw::languageConfigKey(), u8"en");
    const std::vector<vn::String> pending = app->pendingRestartKeys();
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front(), vn::appfw::languageConfigKey());

    // 改回启动值（key 本来就不存在）：状态回到"不需要重启"——它描述现在，不记录历史。
    app->configManager()->remove(vn::appfw::languageConfigKey());
    EXPECT_TRUE(app->pendingRestartKeys().empty());

    // 不需要重启的项永远进不了这个列表，无论它自身值是什么。
    app->configManager()->setString(u8"plugins.disabled", u8"whatever");
    EXPECT_TRUE(app->pendingRestartKeys().empty());
}

TEST(ConfigTest, TheBaselineIsThisRunNotTheProcess)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_config_restart_test.json";
    writeLanguageConfig(file, "en");

    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false;
    config.load_plugins   = false;
    config.config_file    = file;

    {
        auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
        ASSERT_NE(app, nullptr);

        // 启动时读到的是 en，所以此刻没有待生效的改动；改成 zh_CN 才产生一个。
        EXPECT_TRUE(app->pendingRestartKeys().empty());
        app->configManager()->setString(vn::appfw::languageConfigKey(), u8"zh_CN");
        EXPECT_EQ(app->pendingRestartKeys().size(), 1u);
        // 不保存：文件里仍然是 en。
    }

    {
        // 同一个进程里的下一次启动：基准来自它自己读到的那份文件，所以前一次运行的改动不残留。
        auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
        ASSERT_NE(app, nullptr);
        EXPECT_TRUE(app->pendingRestartKeys().empty());
    }

    std::error_code ec;
    std::filesystem::remove(file, ec);
}
