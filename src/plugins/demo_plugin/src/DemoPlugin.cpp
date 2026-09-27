#include "DemoPlugin.hpp"

#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/StartupProgress.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/plugin_export.hpp>
#include <vine/logging/Log.hpp>

#include "DemoDocument.hpp"
#include "DemoView.hpp"

namespace vn::demo
{

VN_OBJECT_META_IMPL(DemoPlugin, vn::appfw::Plugin)

DemoPlugin::DemoPlugin() = default;

vn::async::Task<void> DemoPlugin::load(vn::appfw::PluginLoadContext* context)
{
    auto* app = context != nullptr ? context->application() : nullptr;
    if (app == nullptr) {
        co_return;
    }

    // 演示不再是"外壳启动时无条件建出来的东西"，而是一份普通文档：登记类型 + 登记视图就够了。
    // 内容（骨架/会话/贴图）由**视图**在第一次上屏时建 —— 没打开这份文档就什么都不建。
    registerDocumentType(*app);
    registerDemoView(*app);
    co_return;
}

void DemoPlugin::registerDocumentType(vn::appfw::Application& app)
{
    auto* documents = app.documentManager();
    if (documents == nullptr) {
        return;
    }

    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = DemoDocument::kTypeId;
    registration.display_name = u8"演示";
    registration.description  = u8"演示场景：渲染示例的集合（只读，没有来源）";
    // 有 create 工厂：这份文档不需要载荷 —— 谁来开都可以 `documents->create("demo")`（`open_demo` 与
    // 启动参数 `--open demo` 走的就是这一条）。
    registration.create = []() -> vn::appfw::Document* { return new DemoDocument(); };
    documents->registerType(registration);
}

void DemoPlugin::registerDemoView(vn::appfw::Application& app)
{
    // 视图是 UIElement ⇒ 只有 GUI 宿主才有视图注册表（无头运行里没有"中央区显示什么"这件事）。
    auto* gui_app = dynamic_cast<vn::appfw::gui::GuiApplication*>(&app);
    auto* views   = gui_app != nullptr ? gui_app->viewRegistry() : nullptr;
    if (views == nullptr) {
        return;
    }

    if (!views->registerView(DemoDocument::kTypeId, &createDemoView)) {
        VN_LOGW("demo_plugin: the view for '{}' was not registered (taken, or empty id)", "demo");
    }
}

}  // namespace vn::demo

/// 插件图标（内联 SVG）。含逗号的 SVG 要放在命名常量里：VN_DECLARE_PLUGIN 是宏，
/// 圆括号外层的逗号会把参数切开。
constexpr const char8_t* kPluginIcon = u8R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#c98a2a" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3.2 20 7.6v8.8L12 20.8 4 16.4V7.6z"/><path d="M4 7.6 12 12l8-4.4"/><path d="M12 12v8.8"/></g></svg>)SVG";

// 宏在**全局命名空间**展开（VN_DEFINE_MODULE_COMMAND_QUEUE() 在 vn::appfw::detail 里定义函数，
// 要求展开位置必须包着那个命名空间），所以类名写成全限定名——同 gfx_backend_vsg。
VN_DECLARE_PLUGIN(vn::demo::DemoPlugin, u8"bb3c1e2d-8c8b-4a1c-a9d2-e884407d69a3", u8"demo_plugin", u8"演示场景",
                 u8"1.0.0", u8"演示场景：把渲染内容与管线示例挂到应用外壳的 3D 视图上", u8"Vine", u8"dev@vine.example",
                 u8"https://github.com/vine/demo_plugin", kPluginIcon, { u8"app_shell" })
