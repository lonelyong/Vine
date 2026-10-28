#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <ios>
#include <memory>
#include <sstream>
#include <string>

#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/DataSourceStream.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/meshio/MeshExporter.hpp>
#include <vine/meshio/MeshFormat.hpp>
#include <vine/meshio/MeshLoader.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::io::DataSourceStream;
using vn::io::IoError;
using vn::io::ZipArchive;
using vn::math::Vec3f;
using vn::meshio::MeshExporter;
using vn::meshio::MeshFormat;
using vn::meshio::MeshLoader;

namespace
{

/**
 * @brief Builds one triangle in the z = 0 plane.
 *
 * @return A mesh holding a single triangle.
 */
vn::intrusive_ptr<IndexedTriangleMesh> makeTriangle()
{
    auto mesh = vn::intrusive_ptr<IndexedTriangleMesh>(new IndexedTriangleMesh());

    const auto a = mesh->addVertex(Vec3f(0, 0, 0));
    const auto b = mesh->addVertex(Vec3f(1, 0, 0));
    const auto c = mesh->addVertex(Vec3f(0, 1, 0));
    mesh->addTriangle(a, b, c);
    return mesh;
}

MeshLoader::Options noScaling()
{
    return MeshLoader::Options{ .scale_mode = MeshLoader::ScaleMode::Disabled };
}

} // namespace

TEST(MeshStreamIoTest, ExportsToAStreamAndLoadsBackFromOne)
{
    MeshExporter       exporter;
    std::ostringstream out(std::ios::binary);
    ASSERT_EQ(exporter.exportAs(*makeTriangle(), MeshFormat::Stl, out), IoError::Ok);
    const std::string bytes = out.str();
    ASSERT_FALSE(bytes.empty());

    std::istringstream in(bytes, std::ios::binary);
    MeshLoader         loader(noScaling());
    auto               loaded = loader.loadMerged(in, MeshFormat::Stl);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded.value()).triangleCount(), 1u);
}

TEST(MeshStreamIoTest, ExportsObjToAStreamAndLoadsBackFromOne)
{
    MeshExporter       exporter;
    std::ostringstream out(std::ios::binary);
    ASSERT_EQ(exporter.exportAs(*makeTriangle(), MeshFormat::Obj, out), IoError::Ok);
    const std::string text = out.str();
    ASSERT_FALSE(text.empty());

    // A stream carries one file, so the text must not point at a material library that was never written.
    EXPECT_EQ(text.find("mtllib"), std::string::npos);

    std::istringstream in(text, std::ios::binary);
    MeshLoader         loader(noScaling());
    auto               loaded = loader.loadMerged(in, MeshFormat::Obj);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded.value()).triangleCount(), 1u);
}

TEST(MeshStreamIoTest, ModelToStreamIsFlattened)
{
    // A model has a node tree; STL does not. The writer flattens it, so the stream holds the same triangle soup the
    // path entry would write.
    auto triangle = makeTriangle();

    vn::geometry::Model::Entry entry;
    entry.mesh = triangle;
    vn::geometry::Model::Node root;
    root.meshes = { 0u };
    auto model  = vn::make_intrusive<vn::geometry::Model>();
    model->setMeshes({ entry });
    model->setNodes({ root });
    model->setRoot(0u);

    MeshExporter       exporter;
    std::ostringstream out(std::ios::binary);
    ASSERT_EQ(exporter.exportAs(*model, MeshFormat::Stl, out), IoError::Ok);

    std::istringstream in(out.str(), std::ios::binary);
    MeshLoader         loader(noScaling());
    auto               loaded = loader.loadMerged(in, MeshFormat::Stl);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded.value()).triangleCount(), 1u);
}

TEST(MeshStreamIoTest, RefusesANonStreamableFormat)
{
    // glTF's normal form is two files; one stream cannot hold it, and the refusal is a classified answer, not a throw.
    MeshExporter       exporter;
    std::ostringstream out(std::ios::binary);
    EXPECT_EQ(exporter.exportAs(*makeTriangle(), MeshFormat::Gltf, out), IoError::Unsupported);
    EXPECT_TRUE(out.str().empty()) << "a refused export writes nothing";

    auto source = exporter.createSource(makeTriangle(), MeshFormat::Gltf);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->size(), 0u);
    EXPECT_EQ(source->error(), IoError::Unsupported);
}

TEST(MeshStreamIoTest, LoadsAnEntryOfAPackageWithoutTouchingTheFileSystem)
{
    // The model never becomes a file: it is generated into a package and read straight back out of it.
    MeshExporter exporter;
    ZipArchive   package;
    ASSERT_EQ(package.addFile(u8"geoms/link.stl", exporter.createSource(makeTriangle(), MeshFormat::Stl)), IoError::Ok);

    auto entry = package.openRead(u8"geoms/link.stl");
    ASSERT_TRUE(entry.ok());

    DataSourceStream stream(**entry);
    EXPECT_EQ(stream.size(), 84u + 50u * 1u);

    MeshLoader loader(noScaling());
    auto       loaded = loader.loadMerged(stream, MeshFormat::Stl);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded.value()).triangleCount(), 1u);
    EXPECT_EQ(stream.error(), IoError::Ok) << "the entry read cleanly";
}

TEST(MeshStreamIoTest, RefusesAnEmptyOrBrokenStream)
{
    MeshLoader loader;

    std::istringstream empty;
    auto               nothing = loader.loadMerged(empty, MeshFormat::Stl);
    EXPECT_FALSE(nothing) << "there is nothing to parse";
    EXPECT_EQ(nothing.error(), IoError::InvalidData);

    std::istringstream broken(std::ios::binary);
    broken.setstate(std::ios::badbit);
    auto failed = loader.loadMerged(broken, MeshFormat::Stl);
    EXPECT_FALSE(failed) << "a stream that broke is not content";
    EXPECT_EQ(failed.error(), IoError::IoFailure);
}
