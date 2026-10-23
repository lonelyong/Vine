/**
 * @brief The model -> scene bridge (see `.ai/backlog/graphics.md` §2 G1).
 *
 * `geometry::Model` keeps what a flat mesh loses (the node tree, per-node transforms, per-entry materials), but
 * nothing in the renderer consumed it: the only product path flattened the file into one mesh with one grey
 * material. These cases pin the conversion the renderer needs - the tree shape, the ONE-allocation rule for a mesh
 * reused in two places, and the three source material kinds.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <vine/geometry/ColorMaterial.hpp>
#include <vine/geometry/IndexedTriangleMesh.hpp>
#include <vine/geometry/Material.hpp>
#include <vine/geometry/Model.hpp>
#include <vine/geometry/PbrMaterial.hpp>
#include <vine/geometry/PhongMaterial.hpp>
#include <vine/math/Transform3.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/ModelScene.hpp>

using vn::geometry::ColorMaterial;
using vn::geometry::IndexedTriangleMesh;
using vn::geometry::Model;
using vn::geometry::PbrMaterial;
using vn::geometry::PhongMaterial;
using vn::graphics::Geometry;
using vn::graphics::Group;
using vn::graphics::Material;
using vn::graphics::MatrixTransform;
using vn::graphics::sceneFromModel;

namespace
{

/// @brief One triangle, named, ready to be put in a model entry.
vn::intrusive_ptr<IndexedTriangleMesh> makeTriangle(const char8_t* name)
{
    auto mesh = vn::make_intrusive<IndexedTriangleMesh>();
    const std::uint32_t a = mesh->addVertex(vn::math::Vec3f{ 0.0F, 0.0F, 0.0F });
    const std::uint32_t b = mesh->addVertex(vn::math::Vec3f{ 1.0F, 0.0F, 0.0F });
    const std::uint32_t c = mesh->addVertex(vn::math::Vec3f{ 0.0F, 1.0F, 0.0F });
    mesh->addTriangle(a, b, c);
    mesh->setName(name);
    return mesh;
}

/// @brief The first Geometry under @p node (the conversion attaches a node's own parts before its children).
const Geometry* firstGeometry(const Group& group)
{
    for (const auto& child : group.children()) {
        if (const auto* geometry = dynamic_cast<const Geometry*>(child.get())) {
            return geometry;
        }
    }
    return nullptr;
}

} // namespace

TEST(ModelSceneTest, TheTreeShapeNamesAndTransformsComeFromTheModel)
{
    Model model;
    Model::Node root;
    root.name     = u8"world";
    root.children = { 1U };
    Model::Node child;
    child.name      = u8"link";
    child.transform = vn::math::translate<double>(1.0, 2.0, 3.0);
    child.meshes    = { 0U };
    model.setNodes({ root, child });
    model.setRoot(0U);

    Model::Entry entry;
    entry.mesh = makeTriangle(u8"body");
    model.setMeshes({ entry });

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr);
    EXPECT_EQ(scene.geometries, 1U);

    // The identity root stays a plain Group (no matrix multiply for nothing), named after the node.
    const auto* converted_root = dynamic_cast<const Group*>(scene.root.get());
    ASSERT_NE(converted_root, nullptr);
    EXPECT_EQ(converted_root->name(), vn::String(u8"world"));
    ASSERT_EQ(converted_root->children().size(), 1U);

    // The child carries a transform, so it is a MatrixTransform - and its geometry is named after the MESH.
    const auto* converted_child = dynamic_cast<const MatrixTransform*>(converted_root->children()[0].get());
    ASSERT_NE(converted_child, nullptr);
    EXPECT_EQ(converted_child->name(), vn::String(u8"link"));
    EXPECT_DOUBLE_EQ(converted_child->matrix()(0, 3), 1.0);
    EXPECT_DOUBLE_EQ(converted_child->matrix()(1, 3), 2.0);
    EXPECT_DOUBLE_EQ(converted_child->matrix()(2, 3), 3.0);

    const Geometry* leaf = firstGeometry(*converted_child);
    ASSERT_NE(leaf, nullptr);
    EXPECT_EQ(leaf->name(), vn::String(u8"body"));
    EXPECT_EQ(leaf->positionCount(), 3U);
    ASSERT_EQ(leaf->primitiveCount(), 1U);
    EXPECT_EQ(leaf->primitive(0)->indexCount(), 3U);
}

TEST(ModelSceneTest, AMeshUsedTwiceIsOneAllocationBehindTwoLeaves)
{
    // OSG / vsg scene graphs share a drawable under several transforms; this graph is a strict tree, so the LEAF is
    // copied and the DATA is not: both geometries must read the same position buffer.
    auto        mesh = makeTriangle(u8"shared");
    Model       model;
    Model::Node left;
    left.name   = u8"left";
    left.meshes = { 0U };
    Model::Node right;
    right.name      = u8"right";
    right.transform = vn::math::translate<double>(5.0, 0.0, 0.0);
    right.meshes    = { 0U };
    Model::Node root;
    root.name     = u8"root";
    root.children = { 1U, 2U };
    model.setNodes({ root, left, right });
    model.setRoot(0U);
    Model::Entry entry;
    entry.mesh = mesh;
    model.setMeshes({ entry });

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr);
    EXPECT_EQ(scene.geometries, 2U);

    const auto* converted_root = dynamic_cast<const Group*>(scene.root.get());
    ASSERT_NE(converted_root, nullptr);
    ASSERT_EQ(converted_root->children().size(), 2U);
    const Geometry* first  = firstGeometry(*dynamic_cast<const Group*>(converted_root->children()[0].get()));
    const Geometry* second = firstGeometry(*dynamic_cast<const Group*>(converted_root->children()[1].get()));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second) << "the leaves are separate nodes (the graph is a tree)";
    const vn::graphics::AttributeChannel* first_positions  = first->buffer(0U);
    const vn::graphics::AttributeChannel* second_positions = second->buffer(0U);
    ASSERT_NE(first_positions, nullptr);
    ASSERT_NE(second_positions, nullptr);
    EXPECT_EQ(first_positions->values.get(), second_positions->values.get())
        << "the vertex data is ONE allocation, shared by both leaves";
}

TEST(ModelSceneTest, TheThreeSourceMaterialsBecomePhongParametersAndPbrOpacityBecomesLeafOpacity)
{
    auto color_material = vn::make_intrusive<ColorMaterial>(vn::Colorf(1.0F, 0.0F, 0.0F, 1.0F));
    color_material->setName(u8"red");
    auto phong_material = vn::make_intrusive<PhongMaterial>(
        vn::Colorf(0.1F, 0.1F, 0.1F, 1.0F), vn::Colorf(0.2F, 0.4F, 0.6F, 1.0F),
        vn::Colorf(0.9F, 0.9F, 0.9F, 1.0F), 64.0F);
    phong_material->setName(u8"phong");
    auto pbr_material = vn::make_intrusive<PbrMaterial>(vn::Colorf(0.8F, 0.8F, 0.8F, 1.0F), 1.0F, 0.25F, 0.4F);
    pbr_material->setName(u8"pbr");

    Model       model;
    Model::Node root;
    root.name   = u8"root";
    root.meshes = { 0U, 1U, 2U, 3U };
    model.setNodes({ root });
    model.setRoot(0U);
    Model::Entry color_entry;
    color_entry.mesh     = makeTriangle(u8"a");
    color_entry.material = color_material;
    Model::Entry phong_entry;
    phong_entry.mesh     = makeTriangle(u8"b");
    phong_entry.material = phong_material;
    Model::Entry pbr_entry;
    pbr_entry.mesh     = makeTriangle(u8"c");
    pbr_entry.material = pbr_material;
    Model::Entry missing_entry;  // no mesh: counted, not fatal
    model.setMeshes({ color_entry, phong_entry, pbr_entry, missing_entry });

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr);
    EXPECT_EQ(scene.geometries, 3U);
    EXPECT_EQ(scene.skipped_meshes, 1U) << "a null entry is skipped and counted, the rest still converts";

    const auto* converted_root = dynamic_cast<const Group*>(scene.root.get());
    ASSERT_NE(converted_root, nullptr);
    ASSERT_EQ(converted_root->children().size(), 3U);

    const auto* red = dynamic_cast<const Geometry*>(converted_root->children()[0].get());
    ASSERT_NE(red, nullptr);
    ASSERT_NE(red->material(), nullptr);
    EXPECT_EQ(red->material()->name(), vn::String(u8"red"));
    EXPECT_FLOAT_EQ(red->material()->diffuse().r, 1.0F);
    EXPECT_FLOAT_EQ(red->material()->diffuse().g, 0.0F);
    EXPECT_FLOAT_EQ(red->material()->specular().a, 0.0F) << "a flat colour asks for no highlight of its own";
    EXPECT_FLOAT_EQ(red->opacity(), 1.0F);

    const auto* phong = dynamic_cast<const Geometry*>(converted_root->children()[1].get());
    ASSERT_NE(phong, nullptr);
    ASSERT_NE(phong->material(), nullptr);
    EXPECT_FLOAT_EQ(phong->material()->diffuse().b, 0.6F);
    EXPECT_FLOAT_EQ(phong->material()->specular().r, 0.9F);
    EXPECT_FLOAT_EQ(phong->material()->shininess(), 64.0F);

    const auto* pbr = dynamic_cast<const Geometry*>(converted_root->children()[2].get());
    ASSERT_NE(pbr, nullptr);
    ASSERT_NE(pbr->material(), nullptr);
    EXPECT_FLOAT_EQ(pbr->material()->diffuse().r, 0.0F)
        << "a fully metallic surface has no diffuse lobe (the PBR approximation)";
    EXPECT_FLOAT_EQ(pbr->material()->specular().r, 0.8F)
        << "and its highlight takes the base colour";
    EXPECT_FLOAT_EQ(pbr->material()->specular().a, 0.75F) << "1 - roughness";
    EXPECT_FLOAT_EQ(pbr->opacity(), 0.4F) << "a PBR material's opacity is the LEAF's, not the shared material's";
}

TEST(ModelSceneTest, AnEmptyModelBuildsNothingAndSaysSoInItsCounts)
{
    // "No nodes" is not an error and not a root with nothing under it: there is nothing to place, so the answer is
    // a null root and zero counts (a caller can tell "empty" from "built, drew nothing").
    const vn::graphics::ModelScene scene = sceneFromModel(Model{});
    EXPECT_EQ(scene.root, nullptr);
    EXPECT_EQ(scene.geometries, 0U);
    EXPECT_EQ(scene.skipped_meshes, 0U);
    EXPECT_EQ(scene.skipped_nodes, 0U);
}

TEST(ModelSceneTest, ARootIndexOutsideTheNodeTableIsReportedNotGuessed)
{
    // A malformed model is not silently repaired: the caller sees a null root and one skipped node, instead of a
    // tree built from an index nobody declared.
    Model       model;
    Model::Node node;
    node.name = u8"only";
    model.setNodes({ node });
    model.setRoot(3U);

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    EXPECT_EQ(scene.root, nullptr);
    EXPECT_EQ(scene.skipped_nodes, 1U);
}

TEST(ModelSceneTest, ACyclicNodeReferenceIsCutInsteadOfRecursingForever)
{
    // The model is data, so its references can form a loop (a hand-built or corrupted file). The scene graph it is
    // converted into forbids cycles anyway (Group::addChild rejects a re-parent that would close one), so the walk
    // cuts the second visit, counts it, and still answers a usable tree.
    Model       model;
    Model::Node root;
    root.name     = u8"root";
    root.children = { 0U, 1U };  // 0 is the node itself
    Model::Node child;
    child.name     = u8"child";
    child.children = { 0U };  // and it points back at the root
    model.setNodes({ root, child });
    model.setRoot(0U);

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr);
    EXPECT_EQ(scene.skipped_nodes, 2U) << "the self reference and the back reference are both cut";
    EXPECT_EQ(scene.geometries, 0U);

    const auto* group = dynamic_cast<const Group*>(scene.root.get());
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->children().size(), 1U) << "only the child that was not on the path is attached";
}

TEST(ModelSceneTest, OneSourceMaterialIsOneGraphicsMaterial)
{
    // A material referenced by two entries must reach the backend as ONE object: two objects would each get their
    // own material slot, and the draws that share a material would stop being one batch. The cache is keyed by the
    // source object, not by its values.
    auto shared = vn::make_intrusive<ColorMaterial>(vn::Colorf(0.2F, 0.4F, 0.8F, 1.0F));
    shared->setName(u8"shared");

    Model       model;
    Model::Node root;
    root.name   = u8"root";
    root.meshes = { 0U, 1U };
    model.setNodes({ root });
    model.setRoot(0U);
    Model::Entry first;
    first.mesh     = makeTriangle(u8"a");
    first.material = shared;
    Model::Entry second;
    second.mesh     = makeTriangle(u8"b");
    second.material = shared;
    model.setMeshes({ first, second });

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr);
    ASSERT_EQ(scene.geometries, 2U);
    const auto* group = dynamic_cast<const Group*>(scene.root.get());
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->children().size(), 2U);

    const auto* left  = dynamic_cast<const Geometry*>(group->children()[0].get());
    const auto* right = dynamic_cast<const Geometry*>(group->children()[1].get());
    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    ASSERT_NE(left->material(), nullptr);
    EXPECT_EQ(left->material(), right->material()) << "one source material, one graphics material";
    EXPECT_EQ(left->material()->name(), vn::String(u8"shared"));
}

TEST(ModelSceneTest, AnEntryIndexOutsideTheTableIsCountedNotDereferenced)
{
    // The node's mesh list is an index list too: a stale index must be skipped like any other bad reference, not
    // read out of the array.
    Model       model;
    Model::Node node;
    node.name   = u8"root";
    node.meshes = { 5U };  // no such entry
    model.setNodes({ node });
    model.setRoot(0U);

    const vn::graphics::ModelScene scene = sceneFromModel(model);
    ASSERT_NE(scene.root, nullptr) << "the tree itself is still usable";
    EXPECT_EQ(scene.geometries, 0U);
    EXPECT_EQ(scene.skipped_meshes, 1U);
}
