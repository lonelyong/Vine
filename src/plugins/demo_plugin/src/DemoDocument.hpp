#pragma once

#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>

namespace vn::demo
{

/**
 * @brief 演示场景的文档。
 *
 * 它就是"3D 内容也是一个文档"这条的活例子（appfw-document-model.md §12）：演示不再由外壳在启动时无条件建出来，
 * 而是一份**普通文档** —— 有类型 id、有 create 工厂（所以文档管理器面板会如实显示"可以新建"）、由命令打开、
 * 由视图渲染。
 *
 * 它没有数据、没有来源、永远不会脏：内容是固定的演示场景，视图（`DemoView`）自带渲染面把骨架搭起来。
 */
class DemoDocument final : public vn::appfw::Document
{
  public:
    /// 文档类型 id（登记与查找都用它）。
    static constexpr const char8_t* kTypeId = u8"demo";

  public:
    /// @return 类型 id。
    vn::String typeId() const override { return kTypeId; }

    /// @return 固定标题（演示场景没有名字可言）。
    vn::String title() const override { return u8"演示场景"; }

    /// @return false：演示场景没有可改的东西。
    bool isDirty() const override { return false; }
};

} // namespace vn::demo
