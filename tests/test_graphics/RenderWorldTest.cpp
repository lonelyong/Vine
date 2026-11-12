#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
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

#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>

#include "DrawListBake.hpp"
#include "SceneRows.hpp"
#include "SceneSnapshot.hpp"

using vn::graphics::Aabbd;
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

    const CollectView view     = clipBoxView();
    const auto        snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(snapshot, nullptr);

    std::vector<RenderCommand> selected;
    bool                       viewport_used = false;
    vn::graphics::detail::selectSnapshot(*snapshot, view, vn::graphics::kAllLayers, selected, viewport_used);
    expectSameDrawList(selected, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
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
    const CollectView view = clipBoxView();

    const auto old_snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    std::vector<RenderCommand> old_commands;
    bool                       viewport_used = false;
    vn::graphics::detail::selectSnapshot(*old_snapshot, view, vn::graphics::kAllLayers, old_commands, viewport_used);
    ASSERT_EQ(old_commands.size(), 1U);
    const auto old_values = old_commands.front().material_description;
    ASSERT_NE(old_values, nullptr);
    EXPECT_FLOAT_EQ(old_values->values().metallic, 0.0F);

    material->setMetallic(0.75F);
    const auto fresh_snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    std::vector<RenderCommand> fresh_commands;
    vn::graphics::detail::selectSnapshot(*fresh_snapshot, view, vn::graphics::kAllLayers, fresh_commands, viewport_used);
    ASSERT_EQ(fresh_commands.size(), 1U);
    const auto fresh_values = fresh_commands.front().material_description;
    ASSERT_NE(fresh_values, nullptr);
    EXPECT_NE(fresh_values.get(), old_values.get()) << "a new material revision is a new immutable value";
    EXPECT_FLOAT_EQ(fresh_values->values().metallic, 0.75F);

    // The old snapshot is still the old picture after the edit.
    std::vector<RenderCommand> old_again;
    vn::graphics::detail::selectSnapshot(*old_snapshot, view, vn::graphics::kAllLayers, old_again, viewport_used);
    ASSERT_EQ(old_again.size(), 1U);
    EXPECT_EQ(old_again.front().material_description.get(), old_values.get());
    EXPECT_FLOAT_EQ(old_again.front().material_description->values().metallic, 0.0F);
}

TEST(SnapshotTest, MeasureWhatOneEditCostsToPublishAndSelect)
{
    // K1 / K3 FOR THE BRIDGE. `SceneRows::snapshot` still copies today's whole `SceneBake` and `ensureRows` still
    // rebuilds the whole row table when the graph epoch moves; this test exposes both halves separately. If the
    // total is far above K1's 500 us, the next step is the chunked hot/cold table - not a smaller copy.
    auto root = intrusive_ptr<Group>(new Group());
    for (std::size_t index = 0; index < 2000U; ++index) {
        root->addChild(triangle());
    }
    Scene scene;
    scene.setRoot(root);

    const CollectView view = clipBoxView();
    std::vector<RenderCommand> selected;
    bool                       viewport_used = false;
    const auto                 warm_snapshot = vn::graphics::detail::SceneRows::snapshot(scene); // warm the bake

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

        vn::graphics::detail::selectSnapshot(*snapshot, view, vn::graphics::kAllLayers, selected, viewport_used);
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
    EXPECT_EQ(selected.size(), 2000U);
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    EXPECT_LE(publish_avg, 250) << "N2: one edit at 2000 rows must publish within 250 us in an optimized build";
#endif
}

TEST(SnapshotTest, AnEditedTransformRepublishesAndMatchesTheScene)
{
    // The incremental row refresh must handle the common container edit (a placement change) without falling back
    // to the whole-table build, and the resulting snapshot must still answer the same list as the Scene.
    const Assembly assembly = makeAssembly();
    Scene          scene;
    scene.setRoot(assembly.root);

    const CollectView view   = clipBoxView();
    const auto        before = vn::graphics::detail::SceneRows::snapshot(scene);

    assembly.arm->setMatrix(vn::math::translate(0.25, 0.1, 0.0));
    const auto after = vn::graphics::detail::SceneRows::snapshot(scene);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(before.get(), after.get()) << "an edit is a new publication";

    std::vector<RenderCommand> selected;
    bool                       viewport_used = false;
    vn::graphics::detail::selectSnapshot(*after, view, vn::graphics::kAllLayers, selected, viewport_used);
    expectSameDrawList(selected, scene.collectRenderCommands(view, vn::graphics::kAllLayers));
}

TEST(SnapshotTest, MeasureWhatASnapshotKeepsResident)
{
    // K2: the snapshot must not cost more than 3x the row table it copies (N3's target is 2x). The measured delta
    // covers the chunk vectors and the DrawFacts allocations; the row-table estimate uses the same capacities, so the
    // ratio is the comparison the audit asks for rather than a machine-specific absolute byte count.
#if defined(__GLIBC__)
    auto root = intrusive_ptr<Group>(new Group());
    for (std::size_t index = 0; index < 2000U; ++index) {
        root->addChild(triangle());
    }
    Scene scene;
    scene.setRoot(root);

    const CollectView view = clipBoxView();
    (void)scene.collectRenderCommands(view, vn::graphics::kAllLayers); // build the rows (the baseline)

    const std::size_t before = mallinfo2().uordblks;
    const auto        snapshot = vn::graphics::detail::SceneRows::snapshot(scene);
    const std::size_t after = mallinfo2().uordblks;

    std::size_t row_bytes = 0U;
    for (const auto& chunk : snapshot->chunks) {
        row_bytes += chunk->capacity() * sizeof(vn::graphics::detail::Row);
        for (const auto& row : *chunk) {
            row_bytes += row.draws.capacity() * sizeof(vn::graphics::detail::DrawFacts);
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
    const Aabbd other_box = other_row->world_box;

    // The leaf's bound moves. Only its row and the rows above it are re-derived, and the other branch is untouched.
    leaf->half_extent = 0.9;
    EXPECT_EQ(refreshBoxes(bake), 1U) << "one leaf moved, so one box was re-read";

    const Row* leaf_row = row_of(leaf->id());
    ASSERT_NE(leaf_row, nullptr);
    EXPECT_NEAR(leaf_row->world_box.max().x, 0.9, 1e-12) << "the world-space answer the leaf gave, and nothing else";
    const Row* left_row = row_of(left->id());
    ASSERT_NE(left_row, nullptr);
    EXPECT_NEAR(left_row->world_box.max().x, 0.9, 1e-12) << "the chain above the changed leaf followed it";
    EXPECT_TRUE(row_of(other->id())->world_box == other_box) << "the other branch was not re-derived";

    // ...and it settles: a second look at the same table finds nothing to do.
    EXPECT_EQ(refreshBoxes(bake), 0U) << "and the next look folds nothing";
}
