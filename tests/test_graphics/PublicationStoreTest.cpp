#include <gtest/gtest.h>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/SceneSnapshotRef.hpp>

#include "PublicationStore.hpp"
#include "SnapshotSource.hpp"

using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::Scene;
using vn::graphics::ScenePublishResult;
using vn::graphics::ScenePublishStatus;
using vn::graphics::SceneSnapshotRef;
using vn::graphics::detail::PublicationStore;
using vn::graphics::detail::SnapshotSource;
using vn::intrusive_ptr;
using vn::math::Vec3f;

namespace
{

/// @brief A triangle-list leaf at @p x (one primitive, positions only).
GeometryPtr triangleAt(double x)
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(static_cast<float>(x), 0.0F, 0.0F),
                         Vec3f(static_cast<float>(x) + 0.5F, 0.0F, 0.0F),
                         Vec3f(static_cast<float>(x), 0.5F, 0.0F) });
    mesh->bumpRevision();
    return vn::graphics::geometryFromShape(*mesh);
}

} // namespace

TEST(PublicationStoreTest, PublishAnswersPostedUnchangedAndInsideBatch)
{
    // The public Scene::publish() states its three answers: the first call posts a snapshot, a second with nothing
    // changed reuses it, and a batch holds the previous snapshot until it closes. THE ANSWER CARRIES THE PUBLICATION
    // (see ScenePublishResult::snapshot), so each status also says WHICH picture is the scene's answer - and that value
    // is exactly what a frame built next takes (Scene::publishedSnapshot).
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    const ScenePublishResult posted = scene.publish();
    EXPECT_EQ(posted.status, ScenePublishStatus::Posted);
    ASSERT_TRUE(posted.snapshot.valid()) << "a posted answer names the publication it posted";
    EXPECT_EQ(scene.publishedSnapshot().revision(), posted.snapshot.revision())
        << "the scene answers with the same picture a frame would take";

    const ScenePublishResult unchanged = scene.publish();
    EXPECT_EQ(unchanged.status, ScenePublishStatus::Unchanged);
    EXPECT_EQ(unchanged.snapshot.revision(), posted.snapshot.revision()) << "nothing moved, so the same picture is the answer";

    scene.beginEdit();
    root->children().front()->setOpacity(0.5F);
    const ScenePublishResult inside = scene.publish();
    EXPECT_EQ(inside.status, ScenePublishStatus::InsideBatch);
    EXPECT_EQ(inside.snapshot.revision(), posted.snapshot.revision())
        << "a batch holds the publication back: the answer is still the picture the batch started from";
    EXPECT_EQ(scene.publishedSnapshot().revision(), posted.snapshot.revision()) << "and a frame built now takes that one too";
    scene.endEdit();

    // The batch lands as ONE publication, and the answer names it.
    const ScenePublishResult closed = scene.publish();
    EXPECT_EQ(closed.status, ScenePublishStatus::Posted) << "the outermost endEdit releases the whole batch in one post";
    EXPECT_GT(closed.snapshot.revision(), posted.snapshot.revision());
}

TEST(PublicationStoreTest, OneSourcePerPublicationAndTheSameValueIsTheSameSource)
{
    // A source is the DrawListSource face of ONE immutable value: the same publication answers through the same
    // source (so a static scene's per-view memo survives), and a new publication gets its own (so a frame that already
    // took the old one cannot be changed by the new one).
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    const SceneSnapshotRef first        = scene.publish().snapshot;
    SnapshotSource* const  first_source = store.sourceFor(scene, first);
    ASSERT_NE(first_source, nullptr);
    EXPECT_EQ(store.sourceFor(scene, first), first_source) << "the same publication is one source";
    EXPECT_EQ(store.publicationCount(), 1U);

    (void)scene.publish(); // nothing moved: the same value
    EXPECT_EQ(store.sourceFor(scene, scene.publishedSnapshot()), first_source);
    EXPECT_EQ(store.publicationCount(), 1U) << "an unchanged publish does not add a source";

    root->children().front()->setOpacity(0.5F);
    const SceneSnapshotRef second = scene.publish().snapshot;
    ASSERT_GT(second.revision(), first.revision());
    SnapshotSource* const second_source = store.sourceFor(scene, second);
    ASSERT_NE(second_source, nullptr);
    EXPECT_NE(second_source, first_source) << "a new publication answers through a source of its own";
    EXPECT_EQ(store.publicationCount(), 2U);
}

TEST(PublicationStoreTest, APublicationNoFrameNamesAnyMoreIsDropped)
{
    // The store is bounded by the frames in flight, not by the publications ever posted: a source nothing named for a
    // couple of frames is a picture nothing draws, and it goes with its memo.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    const SceneSnapshotRef first = scene.publish().snapshot;
    ASSERT_NE(store.sourceFor(scene, first), nullptr); // the frame being built right now

    // The next frames do not name it: the first beginFrame may keep it, the second drops it.
    store.beginFrame(2);
    store.beginFrame(3);
    EXPECT_EQ(store.publicationCount(), 0U) << "a publication no frame named is not retained for ever";

    // The value itself is still valid, and drawing it again just attaches a fresh source.
    EXPECT_NE(store.sourceFor(scene, first), nullptr);
    EXPECT_EQ(store.publicationCount(), 1U);
}

TEST(PublicationStoreTest, ABindingOverridesTheScenesLatestPublication)
{
    // THE OVERRIDE (see RenderEngine::bindSnapshot): without a bind a frame takes the scene's latest publication, and a
    // bind makes it take the named one instead - an editor view and an offscreen bake can be one publication apart.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    const SceneSnapshotRef first = scene.publish().snapshot;
    root->children().front()->setOpacity(0.5F);
    const SceneSnapshotRef second = scene.publish().snapshot;
    ASSERT_GT(second.revision(), first.revision());

    EXPECT_EQ(store.publicationFor(scene).revision(), second.revision()) << "no bind: the latest publication is the picture";

    store.bindSnapshot(scene, first);
    EXPECT_EQ(store.publicationFor(scene).revision(), first.revision()) << "a bind outranks later publications";
    EXPECT_NE(store.sourceFor(scene, store.publicationFor(scene)), nullptr);

    // An empty handle is a bound value: "draw nothing" (every view then answers an empty list).
    store.bindSnapshot(scene, SceneSnapshotRef{});
    EXPECT_FALSE(store.publicationFor(scene).valid());
    EXPECT_EQ(store.sourceFor(scene, store.publicationFor(scene)), nullptr) << "nothing published or bound names no source";

    // Re-binding the second publication puts the picture back (a bind replaces the previous one).
    store.bindSnapshot(scene, second);
    EXPECT_EQ(store.publicationFor(scene).revision(), second.revision());
}
