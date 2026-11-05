#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include <vine/models/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>

namespace vn::model_viewer
{

/**
 * @brief 测试用的模型数据：两个内存网格 + 一个最小 STL 写盘。
 *
 * 这个插件是**测试用**的，所以数据自己带：仓库里没有随包发布的网格文件（`test_data/` 只有图像和机器人），
 * 也没有"从 Vfs 读网格"的通路（`MeshLoader::load()` / `loadMerged()` 只吃路径与流，没有 Vfs 入口，见 OpenMeshFileCommand）。
 * 等这两件里任意一件有了，把这里换成真文件即可 —— 打开器那一侧一行都不用动。
 */

/**
 * @brief 造一个顶点落在 [-1, 1] 的立方体。
 *
 * 8 个顶点、12 个三角形：面板上最容易一眼核对的两个数。
 *
 * @return 网格（永不为空）。
 */
inline vn::intrusive_ptr<vn::models::TriangleMesh> makeBoxMesh()
{
    const vn::models::Vec3fArray positions{
        vn::math::Vec3f(-1.0f, -1.0f, -1.0f),
        vn::math::Vec3f(1.0f, -1.0f, -1.0f),
        vn::math::Vec3f(1.0f, 1.0f, -1.0f),
        vn::math::Vec3f(-1.0f, 1.0f, -1.0f),
        vn::math::Vec3f(-1.0f, -1.0f, 1.0f),
        vn::math::Vec3f(1.0f, -1.0f, 1.0f),
        vn::math::Vec3f(1.0f, 1.0f, 1.0f),
        vn::math::Vec3f(-1.0f, 1.0f, 1.0f),
    };
    const vn::models::UInt32Array indices{
        0u, 1u, 2u, 0u, 2u, 3u, // 前
        5u, 4u, 7u, 5u, 7u, 6u, // 后
        4u, 0u, 3u, 4u, 3u, 7u, // 左
        1u, 5u, 6u, 1u, 6u, 2u, // 右
        4u, 5u, 1u, 4u, 1u, 0u, // 下
        3u, 2u, 6u, 3u, 6u, 7u, // 上
    };

    auto mesh = vn::make_intrusive<vn::models::TriangleMesh>();
    mesh->setPositions(positions);
    mesh->setIndices(indices);
    mesh->bumpRevision();
    (void)mesh->computeAabb();
    return mesh;
}

/**
 * @brief 造一块 `segments × segments` 的三角网格。
 *
 * 顶点数 = (segments+1)²，三角形数 = 2·segments² —— 随参数变，用来验证"面板跟着当前文档换"。
 *
 * @param segments 每边的格子数；0 当作 1。
 * @return 网格（永不为空）。
 */
inline vn::intrusive_ptr<vn::models::TriangleMesh> makeGridMesh(std::size_t segments)
{
    const std::size_t side = segments == 0 ? 1 : segments;

    vn::models::Vec3fArray  positions;
    vn::models::UInt32Array indices;
    positions.reserve((side + 1) * (side + 1));
    indices.reserve(side * side * 6u);

    std::vector<std::uint32_t> previous(side + 1, 0);
    std::vector<std::uint32_t> current(side + 1, 0);
    for (std::size_t z = 0; z <= side; ++z) {
        for (std::size_t x = 0; x <= side; ++x) {
            const auto fx = static_cast<float>(x) / static_cast<float>(side) * 2.0f - 1.0f;
            const auto fz = static_cast<float>(z) / static_cast<float>(side) * 2.0f - 1.0f;
            current[x]    = static_cast<std::uint32_t>(positions.size());
            positions.push_back(vn::math::Vec3f(fx, 0.0f, fz));
        }

        if (z > 0) {
            for (std::size_t x = 0; x < side; ++x) {
                const std::uint32_t a = previous[x];
                const std::uint32_t b = previous[x + 1];
                const std::uint32_t c = current[x + 1];
                const std::uint32_t d = current[x];
                indices.insert(indices.end(), { a, b, c, a, c, d });
            }
        }

        std::swap(previous, current);
    }

    auto mesh = vn::make_intrusive<vn::models::TriangleMesh>();
    mesh->setPositions(std::move(positions));
    mesh->setIndices(std::move(indices));
    mesh->bumpRevision();
    (void)mesh->computeAabb();
    return mesh;
}

/**
 * @brief 写一个最小二进制 STL（一个三角形）到磁盘。
 *
 * 和 meshio 用例写的是同一份数据：没有随包发布的网格文件，但"打开器真的调用格式加载器"这一段要能被真跑到。
 *
 * @param file_path 输出路径。
 */
inline void writeTriangleStlFile(const std::filesystem::path& file_path)
{
    std::ofstream out(file_path, std::ios::binary);
    char          header[80] = {};
    out.write(header, sizeof header);

    const std::uint32_t triangle_count = 1;
    out.write(reinterpret_cast<const char*>(&triangle_count), sizeof triangle_count);

    // 法线 + 三个顶点：(0,0,0) (1,0,0) (0,1,0)。
    const float data[12] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    out.write(reinterpret_cast<const char*>(data), sizeof data);

    const std::uint16_t attribute_byte_count = 0;
    out.write(reinterpret_cast<const char*>(&attribute_byte_count), sizeof attribute_byte_count);
}

} // namespace vn::model_viewer
