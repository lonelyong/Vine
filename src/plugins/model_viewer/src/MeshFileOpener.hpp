#pragma once

#include <filesystem>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/meshio/MeshLoader.hpp>

#include "ModelDocument.hpp"
#include "ModelPayload.hpp"

namespace vn::model_viewer
{

/**
 * @brief 查看器读模型文件时用的加载选项。
 *
 * `ScaleMode::Disabled` 是刻意的：查看器只呈现**文件里写的坐标**，不做单位换算。加载器默认的 `Auto`（按 AABB 判
 * 来源单位、再换算到毫米）是给机器人包用的 —— 拿它看模型会出现"内存载荷是边长 2 的盒子、文件载荷是边长 1000 的
 * 三角形"这类怪事：相机、轨道操纵器与轴指示器都按各自的尺度摆位，两份文档看起来不像同一套东西。
 *
 * @return 选项。
 */
inline vn::meshio::MeshLoader::Options viewerLoadOptions()
{
    vn::meshio::MeshLoader::Options options;
    options.scale_mode = vn::meshio::MeshLoader::ScaleMode::Disabled;
    return options;
}

/**
 * @brief 把一个"磁盘上的网格文件"载荷读成模型文档。
 *
 * 这里是**打开器**：怎么读由它决定（见 appfw-document-model.md §5.1），谁造载荷谁只需要说清位置。它和内存载荷的
 * 打开器是同一形状，区别只有一处 —— 这个真的去调格式加载器。
 *
 * 用**本地的**加载器实例，而不是 `MeshLoader::defaultInstance()`：单例的 options 是共享可变状态，为了查看器的
 * 尺度去改它会波及机器人那条管线；本地实例顺带让两次并发打开不抢同一个加载器（`MeshLoader` 的方法不线程安全）。
 *
 * @param payload 载荷：要读的文件路径。
 * @return 新文档（调用方拥有）；读不出来、或读出来不是三角网格时是空指针。
 */
inline vn::appfw::Document* openMeshFilePayload(const MeshFilePayload& payload)
{
    if (payload.file_path.empty()) {
        return nullptr;
    }

    vn::meshio::MeshLoader loader;
    loader.setOptions(viewerLoadOptions());

    auto mesh = loader.load(payload.file_path);
    if (mesh == nullptr) {
        VN_LOGW("model_viewer: could not read '{}'", payload.file_path.string());
        return nullptr;
    }

    // 查看器只认三角形网格：三角形数是它才有的概念（顶点数是 Mesh 级的，任何网格都有）。
    const auto* indexed = vn::obj_cast<vn::geometry::IndexedTriangleMesh>(mesh.get());
    if (indexed == nullptr) {
        VN_LOGW("model_viewer: '{}' is not an indexed triangle mesh", payload.file_path.string());
        return nullptr;
    }

    // 名字取文件名：位置是载荷给的，怎么显示是文档的事（路径本身不进文档 —— 要重读就自己拷一份，见 §5.8）。
    return new ModelDocument(vn::String::fromLocal8Bit(payload.file_path.filename().string().c_str()),
                             vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh>(indexed));
}

} // namespace vn::model_viewer
