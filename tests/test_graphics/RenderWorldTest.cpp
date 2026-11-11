#include <gtest/gtest.h>

#include <memory>
#include <utility>
#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/AutoTransform.hpp>
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
using vn::graphics::detail::refreshBoxes;
using vn::graphics::detail::Row;
using vn::graphics::Aabbd;
using vn::graphics::detail::RenderWorld;
using vn::graphics::detail::RenderWorldSource;
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

/// @brief A view whose world-to-view rotation is NOT the identity, so a billboard's auto rotation is visible.
CollectView tiltedView()
{
    CollectView view;
    view.view       = vn::math::rotate(Vec3d(0.0, 1.0, 0.0), 0.6); // world -> view
    view.eye        = Vec3d(3.0, 2.0, 10.0);
    view.projection = Mat4d{}; // clip box [-1, 1]^3
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

/**
 * @brief A leaf the engine cannot see inside: it states its bound in WORLD space only, at a size the test sets.
 *
 * @details
 * - A Geometry subclass answers `subtreeBounds` with false (see Node::subtreeBounds), so its row can only be built
 *   from the world-space answer `boundingBox()` gives. Driving that answer is how a test moves a box without touching
 *   a single vertex.
 */
class WorldAnswerLeaf : public Geometry {
  public:
    double half_extent{ 0.5 };

    /**
     * @brief Answers the world-space box the test set (this leaf states nothing in its own frame).
     *
     * @return The cube around the origin with the current half extent.
     */
    Aabbd boundingBox() const override
    {
        Aabbd box = Aabbd::empty();
        box.expandBy(vn::math::Point3d(-half_extent, -half_extent, -half_extent));
        box.expandBy(vn::math::Point3d(half_extent, half_extent, half_extent));
        return box;
    }
};

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

TEST(DrawListBakeTest, ARefreshFoldsOnlyWhatChanged)
{
    // A box climbs, but only when it MOVED: a collection over unchanged boxes folds nothing, and one changed leaf
    // costs its own answer plus the chain above it. That is the content side of the same rule the mirror follows on
    // an edit (see RenderWorldTest.AMovedSubtreeIsCulledByItsAncestorsNewBox), and it is what keeps a streaming
    // buffer's per-frame cost at its chain instead of the table.
    auto root  = intrusive_ptr<Group>(new Group());
    auto left  = placed(-0.6, 0.0, 0.0);
    auto right = placed(0.6, 0.0, 0.0);
    auto leaf  = intrusive_ptr<WorldAnswerLeaf>(new WorldAnswerLeaf());
    const GeometryPtr other = triangle(); // the engine's own leaf: answers in its own frame
    left->addChild(leaf);
    right->addChild(other);
    root->addChild(left);
    root->addChild(right);

    SceneBake bake;
    buildRows(*root, bake);

    const auto row_of = [&bake](const void* identity) -> const Row* {
        for (const Row& row : bake.rows) {
            if (row.identity == identity) {
                return &row;
            }
        }
        return nullptr;
    };

    // Nothing announced since the build: a collection over unchanged boxes re-derives nothing at all.
    EXPECT_EQ(refreshBoxes(bake), 0U) << "an unchanged table folds nothing";

    const Row* other_row = row_of(other.get());
    ASSERT_NE(other_row, nullptr);
    const Aabbd other_box = other_row->world_box;

    // The leaf's bound moves. Only its row and the rows above it are re-derived, and the other branch is untouched.
    leaf->half_extent = 0.9;
    EXPECT_EQ(refreshBoxes(bake), 1U) << "one leaf moved, so one box was re-read";

    const Row* leaf_row = row_of(leaf.get());
    ASSERT_NE(leaf_row, nullptr);
    EXPECT_NEAR(leaf_row->world_box.max().x, 0.9, 1e-12) << "the world-space answer the leaf gave, and nothing else";
    const Row* left_row = row_of(left.get());
    ASSERT_NE(left_row, nullptr);
    EXPECT_NEAR(left_row->world_box.max().x, 0.9, 1e-12) << "the chain above the changed leaf followed it";
    EXPECT_TRUE(row_of(other.get())->world_box == other_box) << "the other branch was not re-derived";

    // ...and it settles: a second look at the same table finds nothing to do.
    EXPECT_EQ(refreshBoxes(bake), 0U) << "and the next look folds nothing";
}

TEST(RenderWorldTest, TheMirrorCollectsWhatTheContentCollects)
{    const Assembly assembly = makeAssembly();
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

TEST(RenderWorldTest, TheMirrorIsADrawListSourceAndAnswersAFramesViewTheSameWay)
{
    const Assembly assembly = makeAssembly();
    Scene          scene;
    scene.setRoot(assembly.root);

    SceneBake  rows;
    buildRows(*assembly.root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    // THE FRAME'S OWN QUESTION, asked of both sides. This is the seam a frame expands through
    // (DrawDescription::content): the request names a view and who answers it, so "the two worlds agree" is not two
    // collectors compared - it is the answer a frame would get either way.
    vn::graphics::DrawListSource&            content = scene;
    RenderWorldSource                        mirror{ world };
    const CollectView                        view        = clipBoxView();
    const std::shared_ptr<const std::vector<RenderCommand>> from_content = content.collectRenderCommands(view, vn::graphics::kAllLayers);
    const std::shared_ptr<const std::vector<RenderCommand>> from_mirror  = mirror.collectRenderCommands(view, vn::graphics::kAllLayers);

    expectSameDrawList(*from_mirror, from_content);
    EXPECT_EQ(from_mirror->size(), 2U) << "two leaves, two draws, through either source";
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

TEST(RenderWorldTest, AMovedSubtreeIsCulledByItsAncestorsNewBox)
{
    // A box is a UNION up the chain, so re-placing a row changes every ancestor's box too. If the fold stops at the
    // edited row, the ancestors keep the box they had before the move: a subtree that moved INTO view is then pruned
    // by a box that still says it is somewhere else, and the object silently disappears - the same failure a missed
    // announcement causes, one level up.
    auto root      = intrusive_ptr<Group>(new Group());
    auto container = intrusive_ptr<Group>(new Group());
    const Mat4d away = vn::math::translate(4.0, 0.0, 0.0); // far outside the clip box [-1, 1]^3
    auto arm       = placed(away.element(0, 3), away.element(1, 3), away.element(2, 3));
    const GeometryPtr face = triangle();
    arm->addChild(face);
    container->addChild(arm);
    root->addChild(container);

    Scene scene;
    scene.setRoot(root);

    SceneBake  rows;
    buildRows(*root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    const CollectView view = clipBoxView();
    std::vector<RenderCommand> mirrored;
    world.collect(view, vn::graphics::kAllLayers, mirrored);
    ASSERT_TRUE(mirrored.empty()) << "it starts out of view";

    // The host moves the leaf's chain into view and tells the mirror about the transform it moved.
    const Mat4d inside = Mat4d{};
    arm->setMatrix(inside);
    ASSERT_TRUE(world.place(arm.get(), inside));

    std::vector<RenderCommand> after;
    world.collect(view, vn::graphics::kAllLayers, after);
    expectSameDrawList(after, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
    EXPECT_EQ(after.size(), 1U) << "a row moved into view must not be pruned by its ancestors' stale boxes";

    // ...and moving it back out must prune it again, through every box above it.
    arm->setMatrix(away);
    ASSERT_TRUE(world.place(arm.get(), away));
    std::vector<RenderCommand> returned;
    world.collect(view, vn::graphics::kAllLayers, returned);
    expectSameDrawList(returned, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
    EXPECT_TRUE(returned.empty());
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

TEST(RenderWorldTest, APerViewRowDrawsTheSameInAMirrorThatOwnsNoNode)
{
    // A billboard's placement is a function of the CAMERA, and a mirror owns no node to ask. The row carries that
    // placement as a value (ViewPlacement), so both sides resolve it from the view they are handed - which is the case
    // the rows could not express before: the run used to be handed back to the graph walk.
    auto root = intrusive_ptr<Group>(new Group());
    auto billboard = intrusive_ptr<vn::graphics::AutoTransform>(new vn::graphics::AutoTransform());
    billboard->setPosition(vn::math::Point3d(0.2, 0.1, 0.0));
    billboard->setAutoRotateMode(vn::graphics::AutoTransform::AutoRotateMode::RotateToScreen);
    const GeometryPtr face = triangle();
    billboard->addChild(face);
    root->addChild(billboard);

    Scene scene;
    scene.setRoot(root);

    SceneBake  rows;
    buildRows(*root, rows);
    RenderWorld world;
    world.adopt(std::move(rows));

    const CollectView         view = tiltedView();
    std::vector<RenderCommand> mirrored;
    world.collect(view, vn::graphics::kAllLayers, mirrored);
    ASSERT_EQ(mirrored.size(), 1u);
    expectSameDrawList(mirrored, scene.collectRenderCommands(view, vn::graphics::kAllLayers));

    // The equivalence compares two users of one rule, so it cannot see a rule that does nothing for both. The draw
    // matrix is NOT the authored placement: the auto rotation cancels the view's own turn.
    EXPECT_GT(std::abs(mirrored.front().model_matrix.element(0, 2)), 0.1) << "the billboard turned, it is not the identity";

    // A billboard MOVES as a value too, and the value is the placement itself: a delta that carried only the local
    // matrix would leave the mirror drawing the old orientation.
    billboard->setPosition(vn::math::Point3d(-0.3, 0.2, 0.0));
    ASSERT_TRUE(world.place(billboard.get(), billboard->matrix(), billboard->viewPlacement()));
    std::vector<RenderCommand> after;
    world.collect(view, vn::graphics::kAllLayers, after);
    expectSameDrawList(after, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
    ASSERT_EQ(after.size(), 1u);
    EXPECT_NEAR(after.front().model_matrix.element(0, 3), -0.3, 1e-12) << "the value the mirror was told, not the node's";
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
