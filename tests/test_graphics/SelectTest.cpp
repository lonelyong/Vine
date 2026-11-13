#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include <vine/math/Point3.hpp>
#include <vine/math/Transform3.hpp>

#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/MaterialBase.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/Viewport.hpp>

#include "DrawListBake.hpp"
#include "DrawListDiff.hpp"
#include "SceneRows.hpp"
#include "SceneSnapshot.hpp"

using vn::geom::spatial::Aabbd;
using vn::graphics::CollectView;
using vn::graphics::GeometryPtr;
using vn::graphics::Material;
using vn::graphics::MatrixTransform;
using vn::graphics::RenderCommand;
using vn::graphics::Scene;
using vn::graphics::Viewport;
using vn::graphics::detail::diffDrawLists;
using vn::graphics::detail::Frustum;
using vn::graphics::detail::makeSortKey;
using vn::graphics::detail::Row;
using vn::graphics::detail::SceneRows;
using vn::graphics::detail::selectSnapshot;
using vn::graphics::detail::SortKey;
using vn::intrusive_ptr;
using vn::math::Mat4d;
using vn::math::Point3d;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/** @brief A key by intent (field order: draw order, layer, transparency, depth, tie). */
SortKey key(std::int32_t order, std::uint32_t layer, bool transparent, std::int64_t depth, std::uint64_t tie)
{
    return SortKey{ order, layer, transparent, depth, tie };
}

/** @brief One triangle geometry (positions only, one primitive). */
GeometryPtr triangleGeometry()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.6F, 0.0F, 0.0F), Vec3f(0.0F, 0.6F, 0.0F) });
    mesh->bumpRevision();
    return vn::graphics::geometryFromShape(*mesh);
}

/**
 * @brief Builds a scene whose rows exercise every part of the order (draw order, layer, transparency, depth).
 *
 * Varied content is what makes "the same input answers the same list" meaningful: an order that depends on anything
 * the list does not control would show up as a different sequence between two runs.
 */
void buildSelectScene(Scene& scene)
{
    auto root = intrusive_ptr<vn::graphics::Group>(new vn::graphics::Group());
    scene.setRoot(root);

    auto transparent = intrusive_ptr<Material>(new Material());
    transparent->setAlphaMode(vn::graphics::AlphaMode::Blend);

    for (int index = 0; index < 40; ++index) {
        auto node     = intrusive_ptr<MatrixTransform>(new MatrixTransform());
        auto geometry = triangleGeometry();
        if (index % 5 == 0) {
            geometry->setMaterial(transparent);
        }
        if (index % 7 == 0) {
            node->setDrawOrder(1);
        }
        if (index % 6 == 0) {
            node->setLayers(1U << 1);
        }
        node->setMatrix(vn::math::translate(Vec3d((index % 8) * 0.4 - 1.6, (index / 8) * 0.4 - 1.0, -3.0 - (index % 5))));
        node->addChild(geometry);
        root->addChild(node);
    }
}

/** @brief A view value looking at @p target from @p eye, with a stated device-pixel rectangle. */
CollectView viewFrom(const Vec3d& eye, const Vec3d& target, const Viewport& viewport)
{
    auto camera = intrusive_ptr<vn::graphics::Camera>(new vn::graphics::Camera());
    camera->lookAt(eye, target, Vec3d(0.0, 1.0, 0.0));
    camera->setPerspective(60.0, 0.1, 100.0);

    const vn::graphics::CameraSnapshot value = vn::graphics::CameraSnapshot::of(*camera);
    CollectView                             view;
    view.projection = value.projection;
    view.view       = value.view;
    view.eye        = value.eye;
    view.up         = value.up;
    view.viewport   = viewport;
    return view;
}

} // namespace

/*
 * @brief T9: the sort key is a TOTAL order - any permutation of the same commands answers the same list.
 *
 * This is what a stable sort does NOT give: with equal keys, the result would be the input order, so two producers (or
 * two machines) that walk the same content in a different order would draw it differently. The tie field is what
 * makes the order a property of the commands instead (see SortKey).
 */
TEST(SelectTest, TheSortKeyIsATotalOrder)
{
    const std::vector<SortKey> keys{
        key(0, 0, false, 5, 1),
        key(0, 0, false, 10, 2),
        key(0, 0, true, 10, 3),
        key(0, 0, true, 5, 4),
        key(0, 1, false, 1, 5),
        key(1, 0, false, 1, 6),
        key(0, 0, false, 5, 7), // same place in the order as key 0, a different command
        key(-1, 3, true, 0, 8),
    };

    std::vector<SortKey> expected = keys;
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(std::adjacent_find(expected.begin(), expected.end()) == expected.end(), true)
        << "no two keys compare equal: the tie field is unique per command";

    std::mt19937 rng(20261113U);
    for (int round = 0; round < 64; ++round) {
        std::vector<SortKey> shuffled = keys;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::sort(shuffled.begin(), shuffled.end());
        EXPECT_EQ(shuffled, expected) << "round " << round << ": a permutation is not a different picture";
    }
}

/*
 * @brief The field order the key documents: draw order, then layer, then the segment, then depth, then the tie.
 */
TEST(SelectTest, TheKeyReadsDrawOrderThenLayerThenSegmentThenDepth)
{
    EXPECT_LT(key(0, 9, true, 100, 1), key(1, 0, false, 0, 2)) << "the declared draw order dominates everything";
    EXPECT_LT(key(0, 0, true, 100, 1), key(0, 1, false, 0, 2)) << "the layer dominates the segment and the depth";
    EXPECT_LT(key(0, 0, false, 100, 1), key(0, 0, true, 0, 2)) << "opaque is drawn before transparent";
    EXPECT_LT(key(0, 0, false, 5, 2), key(0, 0, false, 6, 1)) << "opaque front-to-back (closer first)";
    EXPECT_LT(key(0, 0, true, 6, 2), key(0, 0, true, 5, 1)) << "transparent back-to-front (farther first)";
    EXPECT_LT(key(0, 0, false, 5, 1), key(0, 0, false, 5, 2)) << "the tie breaks two otherwise equal commands";
    EXPECT_FALSE(key(0, 0, false, 5, 1) < key(0, 0, false, 5, 1)) << "a key is not less than itself";
}

/*
 * @brief The representative point is the world box CENTRE, not the node's origin.
 *
 * A CAD part that keeps its original coordinates has its local origin far from the geometry, and a key taken there
 * orders the part by a point that is not on it - the defect `.ai/bugs/graphics-transparent-sort-uses-origin.md`
 * records. Here the NEAR object's node origin is farther than the far object's, so an origin-based key would answer
 * the opposite of this expectation.
 */
TEST(SelectTest, TheKeySortsByTheBoxCentreNotTheNodeOrigin)
{
    const Mat4d view; // identity: the camera sits at the origin and looks down its own -Z

    RenderCommand near_command;
    near_command.model_matrix = vn::math::translate(0.0, 0.0, -100.0); // its origin is the FARTHEST point in the test
    RenderCommand far_command;
    far_command.model_matrix = vn::math::translate(0.0, 0.0, 0.0);

    const Aabbd near_box(Point3d(-1.0, -1.0, -6.0), Point3d(1.0, 1.0, -4.0)); // centre z = -5
    const Aabbd far_box(Point3d(-1.0, -1.0, -11.0), Point3d(1.0, 1.0, -9.0)); // centre z = -10

    const SortKey near_key = makeSortKey(near_command, 1U, near_box, view, 1U);
    const SortKey far_key  = makeSortKey(far_command, 1U, far_box, view, 2U);

    EXPECT_LT(near_key.depth, far_key.depth) << "camera-space depth grows away from the eye";
    EXPECT_LT(near_key, far_key) << "the box centre decides; the node origin does not";
}

/*
 * @brief A row with no box falls back to its own origin: an approximation, but a defined one.
 */
TEST(SelectTest, ACommandWithNoBoxFallsBackToItsOrigin)
{
    const Mat4d view;

    RenderCommand command;
    command.model_matrix = vn::math::translate(0.0, 0.0, -7.0);

    const SortKey keyed = makeSortKey(command, 1U, Aabbd::empty(), view, 1U);
    EXPECT_EQ(keyed.depth, static_cast<std::int64_t>(std::llround(7.0 * SortKey::kDepthStep))) << "origin z = -7 is 7 in front";
}

/*
 * @brief T8: select is a PURE function of (snapshot, view, mask) - same input, same list, in any order, from any
 * thread, and after the live content it came from is gone.
 *
 * Two properties are proved here, and they are what "the derivation can run in parallel" (design 2.9 / R14) rests on:
 *   - REPEATABLE: the same snapshot and view answer the same list, and another view in between is not state;
 *   - INDEPENDENT OF THE CONTENT: the rows are values, so emptying the scene changes no answer - if select touched a
 *     Scene / Node / material, this would draw a different (or no) picture.
 * The parallel half is the point of the test: eight callers share one immutable snapshot, each with its own output, and
 * every answer must equal the serial one. It deliberately calls detail::selectSnapshot directly - SnapshotSource's
 * per-view memo is the ENGINE thread's, and testing it here would measure the memo's model, not the pure function.
 */
TEST(SelectTest, SameInputSameList)
{
    Scene scene;
    buildSelectScene(scene);
    const vn::graphics::detail::SceneSnapshotPtr snapshot = SceneRows::snapshot(scene);
    ASSERT_GT(snapshot->row_count, 0U);

    const CollectView view_a = viewFrom(Vec3d(0.0, 0.0, 10.0), Vec3d(0.0, 0.0, -3.0), Viewport{ 0, 0, 800, 600 });
    const CollectView view_b = viewFrom(Vec3d(7.0, 5.0, 12.0), Vec3d(0.0, 0.0, 0.0), Viewport{ 0, 0, 320, 240 });

    bool                       viewport_used = false;
    std::vector<RenderCommand> first;
    selectSnapshot(*snapshot, view_a, vn::graphics::kAllLayers, first, viewport_used);
    ASSERT_FALSE(first.empty()) << "the fixture is in view: an empty list would make 'same' trivial";

    std::vector<RenderCommand> again;
    selectSnapshot(*snapshot, view_a, vn::graphics::kAllLayers, again, viewport_used);
    const auto repeated = diffDrawLists(again, first);
    EXPECT_TRUE(repeated.same) << "same snapshot, same view: a different list at index " << repeated.index << " ("
                               << repeated.field << ")";

    std::vector<RenderCommand> other_view;
    selectSnapshot(*snapshot, view_b, vn::graphics::kAllLayers, other_view, viewport_used);
    std::vector<RenderCommand> back_to_a;
    selectSnapshot(*snapshot, view_a, vn::graphics::kAllLayers, back_to_a, viewport_used);
    const auto interleaved = diffDrawLists(back_to_a, first);
    EXPECT_TRUE(interleaved.same) << "a different view in between is not state (index " << interleaved.index << ")";

    // THE LIVE TREE IS NOT AN INPUT: after the scene stops drawing everything, the snapshot still answers its own
    // picture. Its rows hold shares, and select never asks a Scene, a Node or a material for a value.
    scene.setRoot(nullptr);
    std::vector<RenderCommand> from_the_value;
    selectSnapshot(*snapshot, view_a, vn::graphics::kAllLayers, from_the_value, viewport_used);
    const auto detached = diffDrawLists(from_the_value, first);
    EXPECT_TRUE(detached.same) << "select reads the publication, not the content it was published from (index "
                               << detached.index << ")";

    // PARALLEL: eight callers share the snapshot, each with its own output, all compared with the SERIAL answer. No
    // assertion runs on a worker thread (a gtest expectation is not a cross-thread channel): failures are counted and
    // the first offending thread is named on this one.
    constexpr int            kThreads = 8;
    constexpr int            kRounds  = 32;
    std::atomic<int>         mismatches{ 0 };
    std::atomic<int>         first_bad_thread{ -1 };
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        workers.emplace_back([&, thread] {
            for (int round = 0; round < kRounds; ++round) {
                const CollectView&                view     = (round % 2 == 0) ? view_a : view_b;
                const std::vector<RenderCommand>& expected = (round % 2 == 0) ? first : other_view;
                std::vector<RenderCommand>        answer;
                bool                              used = false;
                selectSnapshot(*snapshot, view, vn::graphics::kAllLayers, answer, used);
                if (!diffDrawLists(answer, expected).same) {
                    int none = -1;
                    first_bad_thread.compare_exchange_strong(none, thread);
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(mismatches.load(), 0) << "thread " << first_bad_thread.load()
                                    << " answered a different list than the serial select";
}

/*
 * @brief T24: culling is decided PER ROW, never per block - one chunk may hold segments under different parents.
 *
 * `SceneSnapshot` shares its rows in fixed-size chunks (`kRowsPerChunk`), and a chunk is a SHARING unit, not a culling
 * unit: two rows that happen to sit in one have nothing in common but their position in the walk. The fixture puts
 * the OUT-OF-VIEW segment FIRST in walk order on purpose - a block-level skip whose bound came from the block's first
 * row (the `chain[0]` shortcut the hot-path research rejected) would answer an empty list here while the camera looks
 * straight at the second segment. The last half turns the camera around and expects the other segment, so "culling
 * switched off" does not pass either.
 */
TEST(SelectTest, OneChunkCullsEachRowOnItsOwnBox)
{
    Scene scene;
    auto  root = intrusive_ptr<vn::graphics::Group>(new vn::graphics::Group());
    scene.setRoot(root);

    const GeometryPtr outside_geometry = triangleGeometry();
    auto              outside_parent   = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    outside_parent->setMatrix(vn::math::translate(Vec3d(1000.0, 0.0, -3.0)));
    outside_parent->addChild(outside_geometry);
    root->addChild(outside_parent); // first in walk order: the block's first segment is the one OUT of view

    const GeometryPtr inside_geometry = triangleGeometry();
    auto              inside_parent   = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    inside_parent->setMatrix(vn::math::translate(Vec3d(0.0, 0.0, -3.0)));
    inside_parent->addChild(inside_geometry);
    root->addChild(inside_parent);

    const vn::graphics::detail::SceneSnapshotPtr snapshot = SceneRows::snapshot(scene);
    ASSERT_LT(snapshot->row_count, vn::graphics::detail::SceneSnapshot::kRowsPerChunk) << "the fixture is ONE block";

    std::size_t outside_index = snapshot->row_count;
    std::size_t inside_index  = snapshot->row_count;
    for (std::size_t index = 0; index < snapshot->row_count; ++index) {
        const Row& row = snapshot->row(index);
        if (row.identity == outside_geometry->id()) {
            outside_index = index;
        }
        else if (row.identity == inside_geometry->id()) {
            inside_index = index;
        }
    }
    ASSERT_LT(outside_index, snapshot->row_count) << "the out-of-view segment is a row";
    ASSERT_LT(inside_index, snapshot->row_count) << "the in-view segment is a row";
    EXPECT_EQ(outside_index / vn::graphics::detail::SceneSnapshot::kRowsPerChunk,
              inside_index / vn::graphics::detail::SceneSnapshot::kRowsPerChunk)
        << "one chunk, two segments - the two rows this test is about share their block";

    const Row& outside_row = snapshot->row(outside_index);
    const Row& inside_row  = snapshot->row(inside_index);
    EXPECT_NE(outside_row.parent_world.element(0, 3), inside_row.parent_world.element(0, 3))
        << "the segments sit under different parent transforms (x = 1000 vs x = 0)";

    const CollectView looking_at_inside = viewFrom(Vec3d(0.0, 0.0, 10.0), Vec3d(0.0, 0.0, -3.0), Viewport{ 0, 0, 800, 600 });
    const Frustum     frustum           = Frustum::fromViewProjection(looking_at_inside.viewProjection());
    EXPECT_TRUE(frustum.isOutside(outside_row.world_box)) << "the block's FIRST row is the one out of view";
    EXPECT_FALSE(frustum.isOutside(inside_row.world_box)) << "the second row is in view - it must not be culled";

    bool                       viewport_used = false;
    std::vector<RenderCommand> visible;
    selectSnapshot(*snapshot, looking_at_inside, vn::graphics::kAllLayers, visible, viewport_used);
    ASSERT_EQ(visible.size(), 1U) << "one segment per parent and one of them in view: a whole-block skip answers none";
    EXPECT_EQ(visible.front().node, inside_geometry->id());

    const CollectView          looking_at_outside = viewFrom(Vec3d(1000.0, 0.0, 10.0), Vec3d(1000.0, 0.0, -3.0), Viewport{ 0, 0, 800, 600 });
    std::vector<RenderCommand> other;
    selectSnapshot(*snapshot, looking_at_outside, vn::graphics::kAllLayers, other, viewport_used);
    ASSERT_EQ(other.size(), 1U) << "the other camera answers the other segment - culling is still ON";
    EXPECT_EQ(other.front().node, outside_geometry->id());
}
