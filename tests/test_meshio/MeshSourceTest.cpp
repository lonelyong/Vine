#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/io/DataSourceStream.hpp>
#include <vine/io/IoError.hpp>
#include <vine/io/Stream.hpp>
#include <vine/io/ZipArchive.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/meshio/AsciiStlSource.hpp>
#include <vine/meshio/BinStlSource.hpp>
#include <vine/meshio/MeshLoader.hpp>
#include <vine/meshio/ObjSource.hpp>

using vn::geometry::IndexedTriangleMesh;
using vn::geometry::Mesh;
using vn::geometry::TriangleMesh;
using vn::geometry::Vec3fArray;
using vn::io::DataSource;
using vn::io::IoError;
using vn::io::ZipArchive;
using vn::math::Vec3f;
using vn::meshio::AsciiStlSource;
using vn::meshio::BinStlSource;
using vn::meshio::MeshLoader;
using vn::meshio::ObjSource;

namespace
{

/**
 * @brief Creates a unique temporary directory that is removed on destruction.
 */
class TempDir
{
  public:
    TempDir()
    {
        static std::atomic<unsigned long long> counter{ 0 };
        std::error_code                        ec;
        path_ = std::filesystem::temp_directory_path(ec) / ("vine_stl_" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

/**
 * @brief Reads a source to its end in pieces of a given size.
 *
 * @param source The source to consume.
 * @param piece Bytes to ask for at a time; one byte off a record boundary is what makes a pull resume mid-record.
 * @return Every byte it produced, in order.
 */
std::vector<unsigned char> drainInPieces(DataSource& source, std::size_t piece)
{
    std::vector<unsigned char> out;
    std::vector<std::byte>     buffer(piece);
    for (std::size_t got = 0; (got = source.read(buffer)) != 0;) {
        out.insert(out.end(), reinterpret_cast<const unsigned char*>(buffer.data()),
                   reinterpret_cast<const unsigned char*>(buffer.data()) + got);
    }
    EXPECT_EQ(source.error(), IoError::Ok);
    return out;
}

/**
 * @brief Reads a source to its end.
 *
 * @param source The source to consume.
 * @return Every byte it produced, in order.
 */
std::vector<unsigned char> drain(DataSource& source)
{
    std::vector<unsigned char>  out;
    std::array<std::byte, 4096> buffer{};
    for (std::size_t got = 0; (got = source.read(buffer)) != 0;) {
        out.insert(out.end(), reinterpret_cast<const unsigned char*>(buffer.data()),
                   reinterpret_cast<const unsigned char*>(buffer.data()) + got);
    }
    EXPECT_EQ(source.error(), IoError::Ok);
    return out;
}

/**
 * @brief Reads a float out of a byte block, the way the format wrote it.
 *
 * @param bytes The block.
 * @param offset Byte offset of the value.
 * @return The value.
 */
float floatAt(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, bytes.data() + offset, sizeof(float));
    return value;
}

/**
 * @brief Reads the 32-bit triangle count out of a binary STL header.
 *
 * @param bytes The block.
 * @return The count the header records.
 */
std::uint32_t countAt(const std::vector<unsigned char>& bytes)
{
    std::uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, sizeof(count));
    return count;
}

/**
 * @brief A quad in the z = 0 plane, wound counter-clockwise, as an indexed mesh.
 *
 * @return Two triangles over four vertices; their facet normal is (0, 0, 1).
 */
vn::intrusive_ptr<IndexedTriangleMesh> makeIndexedQuad()
{
    auto mesh = vn::intrusive_ptr<IndexedTriangleMesh>(new IndexedTriangleMesh());

    const auto a = mesh->addVertex(Vec3f(0, 0, 0));
    const auto b = mesh->addVertex(Vec3f(1, 0, 0));
    const auto c = mesh->addVertex(Vec3f(1, 1, 0));
    const auto d = mesh->addVertex(Vec3f(0, 1, 0));

    mesh->addTriangle(a, b, c);
    mesh->addTriangle(a, c, d);
    return mesh;
}

/**
 * @brief The same quad as a triangle soup, three positions per triangle.
 *
 * @return Two triangles over six vertices.
 */
vn::intrusive_ptr<TriangleMesh> makeSoupQuad()
{
    auto mesh = vn::intrusive_ptr<TriangleMesh>(new TriangleMesh());

    mesh->addTriangle(Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(1, 1, 0));
    mesh->addTriangle(Vec3f(0, 0, 0), Vec3f(1, 1, 0), Vec3f(0, 1, 0));
    return mesh;
}

} // namespace

TEST(BinStlSourceTest, StatesTheExactLengthOfTheStlItWrites)
{
    const BinStlSource indexed(makeIndexedQuad());
    EXPECT_EQ(indexed.error(), IoError::Ok);
    EXPECT_EQ(indexed.size(), 84u + 50u * 2u) << "a binary STL is 84 bytes plus 50 per triangle";

    const BinStlSource soup(makeSoupQuad());
    EXPECT_EQ(soup.error(), IoError::Ok);
    EXPECT_EQ(soup.size(), 84u + 50u * 2u);
}

TEST(BinStlSourceTest, WritesTheLayoutTheFormatAsksFor)
{
    BinStlSource source(makeIndexedQuad());

    const auto bytes = drain(source);
    ASSERT_EQ(bytes.size(), 84u + 50u * 2u);
    EXPECT_EQ(countAt(bytes), 2u) << "the header records the triangle count";

    // First triangle: the facet normal comes from the winding, then the three corners.
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 0), 0.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 4), 0.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 8), 1.0f) << "a counter-clockwise quad in z = 0 faces +z";

    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 12), 0.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 16), 0.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 20), 0.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 24), 1.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 36), 1.0f);
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 40), 1.0f) << "the third corner is (1, 1, 0)";

    std::uint16_t attribute = 1;
    std::memcpy(&attribute, bytes.data() + 84 + 48, sizeof(attribute));
    EXPECT_EQ(attribute, 0u) << "the attribute byte count is zero";
}

TEST(BinStlSourceTest, RestartsAndRepeatsItself)
{
    BinStlSource source(makeIndexedQuad());

    const auto first = drain(source);
    source.rewind();
    const auto second = drain(source);

    EXPECT_EQ(first, second) << "every byte is a function of its triangle, so a second pull is the same content";
}

TEST(BinStlSourceTest, HandsOutPiecesSmallerThanATriangle)
{
    BinStlSource source(makeIndexedQuad());
    const auto whole = drain(source);

    source.rewind();
    std::vector<unsigned char> pieces;
    std::array<std::byte, 3>   tiny{};
    for (std::size_t got = 0; (got = source.read(tiny)) != 0;) {
        pieces.insert(pieces.end(), reinterpret_cast<const unsigned char*>(tiny.data()),
                      reinterpret_cast<const unsigned char*>(tiny.data()) + got);
    }
    EXPECT_EQ(source.error(), IoError::Ok);
    EXPECT_EQ(pieces, whole) << "a buffer that cuts a triangle in half changes nothing about the content";
}

TEST(BinStlSourceTest, AppliesTheScaleFactor)
{
    BinStlSource source(makeIndexedQuad(), 2.0);

    const auto bytes = drain(source);
    ASSERT_EQ(bytes.size(), 84u + 50u * 2u) << "a quad is two triangles";
    EXPECT_FLOAT_EQ(floatAt(bytes, 84 + 24), 2.0f) << "the second corner (1, 0, 0) scales to (2, 0, 0)";
}

TEST(BinStlSourceTest, LoadsBackThroughMeshLoader)
{
    const TempDir                dir;
    const std::filesystem::path  file = dir.path() / "quad.stl";

    {
        BinStlSource     source(makeIndexedQuad());
        const auto    bytes = drain(source);
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(out.good());
    }

    // The reference reader is assimp: what it makes of the file is the check that the layout is a real STL.
    auto loaded = MeshLoader::defaultInstance().load(file);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 2u);
}

TEST(BinStlSourceTest, FillsAPackageWithoutMaterializingTheModel)
{
    const TempDir dir;
    const auto    package = dir.path() / "mesh.zip";

    BinStlSource  source(makeIndexedQuad());
    const auto expected = drain(source);
    source.rewind();

    {
        ZipArchive archive;
        ASSERT_EQ(archive.addFile(u8"geoms/link.stl", std::make_shared<BinStlSource>(std::move(source))), IoError::Ok);
        ASSERT_EQ(archive.saveAs(package), IoError::Ok) << "the ZIP pulls the STL while it is written";
    }

    const auto opened = ZipArchive::open(package, ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    const auto stored = opened->read(u8"geoms/link.stl");
    ASSERT_TRUE(stored.ok());
    EXPECT_EQ(std::vector<unsigned char>(stored->begin(), stored->end()), expected);
}

TEST(BinStlSourceTest, RefusesAMeshItCannotWrite)
{
    const BinStlSource none(vn::intrusive_ptr<const Mesh>{});
    EXPECT_EQ(none.error(), IoError::InvalidData);
    EXPECT_EQ(none.size(), 0u);

    const BinStlSource empty(vn::intrusive_ptr<const Mesh>(new IndexedTriangleMesh()));
    EXPECT_EQ(empty.error(), IoError::InvalidData);

    ZipArchive archive;
    EXPECT_EQ(archive.addFile(u8"geoms/link.stl", std::make_shared<BinStlSource>(vn::intrusive_ptr<const Mesh>{})),
              IoError::InvalidData)
        << "the refusal is visible where the source is handed over, not only at save time";
}

TEST(BinStlSourceTest, RefusesAMeshEditedAfterItWasHandedOver)
{
    auto      mesh = makeIndexedQuad();
    BinStlSource source(mesh);

    std::array<std::byte, 16> head{};
    ASSERT_EQ(source.read(head), 16u);
    EXPECT_EQ(source.error(), IoError::Ok);

    // Growing the mesh changes what the source would write: half of it would be the mesh as it was, half as it is.
    mesh->addVertex(Vec3f(2, 2, 0));
    mesh->addTriangle(0, 2, 4);

    EXPECT_EQ(source.read(head), 0u);
    EXPECT_EQ(source.error(), IoError::InvalidData) << "an edit is a failed save, not a silently mixed file";
}

namespace
{

/**
 * @brief Returns a source's bytes as text.
 *
 * @param bytes The bytes a text format produced.
 * @return The same bytes as a string.
 */
std::string textOf(const std::vector<unsigned char>& bytes)
{
    return std::string(bytes.begin(), bytes.end());
}

/**
 * @brief Counts the lines of a text format that start with a given keyword.
 *
 * @param text The whole file.
 * @param keyword The line prefix to count, e.g. "v".
 * @return The number of lines that start with it.
 */
std::size_t countLines(const std::string& text, const std::string& keyword)
{
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t end  = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.find_first_not_of(" \t") != std::string::npos
            && line.substr(line.find_first_not_of(" \t")).starts_with(keyword + " ")) {
            ++count; // a text format may indent its lines, so the keyword is what counts, not the column
        }
        at = end == std::string::npos ? text.size() : end + 1;
    }
    return count;
}

} // namespace

TEST(AsciiStlSourceTest, StatesNoLengthAndStillWritesTheWholeSolid)
{
    AsciiStlSource source(makeIndexedQuad());

    // A text format only knows its length once it is laid out, so it says so instead of guessing - which is what an
    // unmeasured source is allowed to do, and what a backend that can store one takes.
    EXPECT_EQ(source.size(), vn::io::kUnknownSize);

    const std::string text = textOf(drain(source));
    EXPECT_TRUE(text.starts_with("solid ")) << text.substr(0, 32);
    EXPECT_TRUE(text.ends_with("endsolid vine\n"));
    EXPECT_EQ(countLines(text, "facet"), 2u) << "one facet per triangle";
    EXPECT_EQ(countLines(text, "vertex"), 6u) << "three corners per facet";
    EXPECT_NE(text.find("facet normal 0 0 1\n"), std::string::npos) << "the quad lies in the z = 0 plane";
}

TEST(AsciiStlSourceTest, AppliesTheScaleFactor)
{
    AsciiStlSource source(makeIndexedQuad(), 2.0);

    const std::string text = textOf(drain(source));
    EXPECT_NE(text.find("vertex 2 0 0\n"), std::string::npos) << "the corner (1, 0, 0) scales to (2, 0, 0)";
}

TEST(AsciiStlSourceTest, HandsOutPiecesSmallerThanAFacet)
{
    AsciiStlSource source(makeIndexedQuad());
    const auto    whole = drain(source);

    source.rewind();
    EXPECT_EQ(drainInPieces(source, 7), whole) << "a buffer that cuts a facet in half changes nothing about the text";
}

TEST(AsciiStlSourceTest, LoadsBackThroughMeshLoader)
{
    const TempDir dir;

    // Straight into a package, then out of the package as a stream: no file on disk anywhere in this test, and the
    // text the loader sees is the text this source produced.
    ZipArchive package;
    ASSERT_EQ(package.addFile(u8"geoms/quad.stl", std::make_shared<AsciiStlSource>(makeIndexedQuad())), IoError::Ok);
    ASSERT_EQ(package.saveAs(dir.path() / "quad.zip"), IoError::Ok);

    const auto opened = ZipArchive::open(dir.path() / "quad.zip", ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    auto entry = opened->openRead(u8"geoms/quad.stl");
    ASSERT_TRUE(entry.ok());
    const auto stored = opened->read(u8"geoms/quad.stl");
    ASSERT_TRUE(stored.ok());
    const std::string text = textOf(std::vector<unsigned char>(stored->begin(), stored->end()));
    EXPECT_TRUE(text.starts_with("solid ")) << "the entry holds the text this source produced";

    vn::io::DataSourceStream stream(**entry);
    auto                       loaded = MeshLoader::defaultInstance().load(stream, "stl");
    ASSERT_NE(loaded, nullptr) << "the file assimp could not read:\n" << text;
    EXPECT_EQ(stream.error(), IoError::Ok) << "the entry read cleanly through the stream";
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 2u);
}

TEST(AsciiStlSourceTest, RefusesAMeshItCannotWrite)
{
    const AsciiStlSource none(vn::intrusive_ptr<const Mesh>{});
    EXPECT_EQ(none.error(), IoError::InvalidData);
    EXPECT_EQ(none.size(), 0u);

    ZipArchive archive;
    EXPECT_EQ(archive.addFile(u8"geoms/link.stl", std::make_shared<AsciiStlSource>(vn::intrusive_ptr<const Mesh>{})),
              IoError::InvalidData)
        << "the refusal is visible where the source is handed over, not only at save time";
}

TEST(AsciiStlSourceTest, RefusesAMeshEditedAfterItWasHandedOver)
{
    auto           mesh = makeIndexedQuad();
    AsciiStlSource source(mesh);

    std::array<std::byte, 16> head{};
    ASSERT_EQ(source.read(head), 16u);

    mesh->addVertex(Vec3f(2, 2, 0));
    EXPECT_EQ(source.read(head), 0u);
    EXPECT_EQ(source.error(), IoError::InvalidData);
}

TEST(ObjSourceTest, WritesEveryVertexThenItsFaces)
{
    ObjSource source(makeIndexedQuad());

    EXPECT_EQ(source.size(), vn::io::kUnknownSize) << "a text format cannot state its length up front";

    const std::string text = textOf(drain(source));
    EXPECT_EQ(countLines(text, "v"), 4u) << "the quad's four vertices";
    EXPECT_EQ(countLines(text, "f"), 2u) << "one face per triangle";
    EXPECT_EQ(text.find("vn "), std::string::npos) << "the mesh carries no normals, so the file names none";

    // Faces count from one, and every vertex is defined before the first face names it.
    EXPECT_NE(text.find("f 1 2 3\n"), std::string::npos);
    EXPECT_LT(text.find("v 0 0 0\n"), text.find("f 1 2 3\n"));
}

TEST(ObjSourceTest, NamesTheNormalsItWrites)
{
    auto          mesh = makeIndexedQuad();
    Vec3fArray    normals;
    for (int i = 0; i < 4; ++i) {
        normals.emplace_back(0.0f, 0.0f, 1.0f);
    }
    mesh->setNormals(std::move(normals));

    ObjSource         source(mesh);
    const std::string text = textOf(drain(source));
    EXPECT_EQ(countLines(text, "vn"), 4u) << "one normal per vertex";
    EXPECT_NE(text.find("f 1//1 2//2 3//3\n"), std::string::npos) << "a face names the normal of each of its corners";
}

TEST(ObjSourceTest, HandsOutPiecesSmallerThanALine)
{
    ObjSource  source(makeIndexedQuad());
    const auto whole = drain(source);

    source.rewind();
    EXPECT_EQ(drainInPieces(source, 3), whole) << "a buffer that cuts a line in half changes nothing about the text";
}

TEST(ObjSourceTest, LoadsBackThroughMeshLoader)
{
    const TempDir dir;

    ZipArchive package;
    ASSERT_EQ(package.addFile(u8"geoms/quad.obj", std::make_shared<ObjSource>(makeIndexedQuad())), IoError::Ok);
    ASSERT_EQ(package.saveAs(dir.path() / "quad.zip"), IoError::Ok);

    const auto opened = ZipArchive::open(dir.path() / "quad.zip", ZipArchive::OpenMode::ReadOnly);
    ASSERT_TRUE(opened.ok());
    auto entry = opened->openRead(u8"geoms/quad.obj");
    ASSERT_TRUE(entry.ok());
    const auto stored = opened->read(u8"geoms/quad.obj");
    ASSERT_TRUE(stored.ok());
    const std::string text = textOf(std::vector<unsigned char>(stored->begin(), stored->end()));
    EXPECT_NE(text.find("f 1 2 3"), std::string::npos) << "the entry holds the text this source produced";

    // The entry is read as a stream, so the model never becomes a file anywhere in this chain.
    vn::io::DataSourceStream stream(**entry);
    auto                       loaded = MeshLoader::defaultInstance().load(stream, "obj");
    ASSERT_NE(loaded, nullptr) << "the file assimp could not read:\n" << text;
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*loaded).triangleCount(), 2u);
}
