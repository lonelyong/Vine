#pragma once

#include <vine/appfw/Plugin.hpp>

namespace vn::appfw
{
class Application;
} // namespace vn::appfw

namespace vn::model_viewer
{

/**
 * @brief 模型查看器（测试插件）：一种文档类型 + 一个模型信息面板。
 *
 * 它把文档模型端到端跑一遍，作为**可点的**参考实现：
 *   - 登记文档类型 `"model"`；
 *   - 登记**两个打开器**（内存里的网格 / 磁盘上的网格文件）—— 一种类型吃多种载荷。编辑器要把 mesh 与 b-rep
 *     分成两个类型（操作逻辑不同），那只是登记方式不同，框架不需要为这两种应用分叉；
 *   - 把自己的面板挂到右侧 dock 上，并让它跟着当前文档走（面板自己听 `currentChanged`，见 ModelInfoPanel）。
 *
 * 文档只拿数据、不碰渲染（appfw-document-model.md §5.8）：这里没有渲染会话 —— 把模型画出来是视图层（§7）的事，
 * 而 `VSG_MAX_DEVICES=1` 决定了同一时刻只有一块渲染区。
 *
 * 面板"谁该看见"今天是插件自己建自己挂（框架还没有面板注册表，§12）；有了之后这一段变成一条声明。
 */
class ModelViewerPlugin : public vn::appfw::Plugin {
    VN_OBJECT_META_DECL;

  public:
    ModelViewerPlugin();

  public:
    vn::async::Task<void> load(vn::appfw::PluginLoadContext* context) override;

    /**
     * @brief 挂上"显示"那一组按钮（模型的显示模式）。
     *
     * 放在 `postLoad()` 而不是 `load()` 里：挂 UI 是"插件之间互相接线"那一档的事，而后加载的外壳可能还要往
     * ribbon 上加自己的 tab（`postLoad()` 是"所有插件都 load 完了"的第一个时刻，见 `Plugin` 的说明）。
     * 主窗口本身更早：外壳启动时就建好了（`GuiApplication::startupStart()` → `createWindows()`），所以 window 与
     * ribbon 一定在。无头运行（没有窗口）时命令照样登记，只是没有按钮。
     */
    vn::async::Task<void> postLoad(vn::appfw::PluginLoadContext* context) override;

  private:
    /// 登记文档类型与两个打开器（不碰 UI）。
    void registerDocumentSurface(vn::appfw::Application& app);

    /// 登记"文档类型 → 视图"（只有 GUI 宿主才有视图注册表）。
    void registerView(vn::appfw::Application& app);

    /** @brief 登记并挂上"显示"那一组按钮（模型的显示模式）。 */
    void installShadingModeRibbon(vn::appfw::Application& app);

    /// 建模型信息面板并挂到右侧 dock；没有主窗口（无头运行）时什么也不做。
    void installInfoPanel(vn::appfw::Application& app);
};

} // namespace vn::model_viewer
