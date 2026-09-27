#pragma once

#include <cstddef>
#include <utility>

#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

namespace vn::model_viewer
{

/**
 * @brief 模型查看器的文档：一份已经解析好的网格，外加"它是谁"。
 *
 * 文档只拿**数据**，不碰渲染：视图（把文档变成屏幕上的东西）是文档的消费者，由宿主在窗口就绪之后建 —— 见
 * appfw-document-model.md §5.8 的分工。把载荷变成这个对象的活儿在打开器里（ModelViewerPlugin 登记的那两个）。
 *
 * 类型 id 是 `"model"`（kTypeId）：查看器**一种文档类型吃多种载荷**（内存网格 / 磁盘上的网格文件）。编辑器要
 * 把 mesh 与 b-rep 分成两个类型（操作逻辑不同），那只是登记方式不同 —— 框架不需要为这两种应用分叉。
 */
class ModelDocument : public vn::appfw::Document {
  public:
    /// 登记用的类型 id：registerType() 与 typeId() 必须说同一个，面板也按它认文档。
    static constexpr const char8_t* kTypeId = u8"model";

  public:
    /**
     * @brief 造一份模型文档。
     *
     * @param name 显示名（面板与将来的标签页用它；真实场景是文件名或包内路径）。
     * @param mesh 解析好的网格；可以为空（"空模型"，两个数都会是 0）。
     */
    ModelDocument(vn::String name, vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh> mesh)
      : name_(std::move(name))
      , mesh_(std::move(mesh))
    {
    }

  public:
    vn::String typeId() const override { return vn::String(kTypeId); }
    vn::String title() const override { return name_; }

    /// 查看器不改内容 ⇒ 永远不脏（编辑器那一档按自己的编辑栈实现它）。
    bool isDirty() const override { return false; }

  public:
    /// 顶点数（面板显示的第一个数）；没有网格时是 0。
    std::size_t vertexCount() const { return mesh_ != nullptr ? mesh_->vertexCount() : 0; }

    /// 三角形数（面板显示的第二个数）；没有网格时是 0。
    std::size_t triangleCount() const { return mesh_ != nullptr ? mesh_->triangleCount() : 0; }

    /**
     * @brief 返回文档持有的网格。
     *
     * @return 借用的网格，随文档一起消失；没有网格时是空指针。
     */
    vn::raw_ptr<const vn::geometry::IndexedTriangleMesh> mesh() const { return mesh_.get(); }

  private:
    vn::String                                                 name_;
    vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh> mesh_;
};

} // namespace vn::model_viewer
