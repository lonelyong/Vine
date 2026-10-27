#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <sstream>
#include <string>

#include <vine/intrusive_ptr.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/meshio/MeshExporter.hpp>
#include <vine/meshio/MeshFormat.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::io::DataSource;
using vn::io::IoError;
using vn::math::Vec3f;
using vn::meshio::MeshExporter;
using vn::meshio::MeshFormat;

namespace
{

/**
 * @brief Builds a mesh holding one triangle in the z = 0 plane.
 *
 * @return The mesh.
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

/**
 * @brief Pulls a source to its end, in pieces smaller than one record.
 *
 * @param source The source to read.
 * @return Everything it produced.
 */
std::string pull(DataSource& source)
{
    std::string             bytes;
    std::array<std::byte, 7> chunk{};
    for (;;) {
        const std::size_t got = source.read(chunk);
        if (got == 0) {
            break;
        }
        bytes.append(reinterpret_cast<const char*>(chunk.data()), got);
    }
    return bytes;
}

} // namespace

TEST(MeshSourceTest, StatesTheExactLengthOfTheStlItWrites)
{
    // Binary STL is a fixed layout, so the length is known before the first byte: 80-byte header, a 32-bit triangle
    // count, then 50 bytes per triangle.
    MeshExporter exporter;
    auto         source = exporter.createSource(makeTriangle(), MeshFormat::Stl);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(source->error(), IoError::Ok);
    EXPECT_EQ(source->size(), 84u + 50u * 1u);
    EXPECT_EQ(pull(*source).size(), 84u + 50u * 1u);
}

TEST(MeshSourceTest, ReplaysTheSameBytes)
{
    // A container may save twice; the source replays what it produced rather than exporting again.
    MeshExporter exporter;
    auto         source = exporter.createSource(makeTriangle(), MeshFormat::Stl);
    ASSERT_NE(source, nullptr);

    const std::string first = pull(*source);
    source->rewind();
    const std::string second = pull(*source);
    EXPECT_EQ(first, second);
    EXPECT_EQ(source->error(), IoError::Ok);
}

TEST(MeshSourceTest, MatchesTheStreamEntry)
{
    // One writer per format: the lazy source and exportAs(stream) produce the same content, so a caller can pick the
    // target it has without getting different bytes.
    MeshExporter       exporter;
    std::ostringstream stream(std::ios::binary);
    ASSERT_EQ(exporter.exportAs(*makeTriangle(), MeshFormat::Stl, stream), IoError::Ok);

    auto source = exporter.createSource(makeTriangle(), MeshFormat::Stl);
    ASSERT_NE(source, nullptr);
    EXPECT_EQ(pull(*source), stream.str());
}

TEST(MeshSourceTest, RefusesANullMesh)
{
    MeshExporter exporter;
    auto         source = exporter.createSource(vn::intrusive_ptr<const vn::geometry::Mesh>{}, MeshFormat::Stl);
    ASSERT_NE(source, nullptr) << "the source contract is never-null; the refusal is a classified error";
    EXPECT_EQ(source->size(), 0u);
    EXPECT_EQ(source->error(), IoError::InvalidData);
}
