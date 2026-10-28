#pragma once

#include <vine/String.hpp>
#include <vine/appfw/Command.hpp>
#include <vine/graphics/ShadingMode.hpp>

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
 * 模式本身是 `vn::graphics::ShadingMode`（graphics 那一层的东西，五档见那里），这里只负责"找到该改哪个视图"，
 * 再把这档交给视图**那条链**（`PassChain::setShadingMode()` —— 见下面的"改哪条链"）。
 *
 * **登记方式**：这五个类**不用** `VN_DECLARE_COMMAND` 的自动登记（那会把 `moduleCommandQueue()` 依赖带进每个
 * include 它的翻译单元，连带把"哪些命令属于本插件"这件事藏进静态初始化）—— 插件在自己的 `load()` 里按名字逐个
 * `registerCommand<T>()`：一处说得清、可读、用例也能照做（见 ModelViewerPlugin）。名字是下面那几个常量。
 *
 * **作用在哪个视图上**：**派发时解析出的目标**（`CommandExecutionContext::target()`）—— 目标是视图，视图
 * 自己回答它那块 3D 区（`DocumentView::renderControl()`），一块渲染区只画一幅画（多格已下线，见
 * `RenderSurface`）。命令因此不需要认识任何具体视图类，也不需要问“谁是当前窗口”：目标随执行走，两个窗口/两个
 * 窗格各答各的。前提用 `CommandFlags::NeedsView` 声明，缺 3D 区时由框架统一拒绝（不再在这里写空链判断）。
 *
 * 剩下的一道守卫是**类型**：
 *
 *  - 目标文档不是模型文档 ⇒ `NotApplicable`：演示文档的视图不该被模型查看器的命令改掉。
 *
 * **改哪条链**：视图的配方（哪条链、带不带坐标轴、带不带帧率读数）归 `PassChain`，模式则是链**已经建好之后**
 * 还能改的那一项。交给 `RenderControl::setShadingMode()`：控件知道这条链是不是它建的，能改就直接改（零重建），
 * 改不了就带着新档重建一次（延迟链没有可改的几何趟，重建时按实面那条前置链搭）。“怎么重建”是那块区的主人的事，
 * 本命令不替它做决定。
 *
 * **模式归视图、不归文档**：每份文档自带视图，所以换一份文档时它以默认（实面）出现。想让它跟着文档走就得把模式
 * 存进文档 —— 那是编辑器那一档的事（文档只拿数据，见 ModelDocument 的说明）。
 */
class ShadingModeCommand : public vn::appfw::Command {
  public:
    vn::String group() const override
    {
        return u8"显示";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::NeedsView;
    }

    /// 执行：设目标视图那条链的着色模式（见类注释里的守卫与"改哪条链"）。
    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override;

  protected:
    /// 这条命令设的模式。
    virtual vn::graphics::ShadingMode mode() const = 0;
};

/// 实面：完整光照（默认档）。
class SetShadingLitCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return vn::String(kShadingModeLitCommand);
    }

    vn::String description() const override
    {
        return u8"实面：环境光 + 主光 + 补光（默认）";
    }

  protected:
    vn::graphics::ShadingMode mode() const override
    {
        return vn::graphics::ShadingMode::Lit;
    }
};

/// 平面：同样的光照，法线取三角面（没有逐顶点法线的数据也看得出形状）。
class SetShadingFlatCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return vn::String(kShadingModeFlatCommand);
    }

    vn::String description() const override
    {
        return u8"平面：逐面法线（同一个三角形的每个像素一个朝向）";
    }

  protected:
    vn::graphics::ShadingMode mode() const override
    {
        return vn::graphics::ShadingMode::Flat;
    }
};

/// 无光照：只画表面色（材质 / 贴图 / 顶点色），没有任何光照项。
class SetShadingUnlitCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return vn::String(kShadingModeUnlitCommand);
    }

    vn::String description() const override
    {
        return u8"无光照：只画表面色 —— 分清是形状不对还是光照不对";
    }

  protected:
    vn::graphics::ShadingMode mode() const override
    {
        return vn::graphics::ShadingMode::Unlit;
    }
};

/// 法线：把视图空间法线当颜色画（蓝 = 朝着相机）。
class SetShadingNormalsCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return vn::String(kShadingModeNormalsCommand);
    }

    vn::String description() const override
    {
        return u8"法线：视图空间法线当颜色（看朝向、缺失或内外翻的法线）";
    }

  protected:
    vn::graphics::ShadingMode mode() const override
    {
        return vn::graphics::ShadingMode::Normals;
    }
};

/// 线框：只画三角面的边，用无光照的表面色。
class SetShadingWireframeCommand : public ShadingModeCommand {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return vn::String(kShadingModeWireframeCommand);
    }

    vn::String description() const override
    {
        return u8"线框：只画三角面的边（看网格怎么连的）";
    }

  protected:
    vn::graphics::ShadingMode mode() const override
    {
        return vn::graphics::ShadingMode::Wireframe;
    }
};

} // namespace vn::model_viewer
