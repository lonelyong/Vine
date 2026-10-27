#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <span>
#include <string>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/Object.hpp>
#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/geometry/PbrMaterial.hpp>
#include <vine/geometry/TriangleMesh.hpp>
#include <vine/math/Matrix4x4.hpp>
#include <vine/math/Vector2.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/math/Vector4.hpp>
#include <vine/meshio/MeshExporter.hpp>
#include <vine/meshio/MeshFormat.hpp>
#include <vine/meshio/MeshLoader.hpp>

using vn::geometry::ColorMaterial;
using vn::geometry::IndexedTriangleMesh;
using vn::geometry::Model;
using vn::geometry::PhongMaterial;
using vn::geometry::ShapeType;
using vn::geometry::TriangleMesh;
using vn::math::Vec2f;
using vn::math::Vec3f;
using vn::math::Vec4f;
using vn::meshio::MeshExporter;
using vn::meshio::MeshFormat;
using vn::meshio::MeshLoader;

namespace
{

/** @brief A unique temporary directory that is removed when it goes out of scope. */
class TempDir
{
  public:
    TempDir()
    {
        static std::atomic<unsigned long long> counter{ 0 };
        std::error_code                        ec;
        path_ = std::filesystem::temp_directory_path(ec) / ("vn_fidelity_" + std::to_string(counter.fetch_add(1)));
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

/** @brief Writes a text file, so a loader entry has something to read. */
void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << text;
    ASSERT_TRUE(out.good());
}

/**
 * @brief One triangle corner: every attribute that belongs to a vertex, and nothing else.
 *
 * Comparing corners instead of vertex ARRAYS is what makes an IO test independent of what an importer does to the
 * vertex list: joining identical vertices, splitting a corner whose normal cannot be shared, and reordering all leave
 * the sequence of triangles-with-attributes unchanged, while a dropped or altered attribute does not.
 */
struct Corner
{
    Vec3f position;
    Vec3f normal;
    Vec2f texcoord;
    Vec4f color;
};

/// Three corners; the order carries the winding, the starting corner does not.
using Triangle = std::array<Corner, 3>;

/**
 * @brief Largest colour difference a round trip may introduce through a byte-per-channel format.
 *
 * PLY and COLLADA store a vertex colour as one byte per channel, and the writer truncates rather than rounds, so a value
 * that is not a multiple of 1/255 comes back up to one step away: 0.125 arrives as 31/255 = 0.1216. One step is still
 * far below the distance between two different channels, which is what the comparison is really about.
 */
constexpr float kColorByteTolerance = 1.0f / 255.0f + 1e-4f;

/** @brief Returns whether two corners agree on every attribute within their tolerances. */
bool cornersMatch(const Corner& actual, const Corner& expected, float tolerance, float color_tolerance)
{
    return std::abs(actual.position.x - expected.position.x) <= tolerance
        && std::abs(actual.position.y - expected.position.y) <= tolerance
        && std::abs(actual.position.z - expected.position.z) <= tolerance
        && std::abs(actual.normal.x - expected.normal.x) <= tolerance
        && std::abs(actual.normal.y - expected.normal.y) <= tolerance
        && std::abs(actual.normal.z - expected.normal.z) <= tolerance
        && std::abs(actual.texcoord.x - expected.texcoord.x) <= tolerance
        && std::abs(actual.texcoord.y - expected.texcoord.y) <= tolerance
        && std::abs(actual.color.x - expected.color.x) <= color_tolerance
        && std::abs(actual.color.y - expected.color.y) <= color_tolerance
        && std::abs(actual.color.z - expected.color.z) <= color_tolerance
        && std::abs(actual.color.w - expected.color.w) <= color_tolerance;
}

/** @brief Returns whether @p actual holds the same three corners as @p expected, allowing the cyclic rotation. */
bool trianglesMatch(const Triangle& actual, const Triangle& expected, float tolerance, float color_tolerance)
{
    for (std::size_t rotation = 0; rotation < 3; ++rotation) {
        bool same = true;
        for (std::size_t corner = 0; corner < 3 && same; ++corner) {
            same = cornersMatch(actual[(corner + rotation) % 3], expected[corner], tolerance, color_tolerance);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

/** @brief Renders a triangle for a failure message. */
std::string describe(const Triangle& triangle)
{
    std::string text;
    for (const Corner& corner : triangle) {
        text += "[" + std::to_string(corner.position.x) + "," + std::to_string(corner.position.y) + ","
              + std::to_string(corner.position.z) + " n(" + std::to_string(corner.normal.x) + ","
              + std::to_string(corner.normal.y) + "," + std::to_string(corner.normal.z) + ") uv("
              + std::to_string(corner.texcoord.x) + "," + std::to_string(corner.texcoord.y) + ") c("
              + std::to_string(corner.color.x) + "," + std::to_string(corner.color.y) + ","
              + std::to_string(corner.color.z) + "," + std::to_string(corner.color.w) + ")] ";
    }
    return text;
}

/**
 * @brief Reads a mesh as the triangles it draws, whichever concrete shape holds them.
 *
 * An attribute the mesh does not carry stays zero on every corner, so a comparison against a corner that carries one
 * reports the loss rather than hiding it.
 *
 * @param mesh The mesh to read.
 * @return One entry per triangle, in the mesh's own order.
 */
std::vector<Triangle> trianglesOf(const vn::geometry::Mesh& mesh)
{
    const std::span<const Vec3f> positions = mesh.positions();
    const std::span<const Vec3f> normals   = mesh.normals();
    const std::span<const Vec2f> texcoords = mesh.texcoords();
    const std::span<const Vec4f> colors    = mesh.colors();

    std::vector<std::uint32_t> indices;
    if (mesh.shapeType() == ShapeType::IndexedTriangleMesh) {
        const auto& indexed = vn::obj_cast<IndexedTriangleMesh>(mesh);
        const std::span<const std::uint32_t> view = indexed.indices();
        indices.assign(view.begin(), view.end());
    } else {
        indices.resize(positions.size());
        std::iota(indices.begin(), indices.end(), std::uint32_t{ 0 });
    }

    const bool has_normals   = normals.size() == positions.size();
    const bool has_texcoords = texcoords.size() == positions.size();
    const bool has_colors    = colors.size() == positions.size();

    const auto cornerAt = [&](std::uint32_t index) {
        Corner corner{};
        corner.position = positions[index];
        if (has_normals) {
            corner.normal = normals[index];
        }
        if (has_texcoords) {
            corner.texcoord = texcoords[index];
        }
        if (has_colors) {
            corner.color = colors[index];
        }
        return corner;
    };

    std::vector<Triangle> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        triangles.push_back({ cornerAt(indices[i]), cornerAt(indices[i + 1]), cornerAt(indices[i + 2]) });
    }
    return triangles;
}

/**
 * @brief Asserts that @p actual holds exactly the triangles of @p expected, attributes included.
 *
 * Every expected triangle must have its own counterpart, so a lost, duplicated or altered triangle fails; the order of
 * the two lists and the starting corner of each triangle are free, because an importer rebuilds both.
 *
 * @param actual The triangles that came back.
 * @param expected The triangles that went in.
 * @param tolerance Largest per-component difference accepted for a position, a normal or a texcoord.
 * @param color_tolerance Largest per-channel difference accepted for a colour.
 */
void expectSameGeometry(std::span<const Triangle> actual, std::span<const Triangle> expected, float tolerance,
                        float color_tolerance)
{
    ASSERT_EQ(actual.size(), expected.size()) << "the round trip changed the triangle count";
    std::vector<bool> claimed(actual.size(), false);
    for (std::size_t want_index = 0; want_index < expected.size(); ++want_index) {
        SCOPED_TRACE("source triangle " + std::to_string(want_index) + ": " + describe(expected[want_index]));
        bool found = false;
        for (std::size_t i = 0; i < actual.size() && !found; ++i) {
            if (!claimed[i] && trianglesMatch(actual[i], expected[want_index], tolerance, color_tolerance)) {
                claimed[i] = true;
                found      = true;
            }
        }
        EXPECT_TRUE(found) << "no triangle came back with the attributes it went in with";
    }
}

/// The merged entry states it records no texture coordinates, so a comparison against it ignores them on both sides.
void stripTexcoords(std::vector<Triangle>& triangles)
{
    for (Triangle& triangle : triangles) {
        for (Corner& corner : triangle) {
            corner.texcoord = Vec2f{};
        }
    }
}

/// The same, for a format that cannot spell vertex colours.
void stripColors(std::vector<Triangle>& triangles)
{
    for (Triangle& triangle : triangles) {
        for (Corner& corner : triangle) {
            corner.color = Vec4f{};
        }
    }
}

/**
 * @brief Builds a cube with outward winding, one normal per corner, one texcoord per corner and one colour per corner.
 *
 * Every attribute has a value no other vertex shares, so an attribute that travels through the wrong slot, or one that
 * is dropped and replaced by a default, shows up as a mismatch rather than as a coincidence.
 *
 * @return The cube: 8 vertices, 12 triangles.
 */
vn::intrusive_ptr<IndexedTriangleMesh> makeRichCube()
{
    auto cube = vn::make_intrusive<IndexedTriangleMesh>();
    cube->setName(vn::String::fromUtf8("rich cube"));

    // Corners of a cube spanning [-1, 1]^3, ordered so the winding below can be checked by hand.
    const std::array<Vec3f, 8> corners{ Vec3f(-1, -1, -1), Vec3f(1, -1, -1), Vec3f(1, 1, -1), Vec3f(-1, 1, -1),
                                        Vec3f(-1, -1, 1),  Vec3f(1, -1, 1),  Vec3f(1, 1, 1),  Vec3f(-1, 1, 1) };

    std::array<std::uint32_t, 8> vertex_id{};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        vertex_id[i] = cube->addVertex(corners[i]);
    }

    // Outward-facing, counter-clockwise seen from outside.
    const std::array<std::array<std::size_t, 3>, 12> faces{ { { 0, 3, 2 }, { 0, 2, 1 }, // z = -1
                                                               { 4, 5, 6 }, { 4, 6, 7 }, // z = 1
                                                               { 0, 1, 5 }, { 0, 5, 4 }, // y = -1
                                                               { 3, 7, 6 }, { 3, 6, 2 }, // y = 1
                                                               { 0, 4, 7 }, { 0, 7, 3 }, // x = -1
                                                               { 1, 2, 6 }, { 1, 6, 5 } } }; // x = 1
    for (const auto& face : faces) {
        cube->addTriangle(vertex_id[face[0]], vertex_id[face[1]], vertex_id[face[2]]);
    }

    vn::geometry::Vec3fArray normals;
    vn::geometry::Vec2fArray texcoords;
    vn::geometry::Vec4fArray colors;
    normals.reserve(corners.size());
    texcoords.reserve(corners.size());
    colors.reserve(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Vec3f& corner = corners[i];
        const float  length = corner.length();
        normals.emplace_back(corner.x / length, corner.y / length, corner.z / length);
        texcoords.emplace_back((corner.x + 1.0f) * 0.25f, (corner.y + 1.0f) * 0.125f + static_cast<float>(i) * 0.0625f);
        colors.emplace_back(0.125f * static_cast<float>(i), 0.25f, 0.5f, 1.0f);
    }
    cube->setNormals(std::move(normals));
    cube->setTexcoords(std::move(texcoords));
    cube->setColors(std::move(colors));
    return cube;
}

/**
 * @brief Builds the same cube as a triangle soup, so the non-indexed shape takes the same trip.
 *
 * @return The soup: 36 vertices, 12 triangles, with the attributes of the cube above.
 */
vn::intrusive_ptr<TriangleMesh> makeRichTriangleSoup()
{
    const vn::intrusive_ptr<IndexedTriangleMesh> cube = makeRichCube();

    auto soup = vn::make_intrusive<TriangleMesh>();
    soup->setName(vn::String::fromUtf8("rich soup"));

    const std::span<const Vec3f> corners   = cube->positions();
    const std::span<const Vec3f> normals   = cube->normals();
    const std::span<const Vec2f> texcoords = cube->texcoords();
    const std::span<const Vec4f> colors    = cube->colors();

    vn::geometry::Vec3fArray positions;
    vn::geometry::Vec3fArray soup_normals;
    vn::geometry::Vec2fArray soup_texcoords;
    vn::geometry::Vec4fArray soup_colors;
    const std::span<const std::uint32_t> indices = cube->indices();
    positions.reserve(indices.size());
    soup_normals.reserve(indices.size());
    soup_texcoords.reserve(indices.size());
    soup_colors.reserve(indices.size());
    for (const std::uint32_t index : indices) {
        positions.push_back(corners[index]);
        soup_normals.push_back(normals[index]);
        soup_texcoords.push_back(texcoords[index]);
        soup_colors.push_back(colors[index]);
    }

    soup->setPositions(std::move(positions));
    soup->setNormals(std::move(soup_normals));
    soup->setTexcoords(std::move(soup_texcoords));
    soup->setColors(std::move(soup_colors));
    return soup;
}

/// A single triangle, no normals and no texture coordinates, so the loader has to derive the normals it hands out.
const char* const kPlainObj = R"(o plain
v 0 0 0
v 2 0 0
v 0 2 0
f 1 2 3
)";

/**
 * @brief Finds a node by name.
 *
 * @param model The model to search.
 * @param name The node name to look for.
 * @return The node, or null when the model holds none with that name.
 */
const Model::Node* findNode(const Model& model, const std::string& name)
{
    const std::vector<Model::Node>& nodes = model.nodes();
    const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const Model::Node& node) {
        return node.name.as_std_str() == name;
    });
    return it != nodes.end() ? &*it : nullptr;
}

/**
 * @brief Returns the base colour a material describes, whichever concrete type carries it.
 *
 * A writer decides which KIND of material a file describes - a format that always states a specular term turns a plain
 * colour into Phong - but the base colour itself is the datum every source has, so that is what a round trip owes.
 *
 * @param material The material to read.
 * @return The base colour of the material.
 */
const vn::Colorf& baseColorOf(const vn::geometry::Material& material)
{
    switch (material.materialType()) {
      case vn::geometry::MaterialType::Phong:
          return static_cast<const PhongMaterial&>(material).diffuse();
      case vn::geometry::MaterialType::Pbr:
          return static_cast<const vn::geometry::PbrMaterial&>(material).baseColor();
      default:
          return static_cast<const ColorMaterial&>(material).color();
    }
}

/// The options every test here wants: no scaling, so positions can be compared with the source.
MeshLoader::Options noScaling()
{
    return MeshLoader::Options{ .scale_mode = MeshLoader::ScaleMode::Disabled };
}

} // namespace

TEST(MeshFidelityTest, EveryVertexAttributeSurvivesARoundTrip)
{
    const TempDir dir;
    const auto    cube     = makeRichCube();
    const auto    expected = trianglesOf(*cube);

    const std::filesystem::path path = dir.path() / "rich.ply";

    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Ply, path), vn::io::IoError::Ok);

    // PLY is the family that can spell every attribute the mesh holds, so it is the one the claim is made against.
    // The MODEL entry is what reads them back: the merged entry says it records no texture coordinates.
    MeshLoader loader(noScaling());
    auto       loaded_model = loader.load(path);
    ASSERT_TRUE(loaded_model);
    const auto model = loaded_model.take();
    ASSERT_EQ(model->meshes().size(), 1u);
    const vn::geometry::Mesh& loaded = *model->meshes()[0].mesh;

    ASSERT_EQ(loaded.normals().size(), loaded.positions().size()) << "the normals were dropped";
    ASSERT_EQ(loaded.texcoords().size(), loaded.positions().size()) << "the texture coordinates were dropped";
    ASSERT_EQ(loaded.colors().size(), loaded.positions().size()) << "the colours were dropped";

    // Geometry, normals and texcoords travel as float32; only the colour channel has a byte per channel to fit in.
    expectSameGeometry(trianglesOf(loaded), expected, 1e-6f, kColorByteTolerance);
}

TEST(MeshFidelityTest, TheMergedEntryKeepsEveryAttributeItRecords)
{
    const TempDir dir;
    const auto    cube = makeRichCube();

    std::vector<Triangle> expected = trianglesOf(*cube);
    stripTexcoords(expected);

    const std::filesystem::path path = dir.path() / "merged.ply";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Ply, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       loaded = loader.loadMerged(path);
    ASSERT_TRUE(loaded);
    const auto merged = loaded.take();

    EXPECT_TRUE(merged->texcoords().empty()) << "the merged entry states it records no texture coordinates";
    ASSERT_EQ(merged->normals().size(), merged->positions().size());
    ASSERT_EQ(merged->colors().size(), merged->positions().size());

    std::vector<Triangle> actual = trianglesOf(*merged);
    stripTexcoords(actual);
    expectSameGeometry(actual, expected, 1e-6f, kColorByteTolerance);
}

TEST(MeshFidelityTest, ANonIndexedMeshSurvivesTheSameTrip)
{
    const TempDir dir;
    const auto    soup     = makeRichTriangleSoup();
    const auto    expected = trianglesOf(*soup);

    const std::filesystem::path path = dir.path() / "soup.ply";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*soup, MeshFormat::Ply, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       loaded = loader.load(path);
    ASSERT_TRUE(loaded);
    const auto model = loaded.take();
    ASSERT_EQ(model->meshes().size(), 1u);

    // The importer rebuilds the topology, so the shape may come back indexed - the triangles are what must match.
    expectSameGeometry(trianglesOf(*model->meshes()[0].mesh), expected, 1e-6f, kColorByteTolerance);
}

TEST(MeshFidelityTest, TheModelEntryKeepsNodesMaterialsAndGeometry)
{
    const TempDir dir;

    const auto cube = makeRichCube();
    const std::vector<Triangle> cube_expected = trianglesOf(*cube);

    auto flat_mesh = vn::make_intrusive<IndexedTriangleMesh>();
    flat_mesh->setName(vn::String::fromUtf8("tri"));
    flat_mesh->addVertex(Vec3f(0, 0, 0));
    flat_mesh->addVertex(Vec3f(4, 0, 0));
    flat_mesh->addVertex(Vec3f(0, 4, 0));
    flat_mesh->addTriangle(0, 1, 2);
    // Authored, so what comes back is what went in: deriving a normal is the loader's answer to a file that has none,
    // and that rule has its own test.
    flat_mesh->setNormals({ Vec3f(0, 0, 1), Vec3f(0, 0, 1), Vec3f(0, 0, 1) });
    const std::vector<Triangle> flat_expected = trianglesOf(*flat_mesh);

    auto shiny = vn::make_intrusive<PhongMaterial>(vn::Colorf(0.25f, 0.25f, 0.25f), vn::Colorf(0.5f, 0.25f, 0.125f),
                                                   vn::Colorf(1.0f, 1.0f, 1.0f), 64.0f);
    shiny->setName(vn::String::fromUtf8("shiny"));
    auto flat = vn::make_intrusive<ColorMaterial>(vn::Colorf(0.25f, 0.5f, 0.75f));
    flat->setName(vn::String::fromUtf8("flat"));

    Model::Entry cube_entry;
    cube_entry.mesh     = cube;
    cube_entry.material = shiny;
    Model::Entry flat_entry;
    flat_entry.mesh     = flat_mesh;
    flat_entry.material = flat;

    auto model = vn::make_intrusive<Model>();
    model->setMeshes({ cube_entry, flat_entry });

    Model::Node child;
    child.name = vn::String::fromUtf8("child");
    // Translated, so a baked-in transform would show up as moved vertices.
    child.transform = vn::math::Mat4d(1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 4, 0, 0, 0, 1);
    child.meshes    = { 1u };

    Model::Node root;
    root.name     = vn::String::fromUtf8("root");
    root.children = { 0u };
    root.meshes   = { 0u };

    model->setNodes({ child, root });
    model->setRoot(1u);

    const std::filesystem::path path = dir.path() / "model.dae";
    MeshExporter exporter;
    ASSERT_EQ(exporter.exportAs(*model, MeshFormat::Collada, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       loaded = loader.load(path);
    ASSERT_TRUE(loaded);
    const auto reloaded = loaded.take();

    // The tree: same names, and the child's transform is still a property of the node.
    const Model::Node* reloaded_child = findNode(*reloaded, "child");
    ASSERT_NE(reloaded_child, nullptr) << "a node name was dropped";
    EXPECT_FLOAT_EQ(static_cast<float>(reloaded_child->transform.element(0, 3)), 2.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(reloaded_child->transform.element(1, 3)), 3.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(reloaded_child->transform.element(2, 3)), 4.0f);

    // Both parts are still separate entries, each with its own material. COLLADA identifies a geometry by id and keeps
    // a name on the NODE and on the MATERIAL, not on the geometry, so the entries are told apart by what they hold.
    ASSERT_EQ(reloaded->meshes().size(), 2u);
    const Model::Entry* reloaded_cube     = nullptr;
    const Model::Entry* reloaded_triangle = nullptr;
    for (const Model::Entry& entry : reloaded->meshes()) {
        ASSERT_TRUE(entry.mesh);
        const std::size_t triangle_count = trianglesOf(*entry.mesh).size();
        if (triangle_count == cube_expected.size()) {
            reloaded_cube = &entry;
        } else if (triangle_count == flat_expected.size()) {
            reloaded_triangle = &entry;
        }
    }
    ASSERT_NE(reloaded_cube, nullptr) << "the twelve-triangle part is missing from the round trip";
    ASSERT_NE(reloaded_triangle, nullptr) << "the single-triangle part is missing from the round trip";

    ASSERT_TRUE(reloaded_cube->material);
    EXPECT_EQ(reloaded_cube->material->name().as_std_str(), "shiny");
    ASSERT_TRUE(reloaded_triangle->material);
    EXPECT_EQ(reloaded_triangle->material->name().as_std_str(), "flat");

    const auto* shiny_back = dynamic_cast<const PhongMaterial*>(reloaded_cube->material.get());
    ASSERT_NE(shiny_back, nullptr) << "a material that carried specular terms came back as a different kind";
    EXPECT_NEAR(shiny_back->diffuse().r, 0.5f, 1e-4f);
    EXPECT_NEAR(shiny_back->diffuse().g, 0.25f, 1e-4f);
    EXPECT_NEAR(shiny_back->shininess(), 64.0f, 1e-2f);

    // A plain diffuse colour comes back through the type the FORMAT describes - COLLADA always states a specular term,
    // so a ColorMaterial arrives as Phong - but the colour it carried is the datum that has to survive.
    const vn::Colorf& flat_color = baseColorOf(*reloaded_triangle->material);
    EXPECT_NEAR(flat_color.r, 0.25f, 1e-4f);
    EXPECT_NEAR(flat_color.g, 0.5f, 1e-4f);
    EXPECT_NEAR(flat_color.b, 0.75f, 1e-4f);

    // The geometry travels with its entry: nothing is baked into the vertices, and nothing is lost.
    expectSameGeometry(trianglesOf(*reloaded_cube->mesh), cube_expected, 1e-4f, kColorByteTolerance);
    expectSameGeometry(trianglesOf(*reloaded_triangle->mesh), flat_expected, 1e-4f, kColorByteTolerance);
}

TEST(MeshFidelityTest, AFileWithoutNormalsArrivesWithDerivedOnes)
{
    const TempDir dir;
    writeFile(dir.path() / "plain.obj", kPlainObj);

    MeshLoader loader(noScaling());

    // Both entries state a normal per vertex: a mesh the file describes without normals is not handed out without them,
    // because an absent normal means "derive it from the geometry" to the renderer, not "draw it flat".
    auto       loaded = loader.loadMerged(dir.path() / "plain.obj");
    ASSERT_TRUE(loaded);
    const auto merged = loaded.take();
    ASSERT_EQ(merged->normals().size(), merged->positions().size());
    for (const Vec3f& normal : merged->normals()) {
        EXPECT_NEAR(normal.length(), 1.0f, 1e-4f) << "a derived normal is not a unit vector";
    }

    auto       loaded_model = loader.load(dir.path() / "plain.obj");
    ASSERT_TRUE(loaded_model);
    const auto model = loaded_model.take();
    ASSERT_EQ(model->meshes().size(), 1u);
    const vn::geometry::Mesh& entry_mesh = *model->meshes()[0].mesh;
    EXPECT_EQ(entry_mesh.normals().size(), entry_mesh.positions().size());
    EXPECT_TRUE(entry_mesh.colors().empty()) << "a file with no colour must not invent one";
}

TEST(MeshFidelityTest, TheExportScaleFactorReachesEveryVertex)
{
    const TempDir dir;
    const auto    cube = makeRichCube();

    std::vector<Triangle> expected = trianglesOf(*cube);
    stripTexcoords(expected); // the merged entry records none
    for (Triangle& triangle : expected) {
        for (Corner& corner : triangle) {
            corner.position.x *= 3.0f;
            corner.position.y *= 3.0f;
            corner.position.z *= 3.0f;
        }
    }

    const std::filesystem::path path = dir.path() / "scaled.ply";
    MeshExporter exporter(MeshExporter::Options{ .scale_factor = 3.0 });
    ASSERT_EQ(exporter.exportAs(*cube, MeshFormat::Ply, path), vn::io::IoError::Ok);

    MeshLoader loader(noScaling());
    auto       loaded = loader.loadMerged(path);
    ASSERT_TRUE(loaded);
    const auto merged = loaded.take();

    std::vector<Triangle> actual = trianglesOf(*merged);
    stripTexcoords(actual);
    expectSameGeometry(actual, expected, 1e-5f, kColorByteTolerance);

    // The normals are not scaled with the positions: a direction has no length to scale.
    ASSERT_EQ(merged->normals().size(), merged->positions().size());
    for (const Vec3f& normal : merged->normals()) {
        EXPECT_NEAR(normal.length(), 1.0f, 1e-4f);
    }
}

TEST(MeshFidelityTest, TheLoadScaleRuleIsTheSameForBothEntries)
{
    const TempDir dir;
    writeFile(dir.path() / "plain.obj", kPlainObj);

    MeshLoader loader(MeshLoader::Options{ .scale_mode          = MeshLoader::ScaleMode::Custom,
                                           .custom_scale_factor = 2.5 });

    // The merged entry bakes the factor into the vertices...
    auto       loaded = loader.loadMerged(dir.path() / "plain.obj");
    ASSERT_TRUE(loaded);
    const auto merged = loaded.take();
    float      largest = 0.0f;
    for (const Vec3f& position : merged->positions()) {
        largest = std::max({ largest, position.x, position.y, position.z });
    }
    EXPECT_NEAR(largest, 5.0f, 1e-4f) << "a vertex of the plain triangle was not scaled";

    // ...while the model entry folds the same factor into the root, so the parts keep their places.
    auto       loaded_model = loader.load(dir.path() / "plain.obj");
    ASSERT_TRUE(loaded_model);
    const auto model = loaded_model.take();
    ASSERT_LT(model->root(), model->nodes().size());
    const Model::Node& root = model->nodes()[model->root()];
    EXPECT_NEAR(static_cast<float>(root.transform.element(0, 0)), 2.5f, 1e-4f);
    EXPECT_NEAR(static_cast<float>(root.transform.element(1, 1)), 2.5f, 1e-4f);

    ASSERT_EQ(model->meshes().size(), 1u);
    const std::span<const Vec3f> positions = model->meshes()[0].mesh->positions();
    ASSERT_EQ(positions.size(), 3u);
    EXPECT_FLOAT_EQ(positions[1].x, 2.0f) << "the model entry must not bake the factor as well";
}
