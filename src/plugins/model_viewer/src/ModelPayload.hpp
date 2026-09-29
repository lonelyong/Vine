#pragma once

#include <filesystem>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>

namespace vn::model_viewer
{

/**
 * @brief 载荷一：**内存里已经解析好**的网格，外加一个给人看的名字。
 *
 * 载荷必须派生 `Object` 并且带自己的元数据（`VN_OBJECT_META_DECL` + 恰好一处 `VN_OBJECT_META_IMPL`）：它的 `Type`
 * 就是打开器的登记键（见 appfw-document-model.md §5.7）。框架既不解释载荷的内容，也不拷贝它 —— 载荷只在 `open()`
 * 调用期间被借用，要留下来的东西由造载荷的人或文档类型自己拷。
 */
struct MeshPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    /// 显示名（面板与标题用它）。
    vn::String name;

    /// 解析好的网格；空表示"这份载荷里没有模型"，打开器会拒绝它。
    vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh> mesh;
};

/**
 * @brief 载荷二：磁盘上的一个网格**文件**，由打开器去读。
 *
 * 这就是"查看器一种类型吃多种载荷"：两个载荷类型、两个打开器、同一个文档类型。和载荷一放在一起，是为了让
 * "谁解析"这件事看得见 —— **打开器**解析（这里它调 `MeshLoader`），而层上层（谁造载荷）只需要说清楚东西在哪。
 *
 * @note 位置写成 `std::filesystem::path`：这条载荷表达的就是"磁盘上的一个文件"。**从 Vfs 读模型现在是通的** ——
 *       `MeshLoader::load(std::istream&, format_hint)` 早就有，`loadModel(std::istream&, format_hint)` 也已落地，
 *       用 `vn::io::DataSourceStream` 包一个 `VfsEntrySource` 就能读包里的模型；那需要的是**另一条载荷**（带
 *       Vfs 或字节），不是改这条（见 appfw-document-model.md §5 与 §12）。
 */
struct MeshFilePayload : public vn::Object {
    VN_OBJECT_META_DECL;

    /// 要读的模型文件路径。
    std::filesystem::path file_path;
};

} // namespace vn::model_viewer
