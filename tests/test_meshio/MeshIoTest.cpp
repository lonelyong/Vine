#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>
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
    EXPECT_FALSE(loader.loadMerged(std::filesystem::temp_directory_path() / "vn_no_such_file.stl"));
}

TEST(MeshIoTest, LoadAndExportObjRoundTrip)
{
    const auto cube = makeCube();
    ASSERT_TRUE(cube->isValid());
    ASSERT_EQ(cube->triangleCount(), 12u);

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cube.obj";

    MeshExporter exporter;
    exporter.options().scale_factor = 1.0;
    ASSERT_NO_THROW(exporter.exportAs(*cube, u8"obj", path));
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto loaded = loader.loadMerged(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->shapeType(), ShapeType::IndexedTriangleMesh);

    const auto& itm = vn::obj_cast<IndexedTriangleMesh>(*loaded);
    EXPECT_EQ(itm.triangleCount(), cube->triangleCount());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, VertexColorsSurviveAPlyRoundTrip)
{
    // PLY is the format family that carries per-vertex colours end to end, so it is what proves both directions.
    auto cube = makeCube();
    cube->setColors({ vn::math::Vec4f(1.0f, 0.0f, 0.0f, 1.0f), vn::math::Vec4f(0.0f, 1.0f, 0.0f, 1.0f),
                      vn::math::Vec4f(0.0f, 0.0f, 1.0f, 1.0f), vn::math::Vec4f(1.0f, 1.0f, 0.0f, 1.0f),
                      vn::math::Vec4f(1.0f, 0.0f, 1.0f, 1.0f), vn::math::Vec4f(0.0f, 1.0f, 1.0f, 1.0f),
                      vn::math::Vec4f(1.0f, 1.0f, 1.0f, 1.0f), vn::math::Vec4f(0.0f, 0.0f, 0.0f, 1.0f) });

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_colors.ply";

    MeshExporter exporter;
    exporter.options().scale_factor = 1.0;
    ASSERT_NO_THROW(exporter.exportAs(*cube, u8"ply", path));

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;

    // The merged entry keeps the colours because every source mesh authored them. The importer may split and
    // reorder vertices (a generated facet normal cannot be shared around a corner), so the invariants are
    // "one colour per vertex" and "the same set of colours", not the source order.
    const auto merged = loader.loadMerged(path);
    ASSERT_TRUE(merged);
    ASSERT_FALSE(merged->colors().empty());
    EXPECT_EQ(merged->colors().size(), merged->positions().size());
    for (const vn::math::Vec4f& expected : cube->colors()) {
        EXPECT_TRUE(std::any_of(merged->colors().begin(), merged->colors().end(), [&](const vn::math::Vec4f& got) {
            return got.x == expected.x && got.y == expected.y && got.z == expected.z;
        })) << "a colour the mesh authored is missing from the round trip";
    }

    // The model entry reads the same file through the other path (makeMesh), so both entries agree.
    const auto model = loader.load(path);
    ASSERT_TRUE(model);
    ASSERT_EQ(model->meshes().size(), 1u);
    const auto& entry_mesh = model->meshes().front().mesh;
    ASSERT_NE(entry_mesh.get(), nullptr);
    ASSERT_FALSE(entry_mesh->colors().empty());
    EXPECT_EQ(entry_mesh->colors().size(), entry_mesh->positions().size());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, MergedEntryPadsMeshesThatAuthorNoColourWithWhite)
{
    // One file, two meshes - and only the first one coloured. The merged entry has ONE vertex list, so the mesh that
    // authors no colour contributes OPAQUE WHITE: the multiplicative identity the shading applies, which is exactly
    // what "no colour channel" means, so each part looks the way it would on its own (see MergedArrays).
    auto coloured_cube = makeCube();
    coloured_cube->setColors({ vn::math::Vec4f(0.0f, 0.0f, 0.0f, 1.0f), vn::math::Vec4f(0.5f, 0.0f, 0.0f, 1.0f),
                               vn::math::Vec4f(0.0f, 0.5f, 0.0f, 1.0f), vn::math::Vec4f(0.0f, 0.0f, 0.5f, 1.0f),
                               vn::math::Vec4f(0.5f, 0.5f, 0.0f, 1.0f), vn::math::Vec4f(0.5f, 0.0f, 0.5f, 1.0f),
                               vn::math::Vec4f(0.0f, 0.5f, 0.5f, 1.0f), vn::math::Vec4f(0.5f, 0.5f, 0.5f, 1.0f) });
    auto plain_cube = makeCube();

    auto model = vn::make_intrusive<vn::geometry::Model>();

    vn::geometry::Model::Entry first;
    first.mesh     = coloured_cube;
    first.material = vn::make_intrusive<vn::geometry::ColorMaterial>(vn::Colorf(1.0f, 0.0f, 0.0f));
    vn::geometry::Model::Entry second;
    second.mesh     = plain_cube;
    second.material = vn::make_intrusive<vn::geometry::ColorMaterial>(vn::Colorf(0.0f, 1.0f, 0.0f));
    model->setMeshes({ first, second });

    vn::geometry::Model::Node root;
    root.meshes = { 0u, 1u };
    model->setNodes({ root });
    model->setRoot(0u);

    MeshExporter exporter;
    MeshLoader   loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;

    const auto load_merged = [&](const char* name) {
        const auto path = std::filesystem::temp_directory_path() / name;
        EXPECT_NO_THROW(exporter.exportAs(*model, u8"gltf2", path));
        const auto merged = loader.loadMerged(path);
        std::filesystem::remove(path);
        return merged;
    };
    const auto is_white = [](const vn::math::Vec4f& colour) {
        return colour.x == 1.0f && colour.y == 1.0f && colour.z == 1.0f;
    };

    // Mixed: the coloured mesh keeps its colours, the plain one is padded with white.
    const auto mixed = load_merged("vn_meshio_mixed_colors.gltf");
    ASSERT_TRUE(mixed);
    ASSERT_EQ(mixed->colors().size(), mixed->positions().size()) << "one colour per vertex, padded or authored";
    const std::size_t white = static_cast<std::size_t>(std::count_if(mixed->colors().begin(), mixed->colors().end(),
                                                                     is_white));
    EXPECT_GT(white, 0u) << "the mesh that authors no colour carries white";
    EXPECT_LT(white, mixed->colors().size()) << "the colours the other mesh authored survived the merge";

    // Both coloured: nothing is padded, so no vertex is white.
    plain_cube->setColors(vn::geometry::Vec4fArray(coloured_cube->colors().begin(), coloured_cube->colors().end()));
    const auto both = load_merged("vn_meshio_all_colors.gltf");
    ASSERT_TRUE(both);
    ASSERT_EQ(both->colors().size(), both->positions().size());
    EXPECT_TRUE(std::none_of(both->colors().begin(), both->colors().end(), is_white))
        << "an unauthored vertex is the only reason a white one exists";

    // Neither coloured: the array is absent, not a white one.
    coloured_cube->setColors({});
    plain_cube->setColors({});
    const auto none = load_merged("vn_meshio_no_colors.gltf");
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->colors().empty()) << "\"no mesh authors a colour\" stays \"no colour channel\"";
}

TEST(MeshIoTest, ExportAsciiStl)
{
    const auto cube = makeCube();
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cube.stl";

    MeshExporter exporter;
    exporter.options().scale_factor = 1.0;
    exporter.options().format       = MeshExporter::Options::Format::Ascii;
    ASSERT_NO_THROW(exporter.exportAs(*cube, u8"stl", path));
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    std::filesystem::remove(path);
}

TEST(MeshIoTest, ExportInvalidMeshThrows)
{
    IndexedTriangleMesh empty;
    MeshExporter        exporter;
    EXPECT_THROW(exporter.exportAs(empty, u8"obj", "invalid.obj"), std::invalid_argument);
}

TEST(MeshIoTest, LoadCachesSameFile)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cache.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto first  = loader.loadMerged(path);
    const auto second = loader.loadMerged(path);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.get(), second.get());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, LoadDifferentOptionsNotCached)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_opts.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto first = loader.loadMerged(path);

    loader.options().scale_mode         = MeshLoader::ScaleMode::Custom;
    loader.options().custom_scale_factor = 2.0;
    const auto second = loader.loadMerged(path);

    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.get(), second.get());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, CustomScaleMultipliesVertices)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_scale.stl";
    writeTriangleStl(path);

    MeshLoader loader;
    loader.options().scale_mode         = MeshLoader::ScaleMode::Custom;
    loader.options().custom_scale_factor = 3.0;
    const auto mesh = loader.loadMerged(path);
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

TEST(MeshIoTest, ExportAsWritesTheNamedFormat)
{
    const auto cube = makeCube();
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_export_as.stl";

    MeshExporter exporter;
    exporter.options().format = MeshExporter::Options::Format::Ascii;
    ASSERT_NO_THROW(exporter.exportAs(*cube, u8"stl", path)) << "the family name picks the text variant from the options";
    ASSERT_TRUE(std::filesystem::exists(path));

    MeshLoader loader;
    loader.options().scale_mode = MeshLoader::ScaleMode::Disabled;
    const auto loaded = loader.loadMerged(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(vn::obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), cube->triangleCount());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, ExportAsRefusesAnUnknownFormat)
{
    MeshExporter exporter;
    EXPECT_THROW(exporter.exportAs(*makeCube(), u8"not_a_format", std::filesystem::temp_directory_path() / "vn_unknown.bin"),
                 std::runtime_error);
}
