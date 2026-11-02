// TranslationTest.cpp
//
// 界面语言只有一个入口：配置项 `general.language`。它有三个取值（跟随系统 / 中文 / English），但真正装到进程上的
// 只有两个标签——中文与英文；源串本身就是中文，所以中文不产生目录，缺译文即等于中文。用例钉的是这条链上不依赖
// "机器上恰好装了哪本 .qm"的部分：取值解析、条目的形状、以及无目录时 tr() 必须逐字返回源串。
//
// 目录本身的装载也在这一份里：夹具插件内嵌自己的 .qm，另有一份同名松散文件用来钉"模块旁的文件优先于内嵌"；两者都由
// 构建期 lrelease 产出，所以用例量的是查找顺序，不是"机器上恰好装了哪本 .qm"。

#include <gtest/gtest.h>

#include <any>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <QCoreApplication>
#include <QStandardPaths>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/ConfigStandard.hpp>
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

/// 一个只放"构造就抛"夹具的空目录：`loadAll()` 只会扫到它。
///
/// 每次换一个目录名：插件库是进程生命期的（DynamicLibraryLoader 故意不卸载它们），复用路径会让下一次拷贝撞上
/// "文件仍被映射"。
std::filesystem::path throwingPluginDirectory()
{
    static std::atomic<unsigned> dir_counter{ 0 };

    const std::filesystem::path fixture = VN_THROWING_PLUGIN;
    const std::filesystem::path dir     = std::filesystem::temp_directory_path() / ("vn_test_appfw_throwing_" + std::to_string(dir_counter.fetch_add(1)));

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
        std::filesystem::copy_file(fixture, dir / fixture.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    }
    EXPECT_FALSE(ec) << "夹具插件没拷进 '" << dir.string() << "': " << ec.message();
    return dir;
}

/// 一个放夹具插件副本 + 一份同名松散 `<context>_<locale>.qm` 的空目录：钉"模块旁的文件优先于模块内嵌"。
///
/// @param loose_qm Catalog to place beside the library, under the name the loader looks for.
/// @return The freshly created directory holding both files.
std::filesystem::path pluginDirectoryWithLooseCatalog(const std::filesystem::path& loose_qm)
{
    static std::atomic<unsigned> dir_counter{ 0 };

    const std::filesystem::path fixture = VN_I18N_PLUGIN;
    const std::filesystem::path dir     = std::filesystem::temp_directory_path() / ("vn_test_appfw_override_" + std::to_string(dir_counter.fetch_add(1)));

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
        std::filesystem::copy_file(fixture, dir / fixture.filename(), std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (!ec) {
        // 文件名必须是 `<context>_<locale>.qm`：加载器只按约定找这一个名字。
        std::filesystem::copy_file(loose_qm, dir / "i18n_fixture_zh_CN.qm", std::filesystem::copy_options::overwrite_existing, ec);
    }
    EXPECT_FALSE(ec) << "夹具没拷进 '" << dir.string() << "': " << ec.message();
    return dir;
}

} // namespace

TEST(TranslationTest, NoCatalogKeepsTheSourceText)
{
    // 一个目录都没装：translate() 必须逐字返回源串，不抛、不记日志。源串是英文。
    EXPECT_EQ(vn::appfw::translate("appfw", "About"), vn::String(u8"About"));
    EXPECT_EQ(vn::appfw::translate("model_viewer", "Shading"), vn::String(u8"Shading"));
}

TEST(TranslationTest, Char8LiteralsAreAccepted)
{
    EXPECT_EQ(vn::appfw::translate(u8"missing_context", u8"Untranslated text"), vn::String(u8"Untranslated text"));
    EXPECT_TRUE(vn::appfw::translate(u8"appfw", static_cast<const char8_t*>(nullptr)).empty());
}

TEST(TranslationTest, AnUnknownContextDoesNotChangeTheText)
{
    // context 只是查找的位置，不是文案的一部分：没有目录时它不改变返回值。
    EXPECT_EQ(vn::appfw::translate("", "Shading"), vn::String(u8"Shading"));
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

    // 系统语言不是中文时落回英文（源串），所以两个取值都必须在支持集合里。
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
    EXPECT_EQ(app->configManager()->getString(vn::appfw::languageConfigKey()).value(), vn::String(vn::appfw::kLanguageSystem));
}

TEST(TranslationTest, TheLanguageComesFromTheConfigurationFile)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_test.json";

    // 语言不是 AppConfig 的字段、也不是宿主的声明：它和别的设置一样在配置文件里（键 general.language，
    // 类型标记格式见 ConfigManager::toJson()）。观察到的不是“标签等于什么”，而是插件自己的文案到底被翻没翻：
    // zh_CN 会装上它的目录（只有 i18n_fixture_zh_CN.qm），en 不装（英文就是源串）。
    const auto probeUnder = [&](const char* language) {
        writeLanguageConfig(file, language);

        vn::appfw::AppConfig config = testConfig();
        config.config_file          = file;

        auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
        EXPECT_NE(app, nullptr);
        EXPECT_NE(app->pluginManager()->load(vn::String::fromUtf8(VN_I18N_PLUGIN)), nullptr);
        return app->configManager()->getString(u8"plugins.i18n.probe").value();
    };

    EXPECT_EQ(probeUnder("en"), vn::String(u8"Plugin text"));
    EXPECT_EQ(probeUnder("zh_CN"), vn::String(u8"插件文案"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TranslationTest, StandardConfigLabelsFollowTheInstalledLanguage)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_config_labels_test.json";

    // 标准分类/分组的标题是 appfw 自己的界面文案：节点在创建时就把当时的语言写进 label，所以中文目录先装、
    // 节点后建。名（name）与语言无关，任何语言下都是同一个身份串。
    const auto probeUnder = [&](const char* language) {
        writeLanguageConfig(file, language);

        vn::appfw::AppConfig config = testConfig();
        config.config_file          = file;

        auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
        EXPECT_NE(app, nullptr);

        const auto* category = app->configRegistry()->standardCategory(vn::appfw::StandardCategory::Logging);
        const auto* group    = app->configRegistry()->standardGroup(vn::appfw::StandardCategory::Logging, vn::appfw::StandardGroup::Console);
        EXPECT_NE(category, nullptr);
        EXPECT_NE(group, nullptr);
        return std::make_pair(category != nullptr ? category->label() : vn::String(), group != nullptr ? group->label() : vn::String());
    };

    const auto english = probeUnder("en");
    EXPECT_EQ(english.first, vn::String(u8"Logging"));
    EXPECT_EQ(english.second, vn::String(u8"Console"));

    const auto chinese = probeUnder("zh_CN");
    EXPECT_EQ(chinese.first, vn::String(u8"日志"));
    EXPECT_EQ(chinese.second, vn::String(u8"控制台"));

    EXPECT_EQ(vn::appfw::standardCategoryName(vn::appfw::StandardCategory::Logging), vn::String(u8"logging"));
    EXPECT_EQ(vn::appfw::standardGroupName(vn::appfw::StandardGroup::Console), vn::String(u8"console"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TranslationTest, APluginCatalogComesFromThePluginItself)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_plugin_test.json";
    writeLanguageConfig(file, "zh_CN");

    vn::appfw::AppConfig config = testConfig();
    config.config_file          = file;

    auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    // 插件没有旁边的松散 .qm：目录嵌在它的 DLL 里（:/i18n/i18n_fixture_zh_CN.qm），加载器在 load() 之前装上，
    // 所以插件代码的第一行就已经是目标语言。
    const vn::String plugin = vn::String::fromUtf8(VN_I18N_PLUGIN);
    ASSERT_NE(app->pluginManager()->load(plugin), nullptr);
    EXPECT_EQ(app->configManager()->getString(u8"plugins.i18n.probe").value(), vn::String(u8"插件文案"));

    // v1 不做运行期切换：事后改配置项不会重译已经装上的目录（下一次启动才生效）。
    app->configManager()->setString(vn::appfw::languageConfigKey(), u8"en");
    EXPECT_EQ(vn::appfw::translate("i18n_fixture", "Plugin text"), vn::String(u8"插件文案"));
    EXPECT_EQ(vn::appfw::translate(u8"i18n_fixture", u8"Plugin text"), vn::String(u8"插件文案"));

    // 卸掉后它的目录不再留在进程里（R3）：同一个 context 只剩源串。
    ASSERT_TRUE(app->pluginManager()->unloadAll());
    EXPECT_EQ(vn::appfw::translate("i18n_fixture", "Plugin text"), vn::String(u8"Plugin text"));
    EXPECT_EQ(vn::appfw::translate(u8"i18n_fixture", u8"Plugin text"), vn::String(u8"Plugin text"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TranslationTest, PluginsCannotClaimTheFrameworkTranslationContext)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    auto app = std::make_unique<vn::appfw::Application>(testConfig(), 1, argv);
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->pluginManager()->load(vn::String::fromUtf8(VN_RESERVED_CONTEXT_PLUGIN)), nullptr);
}

TEST(TranslationTest, AFailedBatchLoadLeavesNoCatalogInstalled)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_failed_batch_test.json";
    writeLanguageConfig(file, "zh_CN");

    vn::appfw::AppConfig config = testConfig();
    config.config_file          = file;

    // 插件目录里只有那个"构造就抛"的夹具：loadAll() 会停在 create()，然后把整批回滚。
    vn::appfw::PluginManager::setBuiltInPluginDirectory(throwingPluginDirectory());

    auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
    ASSERT_NE(app, nullptr);

    EXPECT_FALSE(app->pluginManager()->loadAll());

    // 目录在 create() 之前就装上了（那正是插件代码能看到自己语言的前提）；装载失败必须把它一起带走，
    // 否则一个从未运行过的模块的译文会留在进程里继续影响别的层（R3）。
    EXPECT_EQ(vn::appfw::translate("i18n_throwing", "Plugin text"), vn::String(u8"Plugin text"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(TranslationTest, ALooseFileOverridesTheEmbeddedCatalog)
{
    QStandardPaths::setTestModeEnabled(true);

    static char  arg0[] = "test_appfw";
    static char* argv[] = { arg0, nullptr };

    // 构建期产出的松散目录；没有 LinguistTools 时它不存在，本用例就跳过（P6 的同一条取舍）。
    const std::filesystem::path loose_qm = VN_I18N_OVERRIDE_QM;
    if (!std::filesystem::exists(loose_qm)) {
        GTEST_SKIP() << "lrelease was unavailable at build time, so the override catalog was not produced";
    }

    const auto file = std::filesystem::temp_directory_path() / "vine_translation_override_test.json";
    writeLanguageConfig(file, "zh_CN");

    vn::appfw::AppConfig config = testConfig();
    config.config_file          = file;

    // 插件副本 + 同名的 `i18n_fixture_zh_CN.qm`：查找顺序是先模块旁、后模块内嵌，所以应该读到松散文件里的译文。
    const std::filesystem::path dir     = pluginDirectoryWithLooseCatalog(loose_qm);
    const std::filesystem::path library = dir / std::filesystem::path(VN_I18N_PLUGIN).filename();

    auto app = std::make_unique<vn::appfw::Application>(config, 1, argv);
    ASSERT_NE(app, nullptr);
    ASSERT_NE(app->pluginManager()->load(vn::String::fromUtf8(library.string())), nullptr);

    // 内嵌的那本是 "Plugin text"；模块旁的文件赢了，所以插件读到的是覆盖后的译文。
    EXPECT_EQ(app->configManager()->getString(u8"plugins.i18n.probe").value(), vn::String(u8"Overridden text"));

    std::error_code ec;
    std::filesystem::remove(file, ec);
}
