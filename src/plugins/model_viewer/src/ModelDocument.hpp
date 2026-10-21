#pragma once

#include <cstddef>
#include <utility>

#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/raw_ptr.hpp>

namespace vn::model_viewer
{

/**
 * @brief 模型查看器的文档：一份已经解析好的模型（节点树 + 网格 + 材质），外加"它是谁"。
 *
 * 文档只拿**数据**，不碰渲染：视图（把文档变成屏幕上的东西）是文档的消费者，由宿主在窗口就绪之后建 —— 见
 * appfw-document-model.md §5.8 的分工。把载荷变成这个对象的活儿在打开器里（ModelViewerPlugin 登记的那两个）。
 *
 * 文档持的是 `geometry::Model` 而不是一个摊平的网格：**结构是模型的一部分**（节点变换、多部件、每部件材质），
 * 摊平发生在视图那一侧（`sceneFromModel` 把节点树变成场景节点），文档不该先把信息丢掉再让视图去猜。内存载荷
 * （一个网格）由第二个构造函数**包成单节点模型**，于是两种载荷在文档里只有一种形状。
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
     * @brief 造一份模型文档（模型已经带结构）。
     *
     * @param name 显示名（面板与将来的标签页用它；真实场景是文件名或包内路径）。
     * @param model 解析好的模型；可以为空（"空模型"，三个数都会是 0）。
     */
    ModelDocument(vn::String name, vn::intrusive_ptr<vn::geometry::Model> model)
      : name_(std::move(name))
      , model_(std::move(model))
    {
        count(model_.get(), vertices_, triangles_, parts_);
    }

    /**
     * @brief 造一份只有**一个网格**的模型文档（内存载荷那条路）。
     *
     * 网格被包成一个单节点、单位变换、单个 entry 的 `geometry::Model`：文档对外只有"一个模型"这一种形状，
     * 视图因此不需要为两种载荷写两套。
     *
     * @param name 显示名。
     * @param mesh 解析好的网格；可以为空（空模型）。
     */
    ModelDocument(vn::String name, vn::intrusive_ptr<vn::geometry::IndexedTriangleMesh> mesh)
      : name_(std::move(name))
    {
        if (mesh != nullptr) {
            auto model = vn::make_intrusive<vn::geometry::Model>();
            vn::geometry::Model::Node node;
            node.name   = name_;
            node.meshes = { 0U };
            vn::geometry::Model::Entry entry;
            entry.mesh = std::move(mesh);
            model->setNodes({ std::move(node) });
            model->setMeshes({ std::move(entry) });
            model->setRoot(0U);
            model_ = std::move(model);
        }
        count(model_.get(), vertices_, triangles_, parts_);
    }

  public:
    vn::String typeId() const override { return vn::String(kTypeId); }
    vn::String title() const override { return name_; }

    /// 查看器不改内容 ⇒ 永远不脏（编辑器那一档按自己的编辑栈实现它）。
    bool isDirty() const override { return false; }

  public:
    /// 顶点数（面板显示的第一个数）：模型里各 entry 的顶点数之和；没有模型时是 0。
    std::size_t vertexCount() const { return vertices_; }

    /// 三角形数（面板显示的第二个数）：模型里各三角网格 entry 的三角形数之和；没有模型时是 0。
    std::size_t triangleCount() const { return triangles_; }

    /// 部件数（面板的第三个数）：模型里可画的 entry 个数（同一网格挂两个节点算一个部件 —— 它是一份数据）。
    std::size_t partCount() const { return parts_; }

    /**
     * @brief 返回文档持有的模型。
     *
     * @return 借用的模型，随文档一起消失；没有模型时是空指针。
     */
    vn::raw_ptr<const vn::geometry::Model> model() const { return model_.get(); }

  private:
    /// 数一遍模型的规模（按 entry 算，不按节点引用算：同一网格挂两个节点仍是一份数据）。
    static void count(const vn::geometry::Model* model, std::size_t& vertices, std::size_t& triangles,
                      std::size_t& parts)
    {
        vertices  = 0;
        triangles = 0;
        parts     = 0;
        if (model == nullptr) {
            return;
        }
        for (const vn::geometry::Model::Entry& entry : model->meshes()) {
            if (entry.mesh == nullptr) {
                continue;
            }
            ++parts;
            vertices += entry.mesh->vertexCount();
            if (const auto* indexed = dynamic_cast<const vn::geometry::IndexedTriangleMesh*>(entry.mesh.get())) {
                triangles += indexed->triangleCount();
            }
        }
    }

  private:
    vn::String                             name_;
    vn::intrusive_ptr<vn::geometry::Model> model_;
    std::size_t                            vertices_{ 0 };
    std::size_t                            triangles_{ 0 };
    std::size_t                            parts_{ 0 };
};

} // namespace vn::model_viewer
