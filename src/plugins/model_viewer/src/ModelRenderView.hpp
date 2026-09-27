#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <QWidget>

#include <QVBoxLayout>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneView.hpp>

#include "ModelDocument.hpp"

namespace vn::model_viewer
{

/**
 * @brief 模型文档的视图：**真的把这份模型渲染出来**（不是显示两个数的替身）。
 *
 * **一个文档 = 一个视图 = 它自己那份资源**（appfw-document-model.md §7/§9）。应用级共享的只有外壳那一套
 * —— DockPanel、Ribbon 菜单（它们跟随"当前文档"的语义）；文档区里的东西一样不共享：
 *
 *  - 本视图**自带**渲染控件（自己的原生面、自己的会话/设备、自己编的管线）：两份同类型文档各有各的渲染面，
 *    切文档只是换页，不会把谁的资源借给谁；
 *  - 本视图有自己的 `SceneView`（自己的相机、自己的场景、自己的轨道操纵器）与自己的 `Geometry` 副本，
 *    所以两份文档的相机状态互不影响，切回来还是各自的角度；
 *  - **懒建**：场景与 `SceneView` 在第一次真的上屏时才造（`activate()`），而会话在**第一次被布局过之后**
 *    才 attach（`RenderControl::init()`："宿主给时机、控件维护会话"）；打开十份文档就真的建十份
 *    （vsg 那边的设备数上限已解开，见 `gfx_backend_vsg/CMakeLists.txt` 里的 `VSG_MAX_DEVICES`）。
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
    {
        // 本视图自己的渲染控件，铺满本视图的控件 —— 它和视图一起生、一起死（控件树是它的拥有者：
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
     * @brief 本视图自己的渲染控件（自己的面/会话/设备）。
     *
     * @return 控件；生命周期与本视图相同。
     */
    vn::appfw::gui::RenderControl* renderControl() const { return control_; }

  private:
    /// 造这份模型的场景与视图（只造一次）。网格为空时不造，`activate()` 会因此什么都不呈现。
    void buildScene();

    /// 网格 → 引擎能画的 `Geometry`（缺法线就按三角面算一套平滑法线）。
    vn::intrusive_ptr<vn::graphics::Geometry> makeGeometry() const;

    /**
     * @brief 按三角面累加顶点法线并归一化（索引不变，因此是平滑法线）。
     *
     * STL 这类来源只有面法线、没有顶点法线；前向内容程序要读法线，缺了就画成一片死黑。
     * 平滑（而不是按面复制顶点）是刻意的选择：它不改索引，因此不改变面板上那两个数（顶点数/三角形数）——
     * 视图怎么画不该让文档的几何事实变样。
     *
     * @param positions 顶点位置（与索引空间同序）。
     * @param indices   三角索引（三个一组）。
     * @return 与 positions 同长的法线。
     */
    static std::vector<vn::math::Vec3f> smoothNormals(std::span<const vn::math::Vec3f> positions,
                                                      std::span<const std::uint32_t>  indices);

  private:
    ModelDocument*                             model_   = nullptr;
    vn::appfw::gui::RenderControl*             control_ = nullptr;
    vn::intrusive_ptr<vn::graphics::Scene>     scene_;
    vn::intrusive_ptr<vn::graphics::SceneView> view_;
    bool                                       fitted_ = false;
};
inline std::vector<vn::math::Vec3f> ModelRenderView::smoothNormals(std::span<const vn::math::Vec3f> positions,
                                                                   std::span<const std::uint32_t>  indices)
{
    std::vector<vn::math::Vec3f> normals(positions.size(), vn::math::Vec3f(0.0f, 0.0f, 0.0f));

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const std::uint32_t i0 = indices[i + 0];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
        {
            continue; // 越界索引：跳过这个面，而不是读出去。
        }

        const auto face = (positions[i1] - positions[i0]).cross(positions[i2] - positions[i0]);
        // 面积加权（叉积的长度就是两倍面积）：大面对法线的贡献更大，退化面贡献为零。
        normals[i0] += face;
        normals[i1] += face;
        normals[i2] += face;
    }

    for (auto& normal : normals)
    {
        const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length > 1.0e-12f)
        {
            normal = normal * (1.0f / length);
        }
        else
        {
            normal = vn::math::Vec3f(0.0f, 0.0f, 1.0f); // 孤立顶点：给一个确定的朝向，别留零向量。
        }
    }
    return normals;
}

inline vn::intrusive_ptr<vn::graphics::Geometry> ModelRenderView::makeGeometry() const
{
    if (model_ == nullptr)
    {
        return nullptr;
    }

    auto mesh = model_->mesh();
    if (mesh == nullptr)
    {
        return nullptr;
    }

    const auto positions = mesh->positions();
    const auto indices   = mesh->indices();
    if (positions.empty() || indices.size() < 3)
    {
        return nullptr; // 没有可画的东西（空文档）：让 activate() 什么都不呈现，而不是建个空场景。
    }

    auto geometry = vn::make_intrusive<vn::graphics::Geometry>();
    geometry->setName(model_->title());
    geometry->setPositions(vn::graphics::packAttribute(positions));
    geometry->setIndices(vn::graphics::packIndices(indices));

    // 法线：来源带了就用它（长度对得上才算带了），没带就自己算平滑法线。
    const auto normals = mesh->normals();
    if (normals.size() == positions.size())
    {
        geometry->setNormals(vn::graphics::packAttribute(normals));
    }
    else
    {
        geometry->setNormals(vn::graphics::packAttribute(smoothNormals(positions, indices)));
    }

    // 材质：中灰（不是纯白）—— 前向程序把材质与环境/漫反射相乘，纯白在亮背景下会让明暗看不出来。
    auto material = vn::make_intrusive<vn::graphics::Material>();
    material->setDiffuse(vn::Colorf(0.72f, 0.74f, 0.78f, 1.0f));
    material->setSpecular(vn::Colorf(0.25f, 0.25f, 0.25f, 1.0f));
    material->setShininess(48.0f);
    geometry->setMaterial(std::move(material));

    return geometry;
}

inline void ModelRenderView::buildScene()
{
    auto geometry = makeGeometry();
    if (geometry == nullptr)
    {
        return;
    }

    auto root = vn::make_intrusive<vn::graphics::Group>();
    root->setName(u8"model_root");
    root->addChild(geometry);

    scene_ = vn::make_intrusive<vn::graphics::Scene>();
    scene_->setName(model_ != nullptr ? model_->title() : vn::String());
    scene_->setRoot(root);

    // 自己的 view：自己的相机与轨道操纵器 ⇒ 两份同类型文档互不影响（切回来还是各自的角度）。
    // 引擎也是本视图自己的：本视图自带渲染控件，所以这条会话、这个设备只服务这份文档。
    view_ = vn::make_intrusive<vn::graphics::SceneView>();
    view_->setScene(scene_);
    view_->setEngine(control_->engine());
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
