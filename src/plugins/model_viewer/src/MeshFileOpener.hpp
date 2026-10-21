#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Mesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/logging/Log.hpp>
#include <vine/math/Vector3.hpp>
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
 * @brief 按三角面累加顶点法线并归一化（索引不变，因此是平滑法线）。
 *
 * @param positions 顶点位置（与索引空间同序）。
 * @param indices   三角索引（三个一组）。
 * @return 与 positions 同长的法线。
 */
inline std::vector<vn::math::Vec3f> smoothNormals(std::span<const vn::math::Vec3f> positions,
                                                  std::span<const std::uint32_t>  indices)
{
    std::vector<vn::math::Vec3f> normals(positions.size(), vn::math::Vec3f(0.0f, 0.0f, 0.0f));

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t i0 = indices[i + 0];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size()) {
            continue; // 越界索引：跳过这个面，而不是读出去。
        }

        const auto face = (positions[i1] - positions[i0]).cross(positions[i2] - positions[i0]);
        // 面积加权（叉积的长度就是两倍面积）：大面对法线的贡献更大，退化面贡献为零。
        normals[i0] += face;
        normals[i1] += face;
        normals[i2] += face;
    }

    for (auto& normal : normals) {
        const float length =
            std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (length > 1.0e-12f) {
            normal = normal * (1.0f / length);
        }
        else {
            normal = vn::math::Vec3f(0.0f, 0.0f, 1.0f); // 孤立顶点：给一个确定的朝向，别留零向量。
        }
    }
    return normals;
}

/**
 * @brief 给缺法线的网格补一套平滑法线（STL 这类来源只有面法线）。
 *
 * STL 只有面法线、没有顶点法线；前向内容程序要读法线，缺了就画成一片死黑。平滑（而不是按面复制顶点）是刻意
 * 的选择：它不改索引，因此不改变面板上那两个数（顶点数/三角形数）—— 视图怎么画不该让文档的几何事实变样。
 *
 * @param model 就地补齐的模型（来源带了法线的网格不动）。
 */
inline void ensureSmoothNormals(vn::geometry::Model& model)
{
    for (const vn::geometry::Model::Entry& entry : model.meshes()) {
        vn::geometry::Mesh* mesh = entry.mesh.get();
        if (mesh == nullptr) {
            continue;
        }
        const std::span<const vn::math::Vec3f> positions = mesh->positions();
        if (positions.empty() || mesh->normals().size() == positions.size()) {
            continue; // 没有顶点，或来源已经带了（长度对得上才算带了）
        }
        const auto* indexed = dynamic_cast<const vn::geometry::IndexedTriangleMesh*>(mesh);
        if (indexed == nullptr || indexed->indices().size() < 3) {
            continue; // 非三角网格没有面可算
        }
        mesh->setNormals(smoothNormals(positions, indexed->indices()));
    }
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

    // 结构要留下来（节点变换、多部件、每部件材质）：`loadModel` 而不是摊平的 `load`。
    auto model = loader.loadModel(payload.file_path);
    if (model == nullptr) {
        VN_LOGW("model_viewer: could not read '{}'", payload.file_path.string());
        return nullptr;
    }

    // 查看器只画三角网格：一个都没有的模型（点云、纯 b-rep）不接受 —— 与以前"不是三角网格就拒"同一条规矩。
    std::size_t triangle_entries = 0;
    for (const vn::geometry::Model::Entry& entry : model->meshes()) {
        if (entry.mesh != nullptr &&
            dynamic_cast<const vn::geometry::IndexedTriangleMesh*>(entry.mesh.get()) != nullptr) {
            ++triangle_entries;
        }
    }
    if (triangle_entries == 0) {
        VN_LOGW("model_viewer: '{}' holds no triangle mesh", payload.file_path.string());
        return nullptr;
    }

    ensureSmoothNormals(*model);

    // 名字取文件名：位置是载荷给的，怎么显示是文档的事（路径本身不进文档 —— 要重读就自己拷一份，见 §5.8）。
    return new ModelDocument(vn::String::fromLocal8Bit(payload.file_path.filename().string().c_str()),
                             std::move(model));
}

} // namespace vn::model_viewer
