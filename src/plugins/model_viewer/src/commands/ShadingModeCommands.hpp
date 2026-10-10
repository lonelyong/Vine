#pragma once

#include <vine/String.hpp>
#include <vine/appfw/Command.hpp>
#include <vine/graphics/SceneView.hpp>

namespace vn::model_viewer
{

/// 五条"显示模式"命令的名字。
///
/// 它们是常量而不是散落的字面量：插件自己往 ribbon 上挂按钮时用的是同一批名字（见 ModelViewerPlugin），
/// 改名字只改这里一处。
inline constexpr const char8_t* kShadingModeLitCommand       = u8"set_shading_lit";
inline constexpr const char8_t* kShadingModeFlatCommand      = u8"set_shading_flat";
inline constexpr const char8_t* kShadingModeUnlitCommand     = u8"set_shading_unlit";
inline constexpr const char8_t* kShadingModeNormalsCommand   = u8"set_shading_normals";
inline constexpr const char8_t* kShadingModeWireframeCommand = u8"set_shading_wireframe";

/**
 * @brief "显示模式"命令的共同部分：把**当前视图**的着色模式设成某一种。
 *
 * 一条命令一个模式（而不是一条"循环切换"）：菜单里每条都有说得清的名字，用户不必先点两下才知道现在在哪一档。
 * 模式本身是 `SceneView::Options::Mode`（graphics 那一层的东西，五档见那里），这里只负责"找到该改哪个视图"。
 *
 * **登记方式**：这五个类**不用** `VN_DECLARE_COMMAND` 的自动登记（那会把 `moduleCommandQueue()` 依赖带进每个
 * include 它的翻译单元，连带把"哪些命令属于本插件"这件事藏进静态初始化）—— 插件在自己的 `load()` 里按名字逐个
 * `registerCommand<T>()`：一处说得清、可读、用例也能照做（见 ModelViewerPlugin）。名字是下面那几个常量。
 *
 * **作用在哪个视图上**：宿主发布的那一个（`MainWindow::setPrimaryRenderControl()` —— 谁上屏谁发布）。这是框架
 * 给的通道（那接口的注释就是"让别的插件够到渲染引擎/场景，不必依赖 app_shell 内部"），所以这条命令不需要认识
 * 任何具体视图类。两道守卫让它不会改错东西：
 *
 *  - 当前文档不是模型文档 ⇒ `NotApplicable`：演示文档的视图不该被模型查看器的命令改掉；
 *  - 还没有视图上屏（文档开着但没显示过）⇒ 同样 `NotApplicable`，并说清原因（不静默）。
 *
 * **模式归视图、不归文档**：每份文档自带视图，所以换一份文档时它以默认（实面）出现。想让它跟着文档走就得把模式
 * 存进文档 —— 那是编辑器那一档的事（文档只拿数据，见 ModelDocument 的说明）。
 *
 * 改的时候是"读-改-写"（`view->options()` 拿当前配方再改 mode），所以帧率读数、坐标轴这些别的设置原样保留 ——
 * `setOptions()` 收的是整个配方，直接给一个只带 mode 的值会把它们关掉。
 */
class ShadingModeCommand : public vn::appfw::Command {
  public:
    vn::String            group() const override { return u8"显示"; }
    vn::appfw::CommandFlags flags() const override { return vn::appfw::CommandFlags::None; }

    /// 执行：设当前视图的着色模式（见类注释里的两道守卫与"读-改-写"）。
    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override;

  protected:
    /// 这条命令设的模式。
    virtual vn::graphics::SceneView::Options::Mode mode() const = 0;
};

/// 实面：完整光照（默认档）。
class SetShadingLitCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override { return vn::String(kShadingModeLitCommand); }
    vn::String description() const override { return u8"实面：环境光 + 主光 + 补光（默认）"; }

  protected:
    vn::graphics::SceneView::Options::Mode mode() const override { return vn::graphics::SceneView::Options::Mode::Lit; }
};

/// 平面：同样的光照，法线取三角面（没有逐顶点法线的数据也看得出形状）。
class SetShadingFlatCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override { return vn::String(kShadingModeFlatCommand); }
    vn::String description() const override { return u8"平面：逐面法线（同一个三角形的每个像素一个朝向）"; }

  protected:
    vn::graphics::SceneView::Options::Mode mode() const override { return vn::graphics::SceneView::Options::Mode::Flat; }
};

/// 无光照：只画表面色（材质 / 贴图 / 顶点色），没有任何光照项。
class SetShadingUnlitCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override { return vn::String(kShadingModeUnlitCommand); }
    vn::String description() const override { return u8"无光照：只画表面色 —— 分清是形状不对还是光照不对"; }

  protected:
    vn::graphics::SceneView::Options::Mode mode() const override { return vn::graphics::SceneView::Options::Mode::Unlit; }
};

/// 法线：把视图空间法线当颜色画（蓝 = 朝着相机）。
class SetShadingNormalsCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override { return vn::String(kShadingModeNormalsCommand); }
    vn::String description() const override { return u8"法线：视图空间法线当颜色（看朝向、缺失或内外翻的法线）"; }

  protected:
    vn::graphics::SceneView::Options::Mode mode() const override { return vn::graphics::SceneView::Options::Mode::Normals; }
};

/// 线框：只画三角面的边，用无光照的表面色。
class SetShadingWireframeCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override { return vn::String(kShadingModeWireframeCommand); }
    vn::String description() const override { return u8"线框：只画三角面的边（看网格怎么连的）"; }

  protected:
    vn::graphics::SceneView::Options::Mode mode() const override { return vn::graphics::SceneView::Options::Mode::Wireframe; }
};

} // namespace vn::model_viewer
