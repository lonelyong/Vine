#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <malloc.h>
#endif

#include <vine/Buffer.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/View.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/StateNode.hpp>
#include <vine/graphics/AutoTransform.hpp>
#include <vine/graphics/ViewPlacement.hpp>

#include "DrawListBake.hpp"
#include "SceneRows.hpp"
#include "SceneSnapshot.hpp"

using vn::graphics::Aabbd;
using vn::graphics::View;
using vn::graphics::Geometry;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MatrixTransform;
using vn::graphics::DrawItem;
using vn::graphics::Scene;
using vn::graphics::detail::buildRows;
using vn::graphics::detail::refreshBoxes;
using vn::graphics::detail::refreshRow;
using vn::graphics::detail::Row;
using vn::graphics::detail::RowRefresh;
using vn::graphics::detail::SceneBake;
using vn::intrusive_ptr;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A view whose clip box is [-1, 1]^3 (identity projection and view), looking down -Z from z = 10.
///
/// The test content is authored to sit inside that box, so "is it drawn" needs no camera object: the two sides are
/// compared on the same view value, which is exactly the input a frame passes both of them.
View clipBoxView()
{
    View view;
    view.camera.eye = Vec3d(0.0, 0.0, 10.0);
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
void expectSameDrawList(const std::vector<DrawItem>& actual, const std::shared_ptr<const std::vector<DrawItem>>& expected)
{
    ASSERT_TRUE(expected != nullptr);
    ASSERT_EQ(actual.size(), expected->size()) << "the two sides drew a different number of commands";
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const DrawItem& lhs = actual[index];
        const DrawItem& rhs = (*expected)[index];
        EXPECT_EQ(lhs.geometry.get(), rhs.geometry.get()) << "command " << index << ": different geometry";
        EXPECT_EQ(lhs.primitive_index, rhs.primitive_index) << "command " << index << ": different primitive";
        EXPECT_EQ(lhs.program.get(), rhs.program.get()) << "command " << index << ": different program";
        EXPECT_EQ(lhs.material.get(), rhs.material.get()) << "command " << index << ": different material";
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

TEST(SnapshotTest, ThePureSelectMatchesTheScene)
{
    // THE BOUNDARY STEP 2 IS ABOUT. A snapshot is a value: selecting from it never asks the Scene, a Node or a
    // material for anything. For the same view it must answer exactly what the content-side collector answers,
    // command for command - that is the equivalence the migration rides on.
    const Assembly assembly = makeAssembly();
    Scene          scene;
    scene.setRoot(assembly.root);

    const View view     = clipBoxView();
    const auto snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(snapshot, nullptr);

    const std::vector<DrawItem> selected = vn::graphics::detail::selectSnapshot(*snapshot, view).commands;
    expectSameDrawList(selected, scene.collectDrawItems(view));
}

TEST(SnapshotTest, AnOlderSnapshotKeepsTheMaterialValuesItWasPublishedWith)
{
    // AUDIT A1 / K7. A material is edited through a live object, so nothing on the graph announces it. A snapshot
    // copied before the edit must keep the immutable MaterialDescription value it was published with; a snapshot
    // taken after the edit must carry the new revision. Without this, an in-flight frame would silently read the
    // new values and R9/I1 would be false.
    auto root = intrusive_ptr<Group>(new Group());
    auto leaf = triangle();

    auto material = vn::graphics::MaterialPtr(new vn::graphics::Material());
    material->setMetallic(0.0F);
    leaf->setMaterial(material);
    root->addChild(leaf);

    Scene scene;
    scene.setRoot(root);
    const View view = clipBoxView();

    const auto old_snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    const std::vector<DrawItem> old_commands = vn::graphics::detail::selectSnapshot(*old_snapshot, view).commands;
    ASSERT_EQ(old_commands.size(), 1U);
    const auto old_values = old_commands.front().material_description;
    ASSERT_NE(old_values, nullptr);
    EXPECT_FLOAT_EQ(old_values->values().metallic, 0.0F);

    material->setMetallic(0.75F);
    const auto fresh_snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    const std::vector<DrawItem> fresh_commands = vn::graphics::detail::selectSnapshot(*fresh_snapshot, view).commands;
    ASSERT_EQ(fresh_commands.size(), 1U);
    const auto fresh_values = fresh_commands.front().material_description;
    ASSERT_NE(fresh_values, nullptr);
    EXPECT_NE(fresh_values.get(), old_values.get()) << "a new material revision is a new immutable value";
    EXPECT_FLOAT_EQ(fresh_values->values().metallic, 0.75F);

    // The old snapshot is still the old picture after the edit.
    const std::vector<DrawItem> old_again = vn::graphics::detail::selectSnapshot(*old_snapshot, view).commands;
    ASSERT_EQ(old_again.size(), 1U);
    EXPECT_EQ(old_again.front().material_description.get(), old_values.get());
    EXPECT_FLOAT_EQ(old_again.front().material_description->values().metallic, 0.0F);
}

TEST(SnapshotTest, MeasureWhatOneEditCostsToPublishAndSelect)
{
    // K1 / N2 FOR THE BRIDGE, OBSERVED AS A SHAPE RATHER THAN A CLOCK. The claim is "one edit publishes in
    // O(changed)": `SceneRows::snapshot` copies the 32-row chunks that moved and shares the rest. The chunk count is
    // how that is checked, and it means the same thing on a fast machine and a slow one; the microseconds are
    // printed as the record (`../memory/graphics.md`), because an absolute time is a property of the hardware - the
    // rule `SelectTest`'s measurement states as "the times are the record, not a CI gate".
    auto root = intrusive_ptr<Group>(new Group());
    for (std::size_t index = 0; index < 2000U; ++index) {
        root->addChild(triangle());
    }
    Scene scene;
    scene.setRoot(root);

    const View view = clipBoxView();
    const auto warm_snapshot = vn::graphics::detail::SceneRows::snapshot(scene); // warm the bake
    std::vector<DrawItem> selected;

    constexpr int kRepetitions = 10;
    long long     publish_us   = 0;
    long long     select_us    = 0;
    vn::graphics::detail::SceneSnapshotPtr latest_snapshot;
    for (int step = 0; step < kRepetitions; ++step) {
        root->children().front()->setOpacity(0.5F + 0.001F * static_cast<float>(step));

        const auto before_publish = std::chrono::steady_clock::now();
        const auto snapshot       = vn::graphics::detail::SceneRows::snapshot(scene);
        const auto after_publish  = std::chrono::steady_clock::now();
        latest_snapshot           = snapshot;

        selected = vn::graphics::detail::selectSnapshot(*snapshot, view).commands;
        const auto after_select = std::chrono::steady_clock::now();

        publish_us += std::chrono::duration_cast<std::chrono::microseconds>(after_publish - before_publish).count();
        select_us += std::chrono::duration_cast<std::chrono::microseconds>(after_select - after_publish).count();
    }

    std::size_t shared_chunks = 0;
    for (std::size_t index = 0; index < warm_snapshot->chunks.size() && index < latest_snapshot->chunks.size(); ++index) {
        if (warm_snapshot->chunks[index] == latest_snapshot->chunks[index]) {
            ++shared_chunks;
        }
    }

    const long long publish_avg = publish_us / kRepetitions;
    const long long select_avg  = select_us / kRepetitions;
    std::printf("[measure] snapshot rows=%zu publish=%lld us select=%lld us shared=%zu/%zu\n",
                selected.size(),
                publish_avg,
                select_avg,
                shared_chunks,
                warm_snapshot->chunks.size());
    std::fflush(stdout);

    ASSERT_GT(warm_snapshot->chunks.size(), 1U) << "the fixture must span several chunks, or sharing says nothing";
    EXPECT_EQ(selected.size(), 2000U);
    EXPECT_NE(latest_snapshot.get(), warm_snapshot.get()) << "an edit is a new publication, never the cached value";
    EXPECT_EQ(shared_chunks, warm_snapshot->chunks.size() - 1U)
        << "one edit must share every chunk but the one it touched; K1 is that stopping, i.e. the whole table copied";
}

TEST(SnapshotTest, APlacementThatMovedOutsideTheFoldIsNotShared)
{
    // REVIEW 19 / Z5. A chunk is shared only while the select would read the same row: the fold's answer deliberately
    // does not carry the per-view placement, because two placement values can fold to the same box - so the share test
    // compares the row's own Fold input beside it. This case builds a bake by hand, swaps ONE row's placement WITHOUT
    // moving its folded answer (what a node that forgot to announce would look like), and asks for a publication: the
    // chunk must be copied, not served from the previous one.
    class IdlePlacement final : public vn::graphics::ViewPlacement
    {
      public:
        ~IdlePlacement() override = default;

        vn::graphics::ViewDependence declaredDependence() const noexcept override
        {
            return vn::graphics::ViewDependence::None;
        }
        vn::graphics::ViewDependence dependence(const View&) const noexcept override
        {
            return vn::graphics::ViewDependence::None;
        }
        vn::math::Mat4d localMatrix(const vn::math::Mat4d&, const View&) const override
        {
            return vn::math::Mat4d{};
        }
        Aabbd conservativeParentBox(const Aabbd& own_box) const override
        {
            return own_box;
        }
        bool isBounded() const noexcept override
        {
            return true;
        }
    };

    auto billboard = intrusive_ptr<vn::graphics::AutoTransform>(new vn::graphics::AutoTransform());
    billboard->setAutoRotateMode(vn::graphics::AutoTransform::AutoRotateMode::RotateToScreen);
    billboard->setPosition(vn::math::Point3d(0.0, 0.0, -3.0));
    billboard->addChild(triangle());

    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(billboard);
    for (int index = 0; index < 40; ++index) {
        root->addChild(triangle());
    }

    SceneBake bake;
    buildRows(*root, bake);
    const auto first =
        vn::graphics::detail::makeSceneSnapshot(bake, vn::graphics::detail::RowsVersion{ 1U, 1U, 0U, 0U }, nullptr);
    ASSERT_GE(first->chunks.size(), 2U) << "the fixture must span several chunks, or sharing says nothing";

    std::size_t billboard_row = 0U;
    for (; billboard_row < bake.rows.size(); ++billboard_row) {
        if (bake.rows[billboard_row].own.fold.placement != nullptr) {
            break;
        }
    }
    ASSERT_LT(billboard_row, bake.rows.size()) << "the bake read no per-view placement at all";
    ASSERT_LT(billboard_row, vn::graphics::detail::SceneSnapshot::kRowsPerChunk) << "the fixture puts the billboard in the first chunk";

    bake.rows[billboard_row].own.fold.placement = vn::graphics::ViewPlacementPtr(new IdlePlacement());
    const auto second =
        vn::graphics::detail::makeSceneSnapshot(bake, vn::graphics::detail::RowsVersion{ 1U, 2U, 0U, 0U }, first);

    EXPECT_EQ(second->row(billboard_row).own.fold.placement.get(), bake.rows[billboard_row].own.fold.placement.get())
        << "the publication shared a chunk whose select input moved: it would draw the previous placement";
    EXPECT_EQ(second->chunks.back(), first->chunks.back()) << "a chunk whose rows did not move is still shared";
}

TEST(SnapshotTest, AnEditedTransformRepublishesAndMatchesTheScene)
{
    // The incremental row refresh must handle the common container edit (a placement change) without falling back
    // to the whole-table build, and the resulting snapshot must still answer the same list as the Scene.
    const Assembly assembly = makeAssembly();
    Scene          scene;
    scene.setRoot(assembly.root);

    const View view   = clipBoxView();
    const auto        before = vn::graphics::detail::SceneRows::snapshot(scene);

    assembly.arm->setMatrix(vn::math::translate(0.25, 0.1, 0.0));
    const auto after = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(before.get(), after.get()) << "an edit is a new publication";

    const std::vector<DrawItem> selected = vn::graphics::detail::selectSnapshot(*after, view).commands;
    expectSameDrawList(selected, scene.collectDrawItems(view));
}

TEST(SnapshotTest, AnEditRefoldsEveryDescendantAcrossChunkBoundaries)
{
    // REVIEW 6 / P0-1. An edit re-folds the edited node's WHOLE subtree, but only the edited node and its ANCESTORS
    // see their content epoch move (Node::announceContentChange climbs up, never down). A chunk holding nothing but
    // descendants therefore passed the share test and was served from the previous publication, so part of a moved
    // subtree was drawn where it used to be - and the staleness persisted across later publications.
    //
    // The expectation is computed from the EDIT, not from a snapshot: every leaf below the edited transform sits at
    // x = 0.5 once the edit landed, whatever any chunk happens to hold.
    auto root = intrusive_ptr<Group>(new Group());
    auto arm  = placed(0.0, 0.0, 0.0);
    root->addChild(arm);

    std::vector<GeometryPtr> leaves;
    for (int index = 0; index < 40; ++index) { // arm + root + 40 leaves: the edited subtree spans more than one chunk
        GeometryPtr leaf = triangle();
        arm->addChild(leaf);
        leaves.push_back(leaf);
    }
    const GeometryPtr beside = triangle(); // the branch the edit does not touch, sharing the last chunk
    root->addChild(beside);

    Scene scene;
    scene.setRoot(root);
    const View view = clipBoxView();
    const auto before = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_GT(before->row_count, vn::graphics::detail::SceneSnapshot::kRowsPerChunk)
        << "a single-chunk fixture cannot see the share rule this test is about";

    arm->setMatrix(vn::math::translate(0.5, 0.0, 0.0));
    const auto after = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(after, nullptr);

    const std::vector<DrawItem> selected = vn::graphics::detail::selectSnapshot(*after, view).commands;

    std::size_t under_the_arm = 0;
    for (const DrawItem& command : selected) {
        if (command.node == beside->id()) {
            EXPECT_NEAR(command.model_matrix.element(0, 3), 0.0, 1e-12) << "the untouched branch stayed where it was";
            continue;
        }
        ++under_the_arm;
        EXPECT_NEAR(command.model_matrix.element(0, 3), 0.5, 1e-12)
            << "a leaf BELOW the edited node was served from the previous publication";
    }
    EXPECT_EQ(under_the_arm, leaves.size()) << "every leaf below the edited transform is drawn from the new picture";
}

TEST(SnapshotTest, AnEditBelowAStateNodeKeepsWhatTheStateNodeContributes)
{
    // REVIEW 6 / P0-2. The fold used to store the inherited state / material / program only on rows that DRAW, so a
    // container row held nothing - while the incremental path reads the parent's row back as the seed of a re-fold.
    // Editing a transform under a StateNode therefore dropped everything the StateNode contributed for the whole
    // edited subtree, and the command lost the state, the material and the program it was drawn with.
    auto root = intrusive_ptr<Group>(new Group());
    auto hud  = intrusive_ptr<vn::graphics::StateNode>(new vn::graphics::StateNode());
    auto arm  = placed(0.1, 0.0, 0.0);
    const GeometryPtr leaf = triangle();

    vn::graphics::DepthState depth;
    depth.test    = false;
    depth.write   = false;
    hud->setDepth(depth);
    auto material = vn::graphics::MaterialPtr(new vn::graphics::Material());
    material->setMetallic(0.0F);
    hud->setMaterial(material);

    arm->addChild(leaf);
    hud->addChild(arm);
    root->addChild(hud);

    Scene scene;
    scene.setRoot(root);
    const View view = clipBoxView();

    const auto before = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(before, nullptr);

    arm->setMatrix(vn::math::translate(0.2, 0.0, 0.0));
    const auto after = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(before.get(), after.get()) << "the edit has to reach the publication path";

    const std::vector<DrawItem> selected = vn::graphics::detail::selectSnapshot(*after, view).commands;
    ASSERT_EQ(selected.size(), 1U);

    const DrawItem& command = selected.front();
    EXPECT_NEAR(command.model_matrix.element(0, 3), 0.2, 1e-12) << "the edit itself landed";
    ASSERT_TRUE(command.authored_state.depth.has_value())
        << "the StateNode's depth state was dropped by the re-fold (a container row stored nothing to inherit)";
    EXPECT_FALSE(command.authored_state.depth->test) << "the state the ancestor stated is the state the command carries";
    EXPECT_FALSE(command.authored_state.depth->write);
    ASSERT_NE(command.material.get(), nullptr) << "the StateNode's material was dropped by the re-fold";
    EXPECT_EQ(command.material.get(), material.get());
}

TEST(SnapshotTest, MeasureWhatASnapshotKeepsResident)
{
    // K2: the snapshot must not cost more than 3x the row table it copies (N3's target is 2x). The measured delta
    // covers the chunk vectors and the ResolvedDraw allocations; the row-table estimate uses the same capacities, so the
    // ratio is the comparison the audit asks for rather than a machine-specific absolute byte count.
#if defined(__GLIBC__)
    auto root = intrusive_ptr<Group>(new Group());
    for (std::size_t index = 0; index < 2000U; ++index) {
        root->addChild(triangle());
    }
    Scene scene;
    scene.setRoot(root);

    const View view = clipBoxView();
    (void)scene.collectDrawItems(view); // build the rows (the baseline)

    const std::size_t before = mallinfo2().uordblks;
    const auto        snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    const std::size_t after = mallinfo2().uordblks;

    std::size_t row_bytes = 0U;
    for (const auto& chunk : snapshot->chunks) {
        row_bytes += chunk->capacity() * sizeof(vn::graphics::detail::Row);
        for (const auto& row : *chunk) {
            row_bytes += row.own.body.draws.capacity() * sizeof(vn::graphics::detail::ResolvedDraw);
        }
    }

    const std::size_t snapshot_bytes = after >= before ? after - before : 0U;
    const double      ratio          = row_bytes == 0U ? 0.0 : static_cast<double>(snapshot_bytes) / static_cast<double>(row_bytes);
    std::printf("[measure] snapshot resident rows=%zu row_bytes=%zu snapshot_bytes=%zu ratio=%.2f\n",
                snapshot->row_count,
                row_bytes,
                snapshot_bytes,
                ratio);
    std::fflush(stdout);
    EXPECT_LT(ratio, 3.0) << "K2: the snapshot must stay below 3x the row table";
    EXPECT_LE(ratio, 2.0) << "N3: target is at most 2x the row table";
#else
    GTEST_SKIP() << "K2 needs mallinfo2 (glibc)";
#endif
}

TEST(DrawListBakeTest, ARefreshFoldsOnlyWhatChanged)
{
    // A box climbs, but only when it MOVED: a collection over unchanged boxes folds nothing, and one changed leaf
    // costs its own answer plus the chain above it. That is the content side of the same rule a published snapshot
    // follows on an edit, and it is what keeps a streaming buffer's per-frame cost at its chain instead of the table.
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

    const auto row_of = [&bake](vn::graphics::NodeId identity) -> const Row* {
        for (const Row& row : bake.rows) {
            if (row.identity == identity) {
                return &row;
            }
        }
        return nullptr;
    };

    // Nothing announced since the build: a collection over unchanged boxes re-derives nothing at all.
    EXPECT_EQ(refreshBoxes(bake), 0U) << "an unchanged table folds nothing";

    const Row* other_row = row_of(other->id());
    ASSERT_NE(other_row, nullptr);
    const Aabbd other_box = other_row->resolved.world_box;

    // The leaf's bound moves. Only its row and the rows above it are re-derived, and the other branch is untouched.
    leaf->half_extent = 0.9;
    EXPECT_EQ(refreshBoxes(bake), 1U) << "one leaf moved, so one box was re-read";

    const Row* leaf_row = row_of(leaf->id());
    ASSERT_NE(leaf_row, nullptr);
    EXPECT_NEAR(leaf_row->resolved.world_box.max().x, 0.9, 1e-12) << "the world-space answer the leaf gave, and nothing else";
    const Row* left_row = row_of(left->id());
    ASSERT_NE(left_row, nullptr);
    EXPECT_NEAR(left_row->resolved.world_box.max().x, 0.9, 1e-12) << "the chain above the changed leaf followed it";
    EXPECT_TRUE(row_of(other->id())->resolved.world_box == other_box) << "the other branch was not re-derived";

    // ...and it settles: a second look at the same table finds nothing to do.
    EXPECT_EQ(refreshBoxes(bake), 0U) << "and the next look folds nothing";
}

TEST(DrawListBakeTest, ARefreshClassifiesWhatMoved)
{
    // `refreshRow` is the incremental edit path's one question: can the fold replay this change, or must the table be
    // rebuilt? The answer is derived from the two `RowOwn` groups (defaulted equality on `Fold` / `Body`), never from
    // a hand-written field list - which is what this test pins: a field added to `RowOwn` has to land on one side of
    // the line deliberately, not by being forgotten.
    auto              root = intrusive_ptr<Group>(new Group());
    auto              arm  = placed(0.1, 0.0, 0.0);
    const GeometryPtr leaf = triangle();
    arm->addChild(leaf);
    root->addChild(arm);

    SceneBake bake;
    buildRows(*root, bake);

    const auto index_of = [&bake](vn::graphics::NodeId identity) {
        for (std::size_t index = 0; index < bake.rows.size(); ++index) {
            if (bake.rows[index].identity == identity) {
                return static_cast<std::uint32_t>(index);
            }
        }
        return std::numeric_limits<std::uint32_t>::max();
    };
    const std::uint32_t arm_index  = index_of(arm->id());
    const std::uint32_t leaf_index = index_of(leaf->id());
    ASSERT_LT(arm_index, bake.rows.size());
    ASSERT_LT(leaf_index, bake.rows.size());

    // Nothing announced, nothing moved - and re-reading is what the table now holds.
    EXPECT_EQ(refreshRow(bake, arm_index), RowRefresh::NoChange) << "a row re-read from the same node is unchanged";

    // THE REPLAYABLE SET: placement, opacity, layers and draw order are what the fold re-derives from values.
    arm->setMatrix(vn::math::translate(0.25, 0.0, 0.0));
    EXPECT_EQ(refreshRow(bake, arm_index), RowRefresh::RefoldSubtree) << "a moved placement is a re-fold, not a rebuild";
    EXPECT_EQ(refreshRow(bake, arm_index), RowRefresh::NoChange) << "the row now holds the re-read value";

    arm->setOpacity(0.5F);
    EXPECT_EQ(refreshRow(bake, arm_index), RowRefresh::RefoldSubtree) << "opacity is part of the replayable set";

    leaf->setLayers(1U << 3U);
    EXPECT_EQ(refreshRow(bake, leaf_index), RowRefresh::RefoldSubtree) << "a layer declaration is part of it too";

    // EVERYTHING ELSE changes the row's meaning: only a rebuilt table can carry it.
    auto material = vn::graphics::MaterialPtr(new vn::graphics::Material());
    leaf->setMaterial(material);
    EXPECT_EQ(refreshRow(bake, leaf_index), RowRefresh::RebuildTable) << "a new material cannot be replayed";

    // An index that names no row cannot be refreshed: the conservative answer is the rebuild.
    EXPECT_EQ(refreshRow(bake, static_cast<std::uint32_t>(bake.rows.size())), RowRefresh::RebuildTable);
}
