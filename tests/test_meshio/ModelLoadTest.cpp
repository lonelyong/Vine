#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <vine/Object.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/meshio/MeshLoader.hpp>

using vn::obj_cast;
using vn::geometry::IndexedTriangleMesh;
using vn::geometry::MaterialType;
using vn::geometry::Model;
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
        path_ = std::filesystem::temp_directory_path(ec) / ("vine_model_" + std::to_string(counter.fetch_add(1)));
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

/** @brief Writes a file, so the path entry has something to read. */
void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.good());
    out << text;
    ASSERT_TRUE(out.good());
}

/** @brief A two-object OBJ that names two materials from a library next to it. */
const char* const kTwoObjectObj = R"(mtllib parts.mtl
o first
v 0 0 0
v 1 0 0
v 0 1 0
vt 0 0
vt 1 0
vt 0 1
usemtl red
f 1/1 2/2 3/3
o second
v 10 0 0
v 11 0 0
v 10 1 0
usemtl green
f 4 5 6
)";

/** @brief The material library the OBJ above names: both are Phong, one has a texture the loader ignores. */
const char* const kTwoMaterialMtl = R"(newmtl red
Ka 0.1 0.1 0.1
Kd 1 0 0
Ks 1 1 1
Ns 64
map_Kd red.png
newmtl green
Ka 0.1 0.1 0.1
Kd 0 1 0
Ks 1 1 1
Ns 8
)";

/** @brief A COLLADA scene whose one node is translated, so a transform has something to carry. */
const char* const kShiftedCollada = R"(<?xml version="1.0" encoding="utf-8"?>
<COLLADA xmlns="http://www.collada.org/2005/11/COLLADASchema" version="1.4.1">
  <asset><up_axis>Y_UP</up_axis></asset>
  <library_geometries>
    <geometry id="tri" name="tri">
      <mesh>
        <source id="tri-pos">
          <float_array id="tri-pos-array" count="9">0 0 0 1 0 0 0 1 0</float_array>
          <technique_common>
            <accessor source="#tri-pos-array" count="3" stride="3">
              <param name="X" type="float"/><param name="Y" type="float"/><param name="Z" type="float"/>
            </accessor>
          </technique_common>
        </source>
        <vertices id="tri-verts"><input semantic="POSITION" source="#tri-pos"/></vertices>
        <triangles count="1">
          <input semantic="VERTEX" source="#tri-verts" offset="0"/>
          <p>0 1 2</p>
        </triangles>
      </mesh>
    </geometry>
  </library_geometries>
  <library_visual_scenes>
    <visual_scene id="scene">
      <node id="shifted" name="shifted">
        <translate>10 0 0</translate>
        <instance_geometry url="#tri"/>
      </node>
    </visual_scene>
  </library_visual_scenes>
  <scene><instance_visual_scene url="#scene"/></scene>
</COLLADA>
)";

/** @brief Finds a node by name, or null. */
const Model::Node* findNode(const Model& model, const std::string& name)
{
    for (const Model::Node& node : model.nodes()) {
        if (node.name.as_std_str() == name) {
            return &node;
        }
    }
    return nullptr;
}

} // namespace

TEST(ModelLoadTest, KeepsEveryPartAndItsMaterial)
{
    const TempDir dir;
    const auto    obj_path = dir.path() / "parts.obj";
    writeFile(obj_path, kTwoObjectObj);
    writeFile(dir.path() / "parts.mtl", kTwoMaterialMtl);

    MeshLoader::Options options;
    options.scale_mode = MeshLoader::ScaleMode::Disabled; // the assertion below is about structure, not units
    MeshLoader loader;
    loader.setOptions(options);

    auto model = loader.loadModel(obj_path);
    ASSERT_TRUE(model);

    // Two objects are two parts, and each part is one triangle of its own.
    ASSERT_EQ(model->meshes().size(), 2u);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*model->meshes()[0].mesh).triangleCount(), 1u);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*model->meshes()[1].mesh).triangleCount(), 1u);

    // A part is in its own space: the second object keeps the coordinates the file gave it.
    const auto& second_positions = obj_cast<IndexedTriangleMesh>(*model->meshes()[1].mesh).positions();
    ASSERT_EQ(second_positions.size(), 3u);
    EXPECT_FLOAT_EQ(second_positions[0].x, 10.0f);

    // Texture coordinates survive here, unlike in the flattened entry.
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*model->meshes()[0].mesh).texcoords().size(), 3u);

    // The material came out of the library, with the name the file declared and the shading model an MTL describes.
    const auto& red = model->meshes()[0].material;
    ASSERT_TRUE(red);
    EXPECT_EQ(red->name().as_std_str(), "red");
    EXPECT_EQ(red->materialType(), MaterialType::Phong);
    EXPECT_FLOAT_EQ(obj_cast<vn::geometry::PhongMaterial>(*red).diffuse().r, 1.0f);
    const auto& green = model->meshes()[1].material;
    ASSERT_TRUE(green);
    EXPECT_EQ(green->name().as_std_str(), "green");

    // The nodes name the objects, and the model's root is the node the tree starts at.
    EXPECT_NE(findNode(*model, "first"), nullptr);
    EXPECT_NE(findNode(*model, "second"), nullptr);
    EXPECT_LT(model->root(), model->nodes().size());

    // The flattened entry still reads the same file, and it holds both parts at once.
    auto merged = loader.load(obj_path);
    ASSERT_TRUE(merged);
    EXPECT_EQ(obj_cast<IndexedTriangleMesh>(*merged).triangleCount(), 2u);

    // The same path loads once: the cache hands back the very model it built.
    EXPECT_EQ(loader.loadModel(obj_path).get(), model.get());
}

TEST(ModelLoadTest, ReadsPartsFromAStreamButNotTheMaterialLibrary)
{
    // A stream carries one file, so the material library the OBJ names is out of reach. What the OBJ itself spells out
    // still arrives - the parts and the material names its usemtl lines declare - while the parameters only the library
    // could supply do not.
    std::istringstream in{ kTwoObjectObj };

    MeshLoader::Options options;
    options.scale_mode = MeshLoader::ScaleMode::Disabled;
    MeshLoader loader;
    loader.setOptions(options);

    auto model = loader.loadModel(in, "obj");
    ASSERT_TRUE(model);
    ASSERT_EQ(model->meshes().size(), 2u);
    EXPECT_NE(findNode(*model, "first"), nullptr);

    ASSERT_TRUE(model->meshes()[0].material);
    EXPECT_EQ(model->meshes()[0].material->name().as_std_str(), "red");
    ASSERT_TRUE(model->meshes()[1].material);
    EXPECT_EQ(model->meshes()[1].material->name().as_std_str(), "green");

    const auto& without_library = obj_cast<vn::geometry::PhongMaterial>(*model->meshes()[0].material);
    EXPECT_FALSE(without_library.diffuse().r == 1.0f && without_library.diffuse().g == 0.0f)
        << "the library was not reachable, so its red cannot have been applied";

    // The same bytes read by path, where the library IS reachable, carry what the library declares.
    const TempDir dir;
    writeFile(dir.path() / "parts.obj", kTwoObjectObj);
    writeFile(dir.path() / "parts.mtl", kTwoMaterialMtl);

    MeshLoader by_path;
    by_path.setOptions(options);
    auto with_library = by_path.loadModel(dir.path() / "parts.obj");
    ASSERT_TRUE(with_library);
    ASSERT_TRUE(with_library->meshes()[0].material);
    const auto& from_library = obj_cast<vn::geometry::PhongMaterial>(*with_library->meshes()[0].material);
    EXPECT_FLOAT_EQ(from_library.diffuse().r, 1.0f);
    EXPECT_FLOAT_EQ(from_library.diffuse().g, 0.0f);
    EXPECT_FLOAT_EQ(from_library.shininess(), 64.0f);
}

TEST(ModelLoadTest, KeepsTheTransformOfANodeInsteadOfBakingIt)
{
    std::istringstream in{ kShiftedCollada };

    MeshLoader::Options options;
    options.scale_mode = MeshLoader::ScaleMode::Disabled;
    MeshLoader loader;
    loader.setOptions(options);

    auto model = loader.loadModel(in, "dae");
    ASSERT_TRUE(model);

    // The transform sits on the node...
    const Model::Node* shifted = findNode(*model, "shifted");
    ASSERT_NE(shifted, nullptr);
    EXPECT_FLOAT_EQ(static_cast<float>(shifted->transform.element(0, 3)), 10.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(shifted->transform.element(1, 3)), 0.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(shifted->transform.element(3, 3)), 1.0f);

    // ...and the part's vertices are still where the file put them, so nothing was baked twice.
    ASSERT_EQ(model->meshes().size(), 1u);
    const auto& positions = obj_cast<IndexedTriangleMesh>(*model->meshes()[0].mesh).positions();
    ASSERT_EQ(positions.size(), 3u);
    EXPECT_FLOAT_EQ(positions[0].x, 0.0f);

    // The flattened entry bakes that transform, which is the difference between the two entries.
    std::istringstream again{ kShiftedCollada };
    auto               merged = loader.load(again, "dae");
    ASSERT_TRUE(merged);
    const auto& merged_positions = obj_cast<IndexedTriangleMesh>(*merged).positions();
    ASSERT_EQ(merged_positions.size(), 3u);
    EXPECT_FLOAT_EQ(merged_positions[0].x, 10.0f);
}
