#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/meshio/MeshExporter.hpp>
#include <vine/meshio/MeshLoader.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::geometry::ShapeType;
using vn::math::Vec3f;
using vn::meshio::MeshExporter;
using vn::meshio::MeshLoader;

namespace
{

/**
 * @brief Builds an indexed cube mesh with side length 2 centered at the origin.
 *
 * @return The cube mesh (12 triangles).
 */
vn::intrusive_ptr<IndexedTriangleMesh> makeCube()
{
    auto mesh = vn::intrusive_ptr<IndexedTriangleMesh>(new IndexedTriangleMesh());

    // 8 corners of a cube spanning [-1, 1]^3.
    const auto v0 = mesh->addVertex(Vec3f(-1, -1, -1));
    const auto v1 = mesh->addVertex(Vec3f(1, -1, -1));
    const auto v2 = mesh->addVertex(Vec3f(1, 1, -1));
    const auto v3 = mesh->addVertex(Vec3f(-1, 1, -1));
    const auto v4 = mesh->addVertex(Vec3f(-1, -1, 1));
    const auto v5 = mesh->addVertex(Vec3f(1, -1, 1));
    const auto v6 = mesh->addVertex(Vec3f(1, 1, 1));
    const auto v7 = mesh->addVertex(Vec3f(-1, 1, 1));

    // front (z = -1)
    mesh->addTriangle(v0, v1, v2);
    mesh->addTriangle(v0, v2, v3);
    // back (z = 1)
    mesh->addTriangle(v5, v4, v7);
    mesh->addTriangle(v5, v7, v6);
    // left (x = -1)
    mesh->addTriangle(v4, v0, v3);
    mesh->addTriangle(v4, v3, v7);
    // right (x = 1)
    mesh->addTriangle(v1, v5, v6);
    mesh->addTriangle(v1, v6, v2);
    // bottom (y = -1)
    mesh->addTriangle(v4, v5, v1);
    mesh->addTriangle(v4, v1, v0);
    // top (y = 1)
    mesh->addTriangle(v3, v2, v6);
    mesh->addTriangle(v3, v6, v7);

    return mesh;
}

/**
 * @brief Writes a minimal single-triangle binary STL file.
 *
 * @param path The output path.
 */
void writeTriangleStl(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary);
    char          header[80] = {};
    out.write(header, sizeof header);
    const std::uint32_t count = 1;
    out.write(reinterpret_cast<const char*>(&count), sizeof count);
    // Normal + 3 vertices of the triangle (0,0,0) (1,0,0) (0,1,0).
    float data[12] = { 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0 };
    out.write(reinterpret_cast<const char*>(data), sizeof data);
    const std::uint16_t attr = 0;
    out.write(reinterpret_cast<const char*>(&attr), sizeof attr);
}

} // namespace

TEST(MeshIoTest, IsSupportedFormat)
{
    EXPECT_TRUE(MeshLoader::isSupportedFormat("model.stl"));
    EXPECT_TRUE(MeshLoader::isSupportedFormat("model.obj"));
    EXPECT_TRUE(MeshLoader::isSupportedFormat("model.FBX"));
    EXPECT_TRUE(MeshLoader::isSupportedFormat("scene.gltf"));
    EXPECT_FALSE(MeshLoader::isSupportedFormat("model.txt"));
    EXPECT_FALSE(MeshLoader::isSupportedFormat("model"));
}

TEST(MeshIoTest, LoadMissingFileReturnsNull)
{
    MeshLoader loader;
    EXPECT_FALSE(loader.load(std::filesystem::temp_directory_path() / "vine_no_such_file.stl"));
}

TEST(MeshIoTest, LoadAndExportObjRoundTrip)
{
    const auto cube = makeCube();
    ASSERT_TRUE(cube->isValid());
    ASSERT_EQ(cube->triangleCount(), 12u);

    const auto path = std::filesystem::temp_directory_path() / "vine_meshio_cube.obj";

    MeshExporter exporter;
    exporter.options().scale_factor = 1.0;
    ASSERT_NO_THROW(exporter.exportAsObj(*cube, path));
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto loaded = loader.load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->shapeType(), ShapeType::IndexedTriangleMesh);

    const auto& itm = vn::obj_cast<IndexedTriangleMesh>(*loaded);
    EXPECT_EQ(itm.triangleCount(), cube->triangleCount());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, ExportAsciiStl)
{
    const auto cube = makeCube();
    const auto path = std::filesystem::temp_directory_path() / "vine_meshio_cube.stl";

    MeshExporter exporter;
    exporter.options().scale_factor = 1.0;
    exporter.options().format       = MeshExporter::Options::Format::Ascii;
    ASSERT_NO_THROW(exporter.exportAsStl(*cube, path));
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    std::filesystem::remove(path);
}

TEST(MeshIoTest, ExportInvalidMeshThrows)
{
    IndexedTriangleMesh empty;
    MeshExporter        exporter;
    EXPECT_THROW(exporter.exportAsObj(empty, "invalid.obj"), std::invalid_argument);
}

TEST(MeshIoTest, LoadCachesSameFile)
{
    const auto path = std::filesystem::temp_directory_path() / "vine_meshio_cache.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto first  = loader.load(path);
    const auto second = loader.load(path);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.get(), second.get());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, LoadDifferentOptionsNotCached)
{
    const auto path = std::filesystem::temp_directory_path() / "vine_meshio_opts.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto first = loader.load(path);

    loader.options().scale_mode         = MeshLoader::ScaleMode::Custom;
    loader.options().custom_scale_factor = 2.0;
    const auto second = loader.load(path);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.get(), second.get());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, CustomScaleMultipliesVertices)
{
    const auto path = std::filesystem::temp_directory_path() / "vine_meshio_scale.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode         = MeshLoader::ScaleMode::Custom;
    loader.options().custom_scale_factor = 3.0;
    const auto mesh = loader.load(path);
    ASSERT_TRUE(mesh);

    const auto& itm       = vn::obj_cast<IndexedTriangleMesh>(*mesh);
    const auto& positions = itm.positions();
    ASSERT_EQ(positions.size(), 3u);

    // The source vertex (1,0,0) must become (3,0,0).
    bool has_scaled = false;
    for (const auto& v : positions) {
        if (v.x == 3.0f && v.y == 0.0f && v.z == 0.0f) {
            has_scaled = true;
        }
    }
    EXPECT_TRUE(has_scaled);

    std::filesystem::remove(path);
}
