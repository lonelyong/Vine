#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>

#include "RenderWorld.hpp"

using vn::graphics::CollectView;
using vn::graphics::Geometry;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MatrixTransform;
using vn::graphics::RenderCommand;
using vn::graphics::Scene;
using vn::graphics::detail::buildRows;
using vn::graphics::detail::RenderWorld;
using vn::graphics::detail::SceneBake;
using vn::intrusive_ptr;
using vn::math::Mat4d;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A view whose clip box is [-1, 1]^3 (identity projection and view), looking down -Z from z = 10.
///
/// The test content is authored to sit inside that box, so "is it drawn" needs no camera object: the two sides are
/// compared on the same view value, which is exactly the input a frame passes both of them.
CollectView clipBoxView()
{
    CollectView view;
    view.eye = Vec3d(0.0, 0.0, 10.0);
    return view;
}

/// @brief One triangle-list draw, authored directly (positions only, one primitive).
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

/// @brief Asserts @p actual is @p expected draw for draw, field for field.
void expectSameDrawList(const std::vector<RenderCommand>& actual, const std::shared_ptr<const std::vector<RenderCommand>>& expected)
{
    ASSERT_TRUE(expected != nullptr);
    ASSERT_EQ(actual.size(), expected->size()) << "the two sides drew a different number of commands";
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const RenderCommand& lhs = actual[index];
        const RenderCommand& rhs = (*expected)[index];
        EXPECT_EQ(lhs.geometry.get(), rhs.geometry.get()) << "command " << index << ": different geometry";
        EXPECT_EQ(lhs.primitive_index, rhs.primitive_index) << "command " << index << ": different primitive";
        EXPECT_EQ(lhs.program.get(), rhs.program.get()) << "command " << index << ": different program";
        EXPECT_FLOAT_EQ(lhs.opacity, rhs.opacity) << "command " << index << ": different opacity";
        EXPECT_EQ(lhs.draw_order, rhs.draw_order) << "command " << index << ": different draw order";
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(lhs.model_matrix(row, column), rhs.model_matrix(row, column))
                    << "command " << index << ": model matrix element (" << row << ", " << column << ")";
            }
        }
    }
}

/// @brief A root with one two-deep chain and a second leaf beside it: a small articulated assembly.
struct Assembly {
    intrusive_ptr<Group>           root;
    intrusive_ptr<MatrixTransform> arm;
    intrusive_ptr<MatrixTransform> link;
    GeometryPtr                    link_geometry;
    GeometryPtr                    loose_geometry;
};

Assembly makeAssembly()
{
    Assembly assembly;
    assembly.root          = intrusive_ptr<Group>(new Group());
    assembly.arm           = placed(0.2, 0.0, 0.0);
    assembly.link          = placed(0.1, 0.0, 0.0);
    assembly.link_geometry = triangle();
    assembly.loose_geometry = triangle();
    assembly.root->addChild(assembly.arm);
    assembly.arm->addChild(assembly.link);
    assembly.link->addChild(assembly.link_geometry);
    assembly.root->addChild(assembly.loose_geometry);
    return assembly;
}

} // namespace

TEST(RenderWorldTest, TheMirrorCollectsWhatTheContentCollects)
{
    const Assembly assembly = makeAssembly();
    Scene          scene;
    scene.setRoot(assembly.root);

    SceneBake  rows;
    buildRows(*assembly.root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));
    EXPECT_EQ(world.rowCount(), 5U) << "the root, the arm, the link and the two leaves";

    const CollectView view = clipBoxView();
    std::vector<RenderCommand> mirrored;
    world.collect(view, vn::graphics::kAllLayers, mirrored);

    expectSameDrawList(mirrored, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
    EXPECT_EQ(mirrored.size(), 2U) << "two leaves, two draws";
}

TEST(RenderWorldTest, APlacementEditReplacesThatSubtreeAndNothingElse)
{
    Assembly assembly = makeAssembly();
    Scene    scene;
    scene.setRoot(assembly.root);

    SceneBake  rows;
    buildRows(*assembly.root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    const CollectView view = clipBoxView();
    const std::uint64_t before = world.rowsRecomputed();

    // The host's own edit, and the value it hands over: the mirror never reads the node.
    const Mat4d moved = vn::math::translate(0.05, 0.25, 0.0);
    assembly.arm->setMatrix(moved);
    ASSERT_TRUE(world.place(assembly.arm.get(), moved));
    EXPECT_EQ(world.rowsRecomputed() - before, 3U) << "the arm, its link and the link's leaf - not the whole assembly";

    std::vector<RenderCommand> mirrored;
    world.collect(view, vn::graphics::kAllLayers, mirrored);
    expectSameDrawList(mirrored, scene.collectRenderCommands(view, vn::graphics::kAllLayers));

    // The equivalence above compares two users of one fold, so it cannot see a fold that is wrong for BOTH. The
    // absolute placement can: the link hangs from the arm, which hangs from an identity root, so its draw matrix
    // must translate by the two offsets - the applied one and the authored one.
    const RenderCommand* link_draw = nullptr;
    for (const RenderCommand& command : mirrored) {
        if (command.geometry.get() == assembly.link_geometry.get()) {
            link_draw = &command;
        }
    }
    ASSERT_NE(link_draw, nullptr) << "the link's draw is in the list";
    EXPECT_NEAR(link_draw->model_matrix.element(0, 3), 0.05 + 0.1, 1e-12) << "the arm the test moved, and the link's own offset";
    EXPECT_NEAR(link_draw->model_matrix.element(1, 3), 0.25, 1e-12) << "the arm the test moved";
}

TEST(RenderWorldTest, TheMirrorMovesOnlyWhenItIsTold)
{
    Assembly assembly = makeAssembly();
    Scene    scene;
    scene.setRoot(assembly.root);

    SceneBake  rows;
    buildRows(*assembly.root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    const CollectView view = clipBoxView();
    std::vector<RenderCommand> before_mirror;
    world.collect(view, vn::graphics::kAllLayers, before_mirror);
    ASSERT_EQ(before_mirror.size(), 2U);

    // The content moves; the mirror is not told. That is the contract, not a bug: a delta is what carries a change,
    // and reading the node behind the caller's back is exactly what this side may not do.
    assembly.arm->setMatrix(vn::math::translate(-0.2, 0.0, 0.0));

    std::vector<RenderCommand> after_mirror;
    world.collect(view, vn::graphics::kAllLayers, after_mirror);
    ASSERT_EQ(after_mirror.size(), 2U);
    for (std::size_t index = 0; index < after_mirror.size(); ++index) {
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(after_mirror[index].model_matrix(row, column), before_mirror[index].model_matrix(row, column));
            }
        }
    }
}

TEST(RenderWorldTest, AnEditItCannotAnswerForIsRefused)
{
    Assembly assembly = makeAssembly();
    SceneBake  rows;
    buildRows(*assembly.root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    // A node this world was not built from: it has no row, so there is nothing to re-place.
    const auto        stranger = placed(1.0, 0.0, 0.0);
    const Mat4d       moved    = vn::math::translate(1.0, 0.0, 0.0);
    EXPECT_FALSE(world.place(stranger.get(), moved));
    EXPECT_EQ(world.refusals(), 1U);

    // Building from another graph starts a new world: the old rows (and their shares) are gone.
    SceneBake other;
    buildRows(*stranger, other);
    world.adopt(std::move(other));
    EXPECT_EQ(world.rowCount(), 1U);
    EXPECT_TRUE(world.place(stranger.get(), moved));
}
