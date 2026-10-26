#pragma once

#include <cmath>
#include <cstddef>
#include <functional>
#include <vector>

#include <QWidget>

#include <QVBoxLayout>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraManipulator.hpp>
#include <vine/graphics/FpsOverlay.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/LightRig.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/ModelScene.hpp>
#include <vine/graphics/OrbitCameraManipulator.hpp>
#include <vine/graphics/Ray.hpp>
#include <vine/graphics/RayIntersection.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/window/InputEvent.hpp>

#include "ModelDocument.hpp"

namespace vn::model_viewer
{

/**
 * @brief 这份视图的操纵器：**导航照旧，另外把"点击"交出来**。
 *
 * 为什么是操纵器而不是别的地方：拾取要的是"光标下的那条射线 + 谁被射中"，而**输入进到视图的唯一通道就是操纵器**
 * （`SceneView` 把窗口事件推给它的操纵器，见 CameraManipulator 的注释）。选择是**业务**（哪一份文档选中了谁、
 * 选中长什么样都是本插件的事），所以这里只做一件事：把每个事件原样转给内层那个轨道操纵器（导航行为一字不变），
 * 同时在"按下与松开落在同一点附近"时把**归一化**坐标回调出去 —— 归一化是因为 `Camera::screenToWorldRay()`
 * 收的就是 [0, 1] 的屏幕坐标，而视口尺寸只有操纵器知道（它在 `onResize` 里拿到）。
 *
 * 判"点击"的松弛量（`kClickSlopPixels`）是刻意的：拖拽结束也算一次松手，没有这个松弛量，转完相机就会顺手选中
 * 一个东西 —— 那是"导航坏了"的观感，不是选择。
 */
class ModelPickManipulator final : public vn::graphics::CameraManipulator
{
  public:
    /// @brief 点击允许的位移（设备像素）：按下与松开的距离超过它就算拖拽，不拾取。
    static constexpr double kClickSlopPixels = 4.0;

    /**
     * @brief 造一个转发型操纵器。
     *
     * @param camera 这份视图的相机（内层轨道操纵器也绑它）。
     * @param scene 这份视图的场景。内层轨道操纵器要用它算包围盒：`fitToScreen()` 取得的是场景内容的范围，
     *        不是相机自己的默认站位 —— 少了它，适应屏幕就退化成回到默认相机。
     * @param on_click 点击回调，收**归一化**屏幕坐标（[0, 1]，左上角原点）。
     */
    ModelPickManipulator(vn::raw_ptr<vn::graphics::Camera> camera, vn::raw_ptr<vn::graphics::Scene> scene,
                         std::function<void(double, double)> on_click)
      : vn::graphics::CameraManipulator(camera)
      , on_click_(std::move(on_click))
      , inner_(vn::make_intrusive<vn::graphics::OrbitCameraManipulator>(camera, scene))
    {
    }

  public:
    void apply() override { inner_->apply(); }

    bool fitToScreen() override { return inner_->fitToScreen(); }

    void home() override { inner_->home(); }

    void onMousePress(const vn::window::MouseEvent& event) override
    {
        pressed_   = true;
        press_x_   = event.x;
        press_y_   = event.y;
        inner_->onMousePress(event);
    }

    void onMouseMove(const vn::window::MouseEvent& event) override { inner_->onMouseMove(event); }

    void onMouseRelease(const vn::window::MouseEvent& event) override
    {
        inner_->onMouseRelease(event);
        const double moved = std::abs(event.x - press_x_) + std::abs(event.y - press_y_);
        const bool   click = pressed_ && moved <= kClickSlopPixels;
        pressed_           = false;
        if (click && on_click_ && viewport_w_ > 0.0 && viewport_h_ > 0.0)
        {
            on_click_(event.x / viewport_w_, event.y / viewport_h_);
        }
    }

    void onScroll(const vn::window::ScrollEvent& event) override { inner_->onScroll(event); }

    void onKeyDown(const vn::window::KeyEvent& event) override { inner_->onKeyDown(event); }

    void onKeyUp(const vn::window::KeyEvent& event) override { inner_->onKeyUp(event); }

    void onResize(const vn::window::ResizeEvent& event) override
    {
        // 视口尺寸是拾取那一半需要的（归一化），导航那一半自己也会从同一个事件里读。
        viewport_w_ = event.width > 0 ? static_cast<double>(event.width) : 0.0;
        viewport_h_ = event.height > 0 ? static_cast<double>(event.height) : 0.0;
        inner_->onResize(event);
    }

  private:
    std::function<void(double, double)>            on_click_;
    vn::intrusive_ptr<vn::graphics::OrbitCameraManipulator> inner_;
    bool                                           pressed_    = false;
    double                                         press_x_    = 0.0;
    double                                         press_y_    = 0.0;
    double                                         viewport_w_ = 0.0;
    double                                         viewport_h_ = 0.0;
};

/**
 * @brief 模型文档的视图：**真的把这份模型渲染出来**（不是显示两个数的替身）。
 *
 * **一个文档 = 一个视图 = 它自己那份资源**（appfw-document-model.md §7/§9）。应用级共享的只有外壳那一套
 * —— DockPanel、Ribbon 菜单（它们跟随"当前文档"的语义）；文档区里的东西一样不共享：
 *
 *  - 本视图**自带**渲染控件（自己的原生面、自己的会话/设备、自己编的管线）：两份同类型文档各有各的渲染面，
 *    切文档只是换页，不会把谁的资源借给谁；
 *  - 场景与 `Geometry` 副本是每份文档自己的：这套东西（相机、场景、轨道操纵器）**随渲染控件**走，
 *    一个文档一个控件，所以两份文档的相机状态互不影响，切回来还是各自的角度；
 *  - **懒建**：场景与 `SceneView` 在第一次真的上屏时才造（`activate()`），而会话在**第一次被布局过之后**
 *    才 attach（`RenderControl::init()`："宿主给时机、控件维护会话"）；打开十份文档就真的建十份设备，而本 build
 *    的同时设备上限是 vsg 的 `VSG_MAX_DEVICES`（4）—— 超出的那一次 attach 以理由失败（`RenderControl` 会进
 *    `Failed` 并给出 `failureReason()`，拒绝在 `gfx_backend_vsg` 里，见
 *    `VsgBackendTest.ADeviceCapThatIsReachedIsAReasonAndNotAnException`）。
 *  - **自带灯与帧率读数**：灯挂在场景上（`scene->addLighting(LightRig::studio())`，没人装就只有后端的常量填充，
 *    画面是一片平色），帧率读数由**控件的查看器预设**提供（`control_->setViewerOptions(... .stats = true)`，
 *    尺寸与比例归链维护）—— 读数归本视图，因为"这一份文档的帧率"就是这一块渲染面的帧率。
 *  - **回答"命令该作用在哪块区"**：`renderControl()` 返回本视图自己的控件（本视图只有一个窗格）；命令在派发时
 *    解析出的目标就是本视图，于是别的插件（本插件的"显示模式"命令、`test_plugin` 的实时渲染演示）不必认识任何
 *    具体视图类，也不用去问窗口“谁是当前的”。
 *  - **点击选中**：本视图自己带一个拾取型操纵器（`ModelPickManipulator`：导航原样转发给轨道操纵器，另外把"点击"
 *    交回本视图），点到谁就把谁的材质换成**高亮材质**（原来那份留着，落空或换选时原样还回去）。选择是**业务**（住
 *    在这份视图里），图形层只提供射线拾取（`Camera::screenToWorldRay` + `RayIntersection`）与材质 —— 框架里没有
 *    "选择"这个概念，也不需要加一个。
 *
 * 几何信息（顶点数/三角形数）归 `ModelInfoPanel` 那个停靠面板，不在中央区里重复。
 *
 * 和这个插件里的面板一样，它是 header-only 的（成员定义都在类外、都带 `inline`）：用例可以直接 include 它来
 * 验证，而不必把插件的 .so 链进来（见 `tests/test_gui/ModelViewerTest.cpp`）。成员都内联也意味着它没有"键
 * 函数"，虚表在每个用到它的 TU 里都有一份弱定义 —— 这是它不需要 `VN_OBJECT_META_DECL` 的前提。
 */
class ModelRenderView final : public vn::appfw::gui::DocumentView
{
  public:
    /**
     * @brief 造一份模型视图（连同它自己的渲染面）。
     *
     * @param document 要呈现的模型文档（生命周期长于本视图：文档关闭时宿主销毁视图）。
     */
    explicit ModelRenderView(ModelDocument& document)
      : DocumentView(document, new QWidget())
      , model_(&document)
    {        // 本视图自己的渲染控件，铺满本视图的控件 —— 它和视图一起生、一起死（控件树是它的拥有者：
        // RenderControl 自己会在它的控件随父销毁时自毁）。
        control_ = new vn::appfw::gui::RenderControl();
        auto* layout = new QVBoxLayout(impl<QWidget>());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(control_->impl<QWidget>());
    }

  public:
    /**
     * @brief 放进中央区的东西：本视图自己（连带它自己的渲染控件）。
     *
     * 一实例一页：3D 视图与 2D 视图在宿主眼里完全一样（都是"自己就是那一页"），所以宿主不需要知道
     * 谁在渲染 —— 宿主那套页规则只有一条。
     *
     * @return 本视图。
     */
    vn::appfw::gui::UIElement* content() override { return this; }

    /**
     * @brief 这份文档上屏：首次造场景并建立会话，之后只是重画。
     *
     * 相机只在第一次对准内容，之后保持使用者调过的视角（这正是"视图按实例"要保住的东西）。
     */
    void activate() override;

    /**
     * @brief 这份文档下屏：场景与控件留着（视图是实例的，切回来要保住视角），没有窗口级状态要收。
     */
    void deactivate() override;

    /**
     * @brief 命令该作用于的那块 3D 区 —— 也就是本视图自己的渲染控件（本视图只有一个窗格）。
     *
     * @details
     * - 命令在派发时解析出的目标就是本视图（见 ADR-2026-10-24-command-target.md）：视图有多个窗格时在这里改答
     *   用户最后用的那一个，框架不用动。
     * - 覆盖基类的查询，不是额外的状态：控件随本视图生灭，覆盖它只是把“哪一块”说出来。
     *
     * @return 控件；生命周期与本视图相同。
     */
    vn::appfw::gui::RenderControl* renderControl() const override { return control_; }

    /**
     * @brief 当前选中的几何（点击命中的那一个），没选中时为空。
     *
     * 选择住在**本视图**里（一个文档一个视图，切文档切视图都各有各的选择）：框架里没有这个概念，所以这里就是
     * 它的读法。
     *
     * @return 选中的几何，或 nullptr。
     */
    vn::raw_ptr<vn::graphics::Geometry> selectedGeometry() const { return selected_geometry_.get(); }

    /**
     * @brief 右下角的帧率读数（查看器的链里的 HUD 档）。
     *
     * 读数不再由查看器自己造：查看器预设里的 `stats` 让链带上这个 HUD 档，于是**尺寸、比例、摆位、窗口缩放**
     * 全归链维护（见 `PassChain`），显示/隐藏是链上的零重建开关（`PassChain::setStatsVisible()`）。空文档时视图
     * 还没建链，这里就是空指针。
     *
     * @return 读数 pass；视图还没有链时是空指针。
     */
    vn::raw_ptr<vn::graphics::FpsOverlay> fpsOverlay() const
    {
        auto* view = view_.get();
        auto* pipeline = view != nullptr ? view->chain() : nullptr;
        return pipeline != nullptr ? pipeline->fpsOverlay() : nullptr;
    }

  private:
    /// 造这份模型的场景（只造一次）：内容放进控件那个 view 的场景里。网格为空时不造，`activate()` 会因此什么都不呈现。
    void buildScene();

    /**
     * @brief 在一次点击处拾取并选中（见 ModelPickManipulator）。
     *
     * 射线用 `Camera::screenToWorldRay` 造（视图自己的相机、相机自己的画面形状），命中取最近的那一件
     * （`RayIntersection::intersectScene`）；命中就把它的材质换成高亮材质，落空就取消选择。没有命中任何东西
     * 也算一次合法的"点空白"。
     *
     * @param nx 归一化屏幕横坐标（[0, 1]，左上角原点）。
     * @param ny 归一化屏幕纵坐标（[0, 1]，左上角原点）。
     */
    void selectAt(double nx, double ny);

    /// 取消选择：把选中前那份材质原样还回去，并把选择清空。
    void clearSelection();

    /**
     * @brief 给这份文档的场景装灯。
     *
     * 灯是**场景**的属性，不是渲染面的：每个视图有自己的场景（见类注释），所以每个视图必须自己带灯，否则这份
     * 文档会落进"零灯"那一档 —— 后端在没有环境光时只补一个 0.15 的常量填充，网格于是被画成一片平色，看起来
     * 就是"没有光照效果"。
     *
     * 用的是 `LightRig::studio()` 那套（一环境 + 主光 + 补光，方向**世界固定**、不跟相机走）：绕行或自转时明暗
     * 只随几何变化，同一个模型永远有同一张明暗图，"这里是不是画错了"才有可对照的答案。
     *
     * @param scene 这份视图的场景。
     */
    static void addLighting(vn::graphics::Scene& scene);

    /**
     * @brief 给没有材质的几何补一份中灰材质（显示策略，不是模型的事实）。
     *
     * 文件里没声明材质的网格（STL 就是这种）在引擎里落到"默认材质"；查看器给它一份中灰（不是纯白）—— 前向
     * 程序把材质与环境/漫反射相乘，纯白在亮背景下会让明暗看不出来。整棵子树共用**同一份**材质对象，于是后端
     * 能把它们归成一个批次。
     *
     * @param node   子树根。
     * @param shared 共用那份中灰材质。
     */
    static void applyDefaultMaterials(vn::graphics::Node& node, const vn::intrusive_ptr<vn::graphics::Material>& shared);

  private:
    ModelDocument*                             model_   = nullptr;
    vn::appfw::gui::RenderControl*             control_ = nullptr;
    /// 控件那个 view（借来放场景的，不是本视图造的）：`SceneView` 归渲染面所有，见 `buildScene()`。
    vn::intrusive_ptr<vn::graphics::SceneView> view_;
    /// 右下角的帧率读数由查看器预设提供（`ViewerOptions::stats`），见 `fpsOverlay()`。
    bool                                       fitted_ = false;
    /// 当前选中的几何：空 = 没选中。
    vn::intrusive_ptr<vn::graphics::Geometry> selected_geometry_;
    /// 选中前的材质：取消选择时原样还回去（哪一种是素材还是它上面的实例，视图不关心）。
    vn::intrusive_ptr<vn::graphics::MaterialBase> selected_material_;
    /// 高亮材质（第一次选中时才造）：橙色、低高光，与中灰的模型放在一起一眼能认出"选中的是它"。
    vn::intrusive_ptr<vn::graphics::Material>  highlight_;
};
inline void ModelRenderView::applyDefaultMaterials(vn::graphics::Node& node,
                                                    const vn::intrusive_ptr<vn::graphics::Material>& shared)
{
    if (auto* geometry = dynamic_cast<vn::graphics::Geometry*>(&node))
    {
        if (geometry->material() == nullptr)
        {
            geometry->setMaterial(shared);
        }
        return;
    }
    if (auto* group = dynamic_cast<vn::graphics::Group*>(&node))
    {
        for (const auto& child : group->children())
        {
            if (child != nullptr)
            {
                applyDefaultMaterials(*child, shared);
            }
        }
    }
}

inline void ModelRenderView::clearSelection()
{
    if (selected_geometry_ == nullptr)
    {
        return;
    }
    // 原来那份材质**原样还回去**（而不是重造一份一样的）：材质是内容的属性，视图只是临时换了它。
    selected_geometry_->setMaterial(std::move(selected_material_));
    selected_geometry_ = nullptr;
    selected_material_ = nullptr;
}

inline void ModelRenderView::selectAt(double nx, double ny)
{
    if (view_ == nullptr || view_->camera() == nullptr || view_->scene() == nullptr)
    {
        return;
    }

    const vn::graphics::Ray ray = view_->camera()->screenToWorldRay(vn::math::Vec2d(nx, ny));
    const vn::graphics::RayIntersectionResult hit =
        vn::graphics::RayIntersection::intersectScene(ray, view_->scene().get());

    if (!hit.hit || hit.geometry == nullptr)
    {
        clearSelection(); // 点空白 = 取消选择（合法的选择动作，不是"什么都没发生"）
    }
    else if (hit.geometry != selected_geometry_)
    {
        clearSelection(); // 换选：先把上一件的材质还回去
        if (highlight_ == nullptr)
        {
            highlight_ = vn::make_intrusive<vn::graphics::Material>();
            // Roughness from the shininess this highlight used to state (16), by sqrt(2 / (n + 2)).
            highlight_->setBaseColor(vn::Colorf(1.0f, 0.55f, 0.12f, 1.0f));
            highlight_->setRoughness(0.333f);
        }
        selected_geometry_ = hit.geometry;
        selected_material_ = vn::intrusive_ptr<vn::graphics::MaterialBase>(hit.geometry->material());
        selected_geometry_->setMaterial(highlight_);
    }

    // 按需出帧的控件也要把这一笔画出来（连续出帧时多要一帧无害）。后端每帧都会重读命令里的材质：
    // 材质自己数改动（`Material::revision()`），后端只比这一个数字（见 `ContentStore::updateMaterial`），
    // 所以这里不需要任何"通知后端"的调用。
    control_->renderFrame();
}

inline void ModelRenderView::addLighting(vn::graphics::Scene& scene)
{
    if (scene.hasLights())
    {
        return; // 场景是本视图自己的，但建场景会重试（控件还没布局好时）：灯只装一次。
    }

    // 一套 studio 灯（一环境 + 主光 + 补光，方向世界固定）：以前这里手写 25 行，现在是一行 —— 预设的取值与
    // 原来那三盏一致（环境 0.25、主光 1.0、补光 0.35），所以观感不变。
    scene.addLighting(vn::graphics::LightRig::studio());
}

inline void ModelRenderView::buildScene()
{
    const vn::geometry::Model* source = model_ != nullptr ? model_->model() : nullptr;
    if (source == nullptr)
    {
        return; // 空文档：让 activate() 什么都不呈现，而不是建个空场景。
    }

    // 结构就是模型自己的：`sceneFromModel` 把节点树变成场景节点（identity → Group、非 identity →
    // MatrixTransform），每个 entry 一个 `Geometry`（顶点 buffer 共享，叶子不共享 —— 场景图是严格树）。
    const vn::graphics::ModelScene converted = vn::graphics::sceneFromModel(*source);
    if (converted.root == nullptr)
    {
        return;
    }

    // 场景必须挂在**控件那个 view** 上，不能自己另造一个：`SurfaceWindow` 构造时就造了它唯一的那一个
    // `SceneView`（在那儿 setEngine，后端也只绑它），挂到别的 `SceneView` 上的东西屏幕上什么都不会有
    // —— 这正是"打开模型却看不到"的成因（demo_plugin 也是这么写的：`render_control->view()->scene()`）。
    // "一份文档一份相机"仍然成立：一个文档一个视图，一个视图一个控件，一个控件一个面、一个 view。
    auto* control_view = control_ != nullptr ? control_->view() : nullptr;
    if (control_view == nullptr)
    {
        return;
    }
    view_ = vn::intrusive_ptr<vn::graphics::SceneView>(control_view);

    // 拾取型操纵器：导航一字不改（每个事件转发给内层轨道操纵器），点击交回本视图（见 ModelPickManipulator 的注释）。
    // 相机由 view 提供（`SceneView::camera()`），视口尺寸由 onResize 那条路进来 —— 两者都在操纵器手里；
    // 场景一并交给它，否则适应屏幕（激活时的自动对焦与右键菜单）拿不到内容范围。
    view_->setManipulator(vn::make_intrusive<ModelPickManipulator>(
        view_->camera(), view_->scene().get(), [this](double nx, double ny) { selectAt(nx, ny); }));

    // 文件里没声明材质的网格（STL 就是这种）补一份中灰：整棵子树共用**同一份**材质对象，后端能把它们
    // 归成一个批次；没有这一步，它们会落到引擎默认材质上（查看器不该让同一个模型有两种外观）。
    auto grey = vn::make_intrusive<vn::graphics::Material>();
    // Roughness from the shininess this fallback used to state (48), by sqrt(2 / (n + 2)).
    grey->setBaseColor(vn::Colorf(0.72f, 0.74f, 0.78f, 1.0f));
    grey->setRoughness(0.2f);
    applyDefaultMaterials(*converted.root, grey);

    auto root = converted.root;

    // 视图的引擎是面自己设好的（SurfaceWindow 那一段），这里只把内容放进去：内容场景、相机、轨道操纵器都是
    // 这一份文档自己的。
    auto scene = view_->scene();
    scene->setName(model_ != nullptr ? model_->title() : vn::String());
    scene->setRoot(root);
    addLighting(*scene);
    // 帧率读数：让控件的查看器预设带上这个 HUD 档 —— 读-改-写，`setViewerOptions()` 收的是整个预设，只送 stats
    // 会把链、坐标轴那些关掉。以前查看器自己造 `FpsOverlay` 挂引擎（pixel ratio、把逻辑尺寸转给它、resize 再补
    // 一次，31 行），现在尺寸/比例/摆位/窗口缩放全归链自己维护（见 PassChain）—— 而显示与隐藏是零重建的开关
    // （`PassChain::setStatsVisible()`，`System` 菜单里的"帧率"项就是它）。
    vn::appfw::gui::ViewerOptions options = control_->viewerOptions();
    options.stats                         = true;
    control_->setViewerOptions(options);
}

inline void ModelRenderView::activate()
{
    if (view_ == nullptr)
    {
        buildScene();
    }
    if (view_ == nullptr)
    {
        return; // 空文档：没有可呈现的东西，控件保持它自己的空白。
    }

    // 宿主给时机、控件维护会话：第一次真的被显示（布局过）时才建立；没建立成功下次 activate() 再试
    // （控件自己的规则：没拿到可用原生窗口时返回 false，不猜延时）。
    control_->init();
    if (!fitted_)
    {
        // 只在第一次对准内容：之后再切回来要保住使用者调好的视角（视图是实例的，就该是这个意思）。
        control_->fitToScreen();
        fitted_ = true;
    }

    // 发布"当前 3D 视图"：现在由窗口**派生**回答（当前文档视图 → 活动窗格），视图不再往窗口里写任何东西 ——
    // 单槽发布会让两个可见区域互相覆盖（见 ADR-2026-10-24-command-target.md）。
}

void ModelRenderView::deactivate()
{
    // 下屏的收尾：视图不持有任何窗口级状态，所以这里没有要收的东西（见上）。
}

/**
 * @brief 视图工厂：登记给 `"model"` 类型。
 *
 * 按 `dynamic_cast` 取回具体类型：宿主按 `typeId()` 查表，所以拿到这里的文档**应该**是 `ModelDocument`；
 * 取不回来就返回空（宿主显示"视图工厂拒绝了这份文档"）。
 *
 * 每次调用给的是**新实例**（带着它自己的渲染面）—— 一个文档一个视图，没有共用控件这回事。
 *
 * @param document 要呈现的文档。
 * @return 新的视图（宿主拥有），或 nullptr。
 */
inline vn::appfw::gui::DocumentView* createRenderView(vn::appfw::Document& document)
{
    auto* model = dynamic_cast<ModelDocument*>(&document);
    if (model == nullptr)
    {
        return nullptr;
    }
    return new ModelRenderView(*model);
}

} // namespace vn::model_viewer
