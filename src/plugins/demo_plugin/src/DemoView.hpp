#pragma once

#include <memory>

#include <QVBoxLayout>
#include <QWidget>

#include <vine/async/DetachedTask.hpp>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include "DemoDocument.hpp"
#include "DemoScene.hpp"

namespace vn::demo
{

/**
 * @brief 演示文档的视图：**自带渲染面**（一个文档 = 一个视图 = 它自己那份资源）。
 *
 * 今天的外壳不再在启动时建中央区那块 `RenderControl`，演示的整条编排搬到这里：
 *
 *  - 第一次上屏（`activate()`）时造骨架 —— 场景、相机、pass、管线（`DemoScene::install()`，**必须在 attach
 *    之前**，管线是照已登记的 pass 建的）；
 *  - 然后 `RenderControl::init()` 建会话（宿主给时机、控件维护会话：布局还没跑出来时它会返回 false，下次
 *    `activate()` 再试）；
 *  - 会话起来之后才去装贴图（`assembleDemoContentLater()`，它自己等 attach 结束），重活仍在别的线程上。
 *
 * 显示期间它把这块控件发布到窗口上（`MainWindow::setPrimaryRenderControl()`）：这是给"想拿到当前 3D 视图"的
 * 插件用的读数（`test_plugin` 的实时渲染演示就是），下屏时收回去。
 */
class DemoView final : public vn::appfw::gui::DocumentView
{
  public:
    /**
     * @brief 造一份演示视图（连同它自己的渲染面）。
     *
     * @param document 演示文档（生命周期长于本视图）。
     */
    explicit DemoView(DemoDocument& document)
      : DocumentView(document, new QWidget())
    {
        control_ = new vn::appfw::gui::RenderControl();
        auto* layout = new QVBoxLayout(impl<QWidget>());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(control_->impl<QWidget>());
    }

  public:
    /// @return 本视图自己（连带它自己的渲染控件）：一实例一页。
    vn::appfw::gui::UIElement* content() override { return this; }

    /// 上屏：首次造骨架，并把"建会话 + 开内容"安排到**下一拍**（见 activate 的注释）。
    void activate() override;

    /// 下屏：不再自称是窗口上的 3D 视图（会话留着，切回来就接着用）。
    void deactivate() override;

    /// @return 本视图自己的渲染控件。
    vn::appfw::gui::RenderControl* renderControl() const { return control_; }

  private:
    vn::appfw::gui::RenderControl* control_         = nullptr;
    std::shared_ptr<DemoScene>     scene_;
    bool                           attach_posted_   = false;
    bool                           content_started_ = false;
};

inline void DemoView::activate()
{
    if (scene_ == nullptr)
    {
        scene_ = std::make_shared<DemoScene>(control_);
        scene_->install();
    }

    // 会话**立刻**开始建（不排到下一拍）：QWindow 一旦 new 出来句柄就可用了，而这一拍文档刚被打开、窗口还没上屏 ——
    // 于是第一帧是**屏外的预热帧**、走线程池（`initAsync()` 的契约），设备/管线的几百毫秒不落在应用线程上。
    // 反过来（等窗口上屏再建）第一帧就由应用线程渲染，启动占用直接 200 ms+（实测 204 ms / 限 150 ms）。
    //
    // TODO(生命周期)：这一支捕获 this —— 文档在 attach 途中被关掉时会悬空（视图与它的控件都随文档消失）。
    // 要定的是"谁替这个异步负责"：宿主驱动（宿主拥有视图，天然知道它什么时候死）或让视图/场景共享控件所有权。
    if (!attach_posted_)
    {
        attach_posted_ = true;
        [](DemoView* self) -> vn::async::DetachedTask {
            const bool attached = co_await self->control_->initAsync();
            if (attached && !self->content_started_)
            {
                self->content_started_ = true;
                assembleDemoContentLater(self->scene_);
            }
        }(this);
    }

    if (auto* window = vn::appfw::gui::MainWindow::current(); window != nullptr && window->primaryRenderControl() != control_)
    {
        window->setPrimaryRenderControl(control_);
    }
}

inline void DemoView::deactivate()
{
    if (auto* window = vn::appfw::gui::MainWindow::current();
        window != nullptr && window->primaryRenderControl() == control_)
    {
        window->setPrimaryRenderControl(nullptr);
    }
}

/**
 * @brief 视图工厂：登记给 `"demo"` 类型。
 *
 * @param document 要呈现的文档。
 * @return 新的视图（宿主拥有），或 nullptr（不是演示文档）。
 */
inline vn::appfw::gui::DocumentView* createDemoView(vn::appfw::Document& document)
{
    auto* demo = dynamic_cast<DemoDocument*>(&document);
    if (demo == nullptr)
    {
        return nullptr;
    }
    return new DemoView(*demo);
}

} // namespace vn::demo
