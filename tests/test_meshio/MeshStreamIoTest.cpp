#include <gtest/gtest.h>

#include <cstdint>
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
#include <vine/meshio/MeshLoader.hpp>
#include <vine/meshio/BinStlSource.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::io::DataSourceStream;
using vn::io::IoError;
using vn::io::ZipArchive;
using vn::math::Vec3f;
using vn::meshio::MeshExporter;
using vn::meshio::MeshLoader;
using vn::meshio::BinStlSource;

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

} // namespace

TEST(MeshStreamIoTest, ExportsToAStreamAndLoadsBackFromOne)
{
    std::ostringstream out(std::ios::binary);
    MeshExporter::defaultInstance().exportAsStl(*makeTriangle(), out);
    const std::string bytes = out.str();
    ASSERT_FALSE(bytes.empty());

    std::istringstream in(bytes, std::ios::binary);
    auto               loaded = MeshLoader::defaultInstance().load(in, "stl");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 1u);
}

TEST(MeshStreamIoTest, ExportsObjToAStreamAndLoadsBackFromOne)
{
    std::ostringstream out(std::ios::binary);
    MeshExporter::defaultInstance().exportAsObj(*makeTriangle(), out);
    const std::string text = out.str();
    ASSERT_FALSE(text.empty());

    // A stream carries one file, so the text must not point at a material library that was never written.
    EXPECT_EQ(text.find("mtllib"), std::string::npos);

    std::istringstream in(text, std::ios::binary);
    auto               loaded = MeshLoader::defaultInstance().load(in, "obj");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 1u);
}

TEST(MeshStreamIoTest, LoadsAnEntryOfAPackageWithoutTouchingTheFileSystem)
{
    // The model never becomes a file: it is generated into a package and read straight back out of it.
    ZipArchive package;
    ASSERT_EQ(package.addFile(u8"geoms/link.stl", std::make_shared<BinStlSource>(makeTriangle())), IoError::Ok);

    auto entry = package.openRead(u8"geoms/link.stl");
    ASSERT_TRUE(entry.ok());

    DataSourceStream stream(**entry);
    EXPECT_EQ(stream.size(), 84u + 50u * 1u);

    auto loaded = MeshLoader::defaultInstance().load(stream, "stl");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 1u);
    EXPECT_EQ(stream.error(), IoError::Ok) << "the entry read cleanly";
}

TEST(MeshStreamIoTest, RefusesAnEmptyOrBrokenStream)
{
    std::istringstream empty;
    EXPECT_EQ(MeshLoader::defaultInstance().load(empty, "stl"), nullptr) << "there is nothing to parse";

    std::istringstream broken(std::ios::binary);
    broken.setstate(std::ios::badbit);
    EXPECT_EQ(MeshLoader::defaultInstance().load(broken, "stl"), nullptr) << "a stream that broke is not content";
}
