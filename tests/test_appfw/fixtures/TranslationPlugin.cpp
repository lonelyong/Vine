// 测试夹具：一个带自己翻译目录的插件（目录内嵌在它的 DLL 里：:/i18n/i18n_fixture_en.qm）。
//
// 它只做一件事：在 load() 里用 translate(u8"i18n_fixture", …) 取一句自己的文案，把结果记进配置，供用例取值。于是这条
// 链上的三件事都被钉住：加载器在插件代码跑之前装目录、context 与目录文件名一致、内嵌资源在模块加载后可查。
#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/Translation.hpp>
#include <vine/appfw/plugin_export.hpp>

VN_APPFW_NS_BEGIN

class TranslationPlugin : public Plugin {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Records the plugin's own translated text so a test can read it back.
     *
     * @param context Load context exposing host capabilities.
     * @return A task that completes when the plugin is loaded.
     */
    vn::async::Task<void> load(PluginLoadContext* context) override;
};

VN_OBJECT_META_IMPL(TranslationPlugin, Plugin)

vn::async::Task<void> TranslationPlugin::load(PluginLoadContext* context)
{
    raw_ptr<Application> app    = context != nullptr ? context->application() : nullptr;
    ConfigManager*       config = app != nullptr ? app->configManager() : nullptr;
    if (config != nullptr) {
        // 键是夹具自己的探针，正常运行时不会有人读它。
        config->setString(u8"plugins.i18n.probe", translate("i18n_fixture", "插件文案"));
    }
    co_return;
}

VN_DECLARE_PLUGIN(TranslationPlugin,
                  u8"6a1b6d0e-0000-4000-8000-0000000000f2",
                  u8"i18n_fixture",
                  u8"Translation fixture",
                  u8"0.1.0",
                  u8"Carries its own catalog to pin where a plugin's translations come from",
                  u8"Vine",
                  u8"",
                  u8"",
                  u8"",
                  {})

VN_APPFW_NS_END
