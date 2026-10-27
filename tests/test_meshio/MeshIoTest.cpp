#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/io/IoError.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>
#include <vine/meshio/MeshExporter.hpp>
#include <vine/meshio/MeshFormat.hpp>
#include <vine/meshio/MeshLoader.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::geometry::ShapeType;
using vn::math::Vec3f;
using vn::math::Vec4f;
using vn::meshio::formatFromPath;
using vn::meshio::formatInfo;
using vn::meshio::formatName;
using vn::meshio::MeshExporter;
using vn::meshio::MeshFormat;
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

/// Scale-disabled options, so a round trip compares the geometry the file holds.
MeshLoader::Options noScaling()
{
    return MeshLoader::Options{ .scale_mode = MeshLoader::ScaleMode::Disabled };
}

} // namespace

TEST(MeshIoTest, FormatVocabularyIsTheReadContract)
{
    // The enum and the extension table are one list: what formatFromPath() recognises is what the loader accepts and
    // what a file picker is handed, so the two can never disagree about a format again.
    EXPECT_EQ(formatFromPath("model.stl"), MeshFormat::Stl);
    EXPECT_EQ(formatFromPath("model.obj"), MeshFormat::Obj);
    EXPECT_EQ(formatFromPath("model.FBX"), MeshFormat::Fbx);
    EXPECT_EQ(formatFromPath("scene.gltf"), MeshFormat::Gltf);
    EXPECT_EQ(formatFromPath("scene.glb"), MeshFormat::Glb);
    EXPECT_EQ(formatFromPath("scene.dae"), MeshFormat::Collada);
    EXPECT_EQ(formatFromPath("part.3mf"), MeshFormat::ThreeMf);
    EXPECT_EQ(formatFromPath("model.txt"), MeshFormat::Unknown);
    EXPECT_EQ(formatFromPath("model"), MeshFormat::Unknown) << "no extension names no format";

    for (const vn::String& extension : MeshLoader::supportedExtensions()) {
        const auto format = formatFromPath(std::filesystem::path(std::string("x") + extension.as_std_str()));
        EXPECT_NE(format, MeshFormat::Unknown) << extension.as_std_str();
        EXPECT_EQ(std::string(formatInfo(format).extension), extension.as_std_str())
            << "the table row and the picker entry are the same spelling";
    }
    EXPECT_EQ(MeshLoader::supportedExtensions().size(), 8u);

    EXPECT_STREQ(formatName(MeshFormat::Collada), "collada");
    EXPECT_STREQ(formatName(MeshFormat::Unknown), "Unknown");
    EXPECT_TRUE(formatInfo(MeshFormat::Stl).has_encoding) << "STL has a binary and a text spelling";
    EXPECT_FALSE(formatInfo(MeshFormat::Obj).has_encoding);
    EXPECT_FALSE(formatInfo(MeshFormat::Gltf).streamable) << "glTF's main form needs a companion .bin";
    EXPECT_FALSE(formatInfo(MeshFormat::Unknown).streamable);
}

TEST(MeshIoTest, DaeAndColladaAreOneFormat)
{
    // The old API accepted "collada" but refused "dae" for the same file; the enum makes both spellings one value.
    EXPECT_EQ(formatFromPath("thing.dae"), MeshFormat::Collada);

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_thing.dae";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*makeCube(), MeshFormat::Collada, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       by_path = loader.loadMerged(path);
    ASSERT_TRUE(by_path);

    std::filesystem::remove(path);
}

TEST(MeshIoTest, TheExtensionTableNamesWhatTheLoaderActuallyReads)
{
    const auto   path = std::filesystem::temp_directory_path() / "vn_meshio_supported.ply";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*makeCube(), MeshFormat::Ply, path), vn::io::IoError::Ok);
    EXPECT_NE(formatFromPath(path), MeshFormat::Unknown) << "a format this module writes is a format it reads";

    MeshLoader loader(noScaling());
    EXPECT_TRUE(loader.loadMerged(path)) << "the table admits a format the loader can open";

    std::filesystem::remove(path);
}

TEST(MeshIoTest, LoadMissingFileIsNotFound)
{
    MeshLoader loader;
    auto       missing = loader.loadMerged(std::filesystem::temp_directory_path() / "vn_no_such_file.stl");
    EXPECT_FALSE(missing);
    EXPECT_EQ(missing.error(), vn::io::IoError::NotFound)
        << "a path that is not there is not the same failure as a broken file";
}

TEST(MeshIoTest, LoadSaysWhyItFailed)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_garbage.stl";
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a mesh";
    }

    MeshLoader loader;
    auto       broken = loader.loadMerged(path);
    EXPECT_FALSE(broken);
    EXPECT_EQ(broken.error(), vn::io::IoError::InvalidData) << "a file that is not geometry is its own failure";

    // A stream entry answers the same way, and an extension the table does not name is refused before any read.
    std::istringstream garbage("nope");
    EXPECT_EQ(loader.load(garbage, MeshFormat::Stl).error(), vn::io::IoError::InvalidData);

    const auto unknown = std::filesystem::temp_directory_path() / "vn_meshio_model.txt";
    EXPECT_EQ(loader.loadMerged(unknown).error(), vn::io::IoError::Unsupported)
        << "an unnamed extension is not attempted at all";

    const auto good = std::filesystem::temp_directory_path() / "vn_meshio_good.stl";
    writeTriangleStl(good);
    EXPECT_TRUE(loader.loadMerged(good));

    std::filesystem::remove(path);
    std::filesystem::remove(good);
}

TEST(MeshIoTest, LoadAndExportObjRoundTrip)
{
    const auto cube = makeCube();
    ASSERT_TRUE(cube->isValid());
    ASSERT_EQ(cube->triangleCount(), 12u);

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cube.obj";

    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Obj, path), vn::io::IoError::Ok);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    MeshLoader loader(noScaling());
    auto       loaded = loader.loadMerged(path);
    ASSERT_TRUE(loaded);

    const auto& itm = vn::obj_cast<IndexedTriangleMesh>(*loaded.value());
    EXPECT_EQ(itm.triangleCount(), cube->triangleCount());
    EXPECT_EQ(itm.shapeType(), ShapeType::IndexedTriangleMesh);

    std::filesystem::remove(path);
}

TEST(MeshIoTest, VertexColorsSurviveAPlyRoundTrip)
{
    // PLY is the format family that carries per-vertex colours end to end, so it is what proves both directions.
    auto cube = makeCube();
    cube->setColors({ Vec4f(1.0f, 0.0f, 0.0f, 1.0f), Vec4f(0.0f, 1.0f, 0.0f, 1.0f), Vec4f(0.0f, 0.0f, 1.0f, 1.0f),
                      Vec4f(1.0f, 1.0f, 0.0f, 1.0f), Vec4f(1.0f, 0.0f, 1.0f, 1.0f), Vec4f(0.0f, 1.0f, 1.0f, 1.0f),
                      Vec4f(1.0f, 1.0f, 1.0f, 1.0f), Vec4f(0.0f, 0.0f, 0.0f, 1.0f) });

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_colors.ply";

    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Ply, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());

    // The importer may split and reorder vertices (a generated facet normal cannot be shared around a corner), so the
    // invariants are "one colour per vertex" and "the same set of colours", not the source order.
    auto merged = loader.loadMerged(path);
    ASSERT_TRUE(merged);
    const auto& merged_mesh = *merged.value();
    ASSERT_FALSE(merged_mesh.colors().empty());
    EXPECT_EQ(merged_mesh.colors().size(), merged_mesh.positions().size());
    for (const Vec4f& expected : cube->colors()) {
        EXPECT_TRUE(std::any_of(merged_mesh.colors().begin(), merged_mesh.colors().end(), [&](const Vec4f& got) {
            return got.x == expected.x && got.y == expected.y && got.z == expected.z;
        })) << "a colour the mesh authored is missing from the round trip";
    }

    // The model entry reads the same file through the other path, so both entries agree.
    auto model = loader.load(path);
    ASSERT_TRUE(model);
    ASSERT_EQ(model.value()->meshes().size(), 1u);
    const auto& entry_mesh = model.value()->meshes().front().mesh;
    ASSERT_NE(entry_mesh.get(), nullptr);
    ASSERT_FALSE(entry_mesh->colors().empty());
    EXPECT_EQ(entry_mesh->colors().size(), entry_mesh->positions().size());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, MergedEntryPadsMeshesThatAuthorNoColourWithWhite)
{
    // One file, two meshes - and only the first one coloured. The merged entry has ONE vertex list, so the mesh that
    // authors no colour contributes OPAQUE WHITE: the multiplicative identity the shading applies.
    auto coloured_cube = makeCube();
    coloured_cube->setColors({ Vec4f(0.0f, 0.0f, 0.0f, 1.0f), Vec4f(0.5f, 0.0f, 0.0f, 1.0f),
                               Vec4f(0.0f, 0.5f, 0.0f, 1.0f), Vec4f(0.0f, 0.0f, 0.5f, 1.0f),
                               Vec4f(0.5f, 0.5f, 0.0f, 1.0f), Vec4f(0.5f, 0.0f, 0.5f, 1.0f),
                               Vec4f(0.0f, 0.5f, 0.5f, 1.0f), Vec4f(0.5f, 0.5f, 0.5f, 1.0f) });
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
    MeshLoader   loader(noScaling());

    const auto load_merged = [&](const char* name) {
        const auto path = std::filesystem::temp_directory_path() / name;
        EXPECT_EQ(exporter.exportAs(*model, MeshFormat::Glb, path), vn::io::IoError::Ok);
        auto merged = loader.loadMerged(path);
        std::filesystem::remove(path);
        return merged;
    };
    const auto is_white = [](const Vec4f& colour) {
        return colour.x == 1.0f && colour.y == 1.0f && colour.z == 1.0f;
    };

    // Mixed: the coloured mesh keeps its colours, the plain one is padded with white.
    auto mixed = load_merged("vn_meshio_mixed_colors.glb");
    ASSERT_TRUE(mixed);
    const auto& mixed_mesh = *mixed.value();
    ASSERT_EQ(mixed_mesh.colors().size(), mixed_mesh.positions().size()) << "one colour per vertex, padded or authored";
    const std::size_t white = static_cast<std::size_t>(
        std::count_if(mixed_mesh.colors().begin(), mixed_mesh.colors().end(), is_white));
    EXPECT_GT(white, 0u) << "the mesh that authors no colour carries white";
    EXPECT_LT(white, mixed_mesh.colors().size()) << "the colours the other mesh authored survived the merge";

    // Both coloured: nothing is padded, so no vertex is white.
    plain_cube->setColors(vn::geometry::Vec4fArray(coloured_cube->colors().begin(), coloured_cube->colors().end()));
    auto both = load_merged("vn_meshio_all_colors.glb");
    ASSERT_TRUE(both);
    EXPECT_TRUE(std::none_of(both.value()->colors().begin(), both.value()->colors().end(), is_white))
        << "an unauthored vertex is the only reason a white one exists";

    // Neither coloured: the array is absent, not a white one.
    coloured_cube->setColors({});
    plain_cube->setColors({});
    auto none = load_merged("vn_meshio_no_colors.glb");
    ASSERT_TRUE(none);
    EXPECT_TRUE(none.value()->colors().empty()) << "\"no mesh authors a colour\" stays \"no colour channel\"";
}

TEST(MeshIoTest, ExportAsciiStl)
{
    const auto cube = makeCube();
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cube_ascii.stl";

    MeshExporter exporter(MeshExporter::Options{ .scale_factor = 1.0,
                                                 .encoding     = MeshExporter::Encoding::Text });
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Stl, path), vn::io::IoError::Ok);
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_GT(std::filesystem::file_size(path), 0u);

    {
        std::ifstream in(path);
        std::string   first_word;
        in >> first_word;
        EXPECT_EQ(first_word, "solid") << "the text variant, not the binary one";
    }

    std::filesystem::remove(path);
}

TEST(MeshIoTest, ExportInvalidMeshIsInvalidData)
{
    IndexedTriangleMesh empty;
    MeshExporter        exporter;
    EXPECT_EQ(exporter.exportAs(empty, MeshFormat::Obj, "invalid.obj"), vn::io::IoError::InvalidData);
}

TEST(MeshIoTest, ExportUnknownFormatIsUnsupported)
{
    MeshExporter exporter;
    EXPECT_EQ(exporter.exportAs(*makeCube(), MeshFormat::Unknown, std::filesystem::temp_directory_path() / "x.bin"),
              vn::io::IoError::Unsupported);
}

TEST(MeshIoTest, LoadCachesSameFile)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_cache.stl";
    writeTriangleStl(path);

    MeshLoader loader(noScaling());
    auto       first  = loader.loadMerged(path);
    auto       second = loader.loadMerged(path);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.value().get(), second.value().get()) << "the same file is parsed once";

    // Clearing releases what the loader kept; the next load builds its own result.
    loader.clearCache();
    auto third = loader.loadMerged(path);
    ASSERT_TRUE(third);
    EXPECT_NE(first.value().get(), third.value().get()) << "a cleared cache hands out a freshly built mesh";

    std::filesystem::remove(path);
}

TEST(MeshIoTest, DifferentOptionsBuildDifferentGeometry)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_opts.stl";
    writeTriangleStl(path);

    // Options belong to the loader that holds them, so two policies are two loaders - each with its own cache.
    MeshLoader plain(noScaling());
    MeshLoader scaled(MeshLoader::Options{ .scale_mode = MeshLoader::ScaleMode::Custom, .custom_scale_factor = 2.0 });

    auto first  = plain.loadMerged(path);
    auto second = scaled.loadMerged(path);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(first.value().get(), second.value().get());

    std::filesystem::remove(path);
}

TEST(MeshIoTest, CustomScaleMultipliesVertices)
{
    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_scale.stl";
    writeTriangleStl(path);

    MeshLoader loader(
        MeshLoader::Options{ .scale_mode = MeshLoader::ScaleMode::Custom, .custom_scale_factor = 3.0 });
    auto mesh = loader.loadMerged(path);
    ASSERT_TRUE(mesh);

    const auto& itm       = vn::obj_cast<IndexedTriangleMesh>(*mesh.value());
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

TEST(MeshIoTest, ModelToStlAppliesTransformsOnce)
{
    // STL has no node tree, so a model written to it is flattened by the writer. The transform must land exactly once:
    // a writer that bakes it and then lets the flattening pass run again would move the part by twice the offset.
    auto triangle = makeCube();

    vn::geometry::Model::Entry entry;
    entry.mesh = triangle;
    vn::geometry::Model::Node root;
    root.children = { 1u }; // the root itself draws nothing
    vn::geometry::Model::Node shifted;
    shifted.name                       = vn::String(u8"shifted");
    shifted.transform.element(0, 3)    = 5.0; // +5 on x
    shifted.meshes                     = { 0u };

    auto model = vn::make_intrusive<vn::geometry::Model>();
    model->setMeshes({ entry });
    model->setNodes({ root, shifted });
    model->setRoot(0u);

    const auto path = std::filesystem::temp_directory_path() / "vn_meshio_model_flat.stl";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*model, MeshFormat::Stl, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       merged = loader.loadMerged(path);
    ASSERT_TRUE(merged);

    const auto& positions = merged.value()->positions();
    ASSERT_FALSE(positions.empty());
    float min_x = positions.front().x;
    float max_x = positions.front().x;
    for (const auto& p : positions) {
        min_x = std::min(min_x, p.x);
        max_x = std::max(max_x, p.x);
    }
    EXPECT_FLOAT_EQ(min_x, 4.0f) << "the cube spans [-1, 1], shifted by exactly +5";
    EXPECT_FLOAT_EQ(max_x, 6.0f);

    std::filesystem::remove(path);
}
