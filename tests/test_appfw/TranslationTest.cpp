// TranslationTest.cpp
//
// 界面语言只有一个入口：配置项 `general.language`。它有三个取值（跟随系统 / 中文 / English），但真正装到进程上的
// 只有两个标签——中文与英文；源串本身就是中文，所以中文不产生目录，缺译文即等于中文。用例钉的是这条链上不依赖
// "机器上恰好装了哪本 .qm"的部分：取值解析、条目的形状、以及无目录时 tr() 必须逐字返回源串。
//
// 目录本身的装载（内嵌资源 / 模块旁文件覆盖）要真跑 lrelease 才有意义，见 .ai/design/appfw-translation.md T5/T6。

#include <gtest/gtest.h>

#include <any>
#include <filesystem>
#include <fstream>
#include <memory>

#include <QCoreApplication>
#include <QStandardPaths>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/Translation.hpp>

#include <vine/String.hpp>

namespace
{

vn::appfw::AppConfig testConfig()
{
    vn::appfw::AppConfig config;
    config.name           = u8"test_appfw";
    config.persist_config = false; // 不碰开发机上的真实配置
    config.load_plugins   = false; // 语言与插件无关
    return config;
}

/// Writes a language setting in the framework's typed-marker JSON, the way ConfigManager::toJson() would.
void writeLanguageConfig(const std::filesystem::path& file, const char* value)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << R"({"general":{"language":{"type":"string","value":")" << value << R"("}}})";
}

} // namespace

TEST(TranslationTest, NoCatalogKeepsTheSourceText)
{
    // 一个目录都没装：translate() 必须逐字返回源串，不抛、不记日志。
    EXPECT_EQ(vn::appfw::translate("appfw", "关于"), vn::String(u8"关于"));
    EXPECT_EQ(vn::appfw::translate("model_viewer", "着色"), vn::String(u8"着色"));
}

TEST(TranslationTest, AnUnknownContextDoesNotChangeTheText)
{
    // context 只是查找的位置，不是文案的一部分：没有目录时它不改变返回值。
    EXPECT_EQ(vn::appfw::translate("", "着色"), vn::String(u8"着色"));
}

TEST(TranslationTest, ANullSourceIsEmptyRatherThanACrash)
{
    EXPECT_TRUE(vn::appfw::translate("appfw", nullptr).empty());
}

TEST(TranslationTest, LanguageTagsResolveToTheTwoSupportedLanguages)
{
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"en"), vn::String(u8"en"));
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"en_US"), vn::String(u8"en"));
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"EN"), vn::String(u8"en"));

    // 中文只有一本目录，所以 zh / zh_CN / zh_TW 都落到同一本。
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"zh_CN"), vn::String(u8"zh_CN"));
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"zh"), vn::String(u8"zh_CN"));
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"zh_TW"), vn::String(u8"zh_CN"));
}

TEST(TranslationTest, SystemAndUnknownSettingsFollowTheSystemLocale)
{
    const vn::String system = vn::appfw::resolveLanguageTag(vn::appfw::kLanguageSystem);

    // 系统语言不是英文时落回中文（源串），所以两个取值都必须在支持集合里。
    ASSERT_TRUE(system == u8"en" || system == u8"zh_CN");

    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8""), system);
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"system"), system);

    // 不支持的语言没有"猜一个"的选项：与跟随系统同解。
    EXPECT_EQ(vn::appfw::resolveLanguageTag(u8"de_DE"), system);
}

TEST(TranslationTest, TheLanguageItemDescribesAChoiceWithTheSystemDefault)
{
    const vn::appfw::ConfigItem item = vn::appfw::languageConfigItem();

    EXPECT_EQ(item.key(), vn::appfw::languageConfigKey());
    EXPECT_EQ(item.key(), vn::String(u8"general.language"));
    EXPECT_EQ(item.type(), vn::appfw::ConfigItemType::Choice);

    // 默认必须是"跟随系统"：没动过配置的进程保持今天的行为。
    ASSERT_TRUE(item.hasDefault());
    EXPECT_EQ(item.defaultString(), vn::String(vn::appfw::kLanguageSystem));

    // 三个选项的值就是 resolveLanguageTag() 认得的取值；显示文字随语言变，值不变。
    const auto& choices = item.choices();
    ASSERT_EQ(choices.size(), 3u);
    EXPECT_EQ(std::any_cast<vn::String>(choices.at(0).value), vn::String(vn::appfw::kLanguageSystem));
    EXPECT_EQ(std::any_cast<vn::String>(choices.at(1).value), vn::String(u8"zh_CN"));
    EXPECT_EQ(std::any_cast<vn::String>(choices.at(2).value), vn::String(u8"en"));
    EXPECT_FALSE(choices.at(2).description.empty());
}

TEST(TranslationTest, TheApplicationRegistersTheLanguageItem)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    auto app = std::make_unique<vn::appfw::Application>(testConfig(), 1, argv);
    ASSERT_NE(app, nullptr);

    // 条目由框架自己注册到"通用 / 语言"：宿主与插件都不必知道它的存在。
    const vn::appfw::ConfigItem* item = app->configRegistry()->item(vn::appfw::languageConfigKey());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->type(), vn::appfw::ConfigItemType::Choice);

    // 条目在注册表里，也就意味着它在配置窗口里可见、可改、可持久化。
    EXPECT_EQ(app->configManager()->getString(vn::appfw::languageConfigKey(), vn::String(u8"<unset>")),
              vn::String(u8"<unset>"));
}

TEST(TranslationTest, TheLanguageComesFromTheConfigurationFile)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_test.json";

    // 语言不是 AppConfig 的字段、也不是宿主的声明：它和别的设置一样在配置文件里（键 general.language，
    // 类型标记格式见 ConfigManager::toJson()）。观察到的不是“标签等于什么”，而是插件自己的文案到底被翻没翻：
    // en 会装上它的目录（只有 i18n_fixture_en.qm），zh_CN 不装（中文就是源串）。
    const auto probeUnder = [&](const char* language) {
        writeLanguageConfig(file, language);

        vn::appfw::AppConfig config = testConfig();
        config.config_file           = file;

        auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
        EXPECT_NE(app, nullptr);
        EXPECT_NE(app->pluginManager()->load(vn::String::fromUtf8(VN_I18N_PLUGIN)), nullptr);
        return app->configManager()->getString(u8"plugins.i18n.probe");
    };

    EXPECT_EQ(probeUnder("en"), vn::String(u8"Plugin text"));
    EXPECT_EQ(probeUnder("zh_CN"), vn::String(u8"插件文案"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TranslationTest, APluginCatalogComesFromThePluginItself)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_plugin_test.json";
    writeLanguageConfig(file, "en");

    vn::appfw::AppConfig config = testConfig();
    config.config_file           = file;

    auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 插件没有旁边的松散 .qm：目录嵌在它的 DLL 里（:/i18n/i18n_fixture_en.qm），加载器在 load() 之前装上，
    // 所以插件代码的第一行就已经是目标语言。
    const vn::String plugin = vn::String::fromUtf8(VN_I18N_PLUGIN);
    ASSERT_NE(app->pluginManager()->load(plugin), nullptr);
    EXPECT_EQ(app->configManager()->getString(u8"plugins.i18n.probe"), vn::String(u8"Plugin text"));

    // v1 不做运行期切换：事后改配置项不会重译已经装上的目录（下一次启动才生效）。
    app->configManager()->setString(vn::appfw::languageConfigKey(), u8"zh_CN");
    EXPECT_EQ(vn::appfw::translate("i18n_fixture", "插件文案"), vn::String(u8"Plugin text"));

    // 卸掉后它的目录不再留在进程里（R3）：同一个 context 只剩源串。
    ASSERT_TRUE(app->pluginManager()->unloadAll());
    EXPECT_EQ(vn::appfw::translate("i18n_fixture", "插件文案"), vn::String(u8"插件文案"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}
