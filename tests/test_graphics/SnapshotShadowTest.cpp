#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include "ContentMirror.hpp"
#include "DrawListDiff.hpp"
#include "PublishOutcome.hpp"
#include "SceneRows.hpp"
#include "SceneSnapshot.hpp"

using vn::graphics::CollectView;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MaterialPtr;
using vn::graphics::MatrixTransform;
using vn::graphics::Scene;
using vn::graphics::detail::ContentMirror;
using vn::graphics::detail::diffDrawLists;
using vn::graphics::detail::DrawListDiff;
using vn::graphics::detail::PublishOutcome;
using vn::graphics::detail::PublishStatus;
using vn::graphics::detail::SceneRows;
using vn::graphics::detail::selectSnapshot;
using vn::intrusive_ptr;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief The clip box [-1, 1]^3, eye on +Z: the same value both sources are asked with.
CollectView clipBoxView()
{
    CollectView view;
    view.eye = Vec3d(0.0, 0.0, 10.0);
    return view;
}

/// @brief One triangle-list draw (positions only, one primitive).
GeometryPtr triangle()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.5F, 0.0F, 0.0F), Vec3f(0.0F, 0.5F, 0.0F) });
    mesh->bumpRevision();
    return vn::graphics::geometryFromShape(*mesh);
}

/// @brief A transform node at @p x, @p y, @p z.
intrusive_ptr<MatrixTransform> placed(double x, double y, double z)
{
    auto node = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    node->setMatrix(vn::math::translate(x, y, z));
    return node;
}

/// @brief A small articulated assembly whose edits cover every shadow-relevant row field.
struct Assembly {
    intrusive_ptr<Group>           root;
    intrusive_ptr<MatrixTransform> arm;
    intrusive_ptr<MatrixTransform> link;
    intrusive_ptr<MatrixTransform> loose;
    GeometryPtr                    link_geometry;
    MaterialPtr                    material;
};

Assembly makeAssembly()
{
    Assembly assembly;
    assembly.root          = intrusive_ptr<Group>(new Group());
    assembly.arm           = placed(0.2, 0.0, 0.0);
    assembly.link          = placed(0.1, 0.0, 0.0);
    assembly.loose         = placed(-0.6, 0.1, 0.0);
    assembly.link_geometry = triangle();
    assembly.material      = MaterialPtr(new vn::graphics::Material());
    assembly.material->setMetallic(0.0F);
    assembly.link_geometry->setMaterial(assembly.material);
    assembly.root->addChild(assembly.arm);
    assembly.arm->addChild(assembly.link);
    assembly.link->addChild(assembly.link_geometry);
    assembly.loose->addChild(triangle());
    assembly.root->addChild(assembly.loose);
    return assembly;
}

/// @brief Asserts the production snapshot list and the legacy mirror list are field-exact for @p view.
void expectShadowAgrees(ContentMirror& mirror, const CollectView& view, const char* what)
{
    const auto actual   = mirror.snapshotSource().collectRenderCommands(view, vn::graphics::kAllLayers);
    const auto expected = mirror.legacySource().collectRenderCommands(view, vn::graphics::kAllLayers);
    ASSERT_TRUE(actual != nullptr);
    ASSERT_TRUE(expected != nullptr);

    const DrawListDiff diff = diffDrawLists(*actual, *expected);
    EXPECT_TRUE(diff.same) << what << ": " << (diff.field != nullptr ? diff.field : "?") << " differs at draw "
                           << diff.index << " (mirror " << actual->size() << " draws, snapshot " << expected->size() << ")";
}

} // namespace

TEST(SnapshotShadowTest, TheSnapshotSourceMatchesTheMirrorAcrossSixHundredFrames)
{
    // T34 / 5a. The shadow path is the migration's equivalence proof: for the same view, `selectSnapshot` and the
    // mirror's rows must produce one list, field for field. Six hundred frames is far longer than the three in-flight
    // slots and exercises transform, state, material and structure edits in the same run.
    Assembly assembly = makeAssembly();
    Scene    scene;
    scene.setRoot(assembly.root);

    ContentMirror mirror(scene);
    mirror.setLegacyShadow(true);

    const CollectView view = clipBoxView();
    for (std::uint64_t frame = 1; frame <= 600U; ++frame) {
        const double phase = static_cast<double>(frame) * 0.01;
        assembly.arm->setMatrix(vn::math::translate(0.2 * std::sin(phase), 0.1 * std::cos(phase), 0.0));
        if (frame % 10U == 0U) {
            assembly.link_geometry->setOpacity(0.5F + 0.001F * static_cast<float>(frame % 50U));
        }
        if (frame % 25U == 0U) {
            assembly.material->setMetallic(0.1F + 0.01F * static_cast<float>(frame % 80U));
        }
        if (frame % 40U == 0U) {
            assembly.loose->setMatrix(vn::math::translate(-0.6, 0.1 * std::sin(phase), 0.0));
        }

        const PublishOutcome outcome = mirror.publish(scene);
        EXPECT_TRUE(outcome.status == PublishStatus::Posted || outcome.status == PublishStatus::Unchanged)
            << "frame " << frame << " refused to publish";

        mirror.beginFrame(frame);
        (void)mirror.drain();
        ASSERT_NE(mirror.snapshotSource().snapshot(), nullptr) << "frame " << frame << " has no published snapshot";
        expectShadowAgrees(mirror, view, "animated frame");
    }
}

TEST(SnapshotShadowTest, AnEditBatchKeepsBothSidesOnTheOldPicture)
{
    // The batch's whole point is that the mirror keeps the picture the batch started from (T31). The shadow is taken
    // from the same publication as the rows, so it must hold the same old picture and move with the batch's one
    // publication - not one frame earlier.
    Assembly assembly = makeAssembly();
    Scene    scene;
    scene.setRoot(assembly.root);

    ContentMirror mirror(scene);
    mirror.setLegacyShadow(true);
    ASSERT_EQ(mirror.publish(scene).status, PublishStatus::Posted);
    mirror.beginFrame(1U);
    (void)mirror.drain();

    const CollectView view = clipBoxView();
    expectShadowAgrees(mirror, view, "before the batch");

    scene.beginEdit();
    assembly.arm->setMatrix(vn::math::translate(0.4, 0.0, 0.0));
    assembly.link_geometry->setOpacity(0.25F);
    EXPECT_EQ(mirror.publish(scene).status, PublishStatus::InsideBatch);
    mirror.beginFrame(2U);
    (void)mirror.drain();
    expectShadowAgrees(mirror, view, "inside the batch");

    scene.endEdit();
    EXPECT_EQ(mirror.publish(scene).status, PublishStatus::Posted);
    mirror.beginFrame(3U);
    (void)mirror.drain();
    expectShadowAgrees(mirror, view, "after the batch");
}

TEST(SnapshotShadowTest, AnOlderSnapshotKeepsTheShapeAndProgramItWasPublishedWith)
{
    // T17 ON THE SNAPSHOT SIDE. A frame in flight must keep the material VALUES, the geometry DESCRIPTION and the
    // resolved PROGRAM it was published with, even though all three are edited through live objects. The old snapshot
    // holds immutable values; the fresh one must carry the new pointers. This is the snapshot half of the old mirror's
    // in-flight test (RenderEngineTest.AMaterialEditLandsOnTheNextFrame...).
    auto root = intrusive_ptr<Group>(new Group());
    auto leaf = triangle();

    auto material = MaterialPtr(new vn::graphics::Material());
    material->setMetallic(0.0F);
    leaf->setMaterial(material);
    root->addChild(leaf);

    Scene scene;
    scene.setRoot(root);
    const CollectView view = clipBoxView();

    const auto old_snapshot = SceneRows::snapshot(scene);
    std::vector<vn::graphics::RenderCommand> old_commands;
    bool                                     viewport_used = false;
    selectSnapshot(*old_snapshot, view, vn::graphics::kAllLayers, old_commands, viewport_used);
    ASSERT_EQ(old_commands.size(), 1U);
    const auto old_description = old_commands.front().description;
    const auto old_material    = old_commands.front().material_description;
    const auto old_program     = old_commands.front().program;
    ASSERT_NE(old_description, nullptr);
    ASSERT_NE(old_material, nullptr);
    EXPECT_EQ(old_program, nullptr);

    // Three edits through live objects: a material value, a geometry buffer, and the leaf's program.
    material->setMetallic(0.75F);
    std::vector<float> positions{ 0.0F, 0.0F, 0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F };
    leaf->setPositions(intrusive_ptr<const vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions))));
    leaf->bumpRevision();
    auto program = intrusive_ptr<vn::graphics::ShaderProgram>(new vn::graphics::ShaderProgram());
    program->setName(u8"snapshot-t17");
    leaf->setProgram(program);

    const auto fresh_snapshot = SceneRows::snapshot(scene);
    std::vector<vn::graphics::RenderCommand> fresh_commands;
    selectSnapshot(*fresh_snapshot, view, vn::graphics::kAllLayers, fresh_commands, viewport_used);
    ASSERT_EQ(fresh_commands.size(), 1U);
    EXPECT_NE(fresh_commands.front().description.get(), old_description.get()) << "a geometry revision is a new value";
    EXPECT_NE(fresh_commands.front().material_description.get(), old_material.get())
        << "a material revision is a new value";
    EXPECT_EQ(fresh_commands.front().program.get(), program.get()) << "the leaf's new program is what the row resolves";
    EXPECT_NE(fresh_commands.front().description.get(), nullptr);

    // The old snapshot is still the old picture after all three edits.
    std::vector<vn::graphics::RenderCommand> old_again;
    selectSnapshot(*old_snapshot, view, vn::graphics::kAllLayers, old_again, viewport_used);
    ASSERT_EQ(old_again.size(), 1U);
    EXPECT_EQ(old_again.front().description.get(), old_description.get());
    EXPECT_EQ(old_again.front().material_description.get(), old_material.get());
    EXPECT_EQ(old_again.front().program.get(), old_program.get());
}

TEST(SnapshotShadowTest, ADifferentSnapshotIsReportedAsADifference)
{
    // A gate that cannot fail is not a gate. Clearing the shadow's snapshot makes the two answers differ in size, and
    // the diff must say so - this is the mutation that proves the comparison is live.
    Assembly assembly = makeAssembly();
    Scene    scene;
    scene.setRoot(assembly.root);

    ContentMirror mirror(scene);
    mirror.setLegacyShadow(true);
    ASSERT_EQ(mirror.publish(scene).status, PublishStatus::Posted);
    mirror.beginFrame(1U);
    (void)mirror.drain();

    const CollectView view   = clipBoxView();
    const auto        legacy = mirror.legacySource().collectRenderCommands(view, vn::graphics::kAllLayers);
    ASSERT_FALSE(legacy->empty());

    mirror.snapshotSource().setSnapshot(nullptr);
    const auto         empty = mirror.snapshotSource().collectRenderCommands(view, vn::graphics::kAllLayers);
    const DrawListDiff diff  = diffDrawLists(*legacy, *empty);
    EXPECT_FALSE(diff.same);
    EXPECT_STREQ(diff.field, "count");
}
