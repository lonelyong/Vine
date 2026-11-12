#include <gtest/gtest.h>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Scene.hpp>

#include "PublicationStore.hpp"

using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::Scene;
using vn::graphics::ScenePublishResult;
using vn::graphics::ScenePublishStatus;
using vn::graphics::detail::PublicationStore;
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
    // The public Scene::publish() states its three answers through the same path a frame uses: the first call posts a
    // snapshot, a second with nothing changed reuses it, and a batch holds the previous snapshot until it closes.
    // THE ANSWER CARRIES THE PUBLICATION (see ScenePublishResult::snapshot), so each status also says WHICH picture is
    // the scene's answer - which is what a host hands to the engine it draws through.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    const ScenePublishResult posted = store.publish(scene);
    EXPECT_EQ(posted.status, ScenePublishStatus::Posted);
    ASSERT_TRUE(posted.snapshot.valid()) << "a posted answer names the publication it posted";

    const ScenePublishResult unchanged = store.publish(scene);
    EXPECT_EQ(unchanged.status, ScenePublishStatus::Unchanged);
    EXPECT_EQ(unchanged.snapshot.revision(), posted.snapshot.revision()) << "nothing moved, so the same picture is the answer";

    scene.beginEdit();
    root->children().front()->setOpacity(0.5F);
    const ScenePublishResult inside = store.publish(scene);
    EXPECT_EQ(inside.status, ScenePublishStatus::InsideBatch);
    EXPECT_EQ(inside.snapshot.revision(), posted.snapshot.revision())
        << "a batch holds the publication back: the answer is still the picture the batch started from";
    scene.endEdit();

    // The batch lands as ONE publication, and the answer names it.
    const ScenePublishResult closed = store.publish(scene);
    EXPECT_EQ(closed.status, ScenePublishStatus::Posted) << "the outermost endEdit releases the whole batch in one post";
    EXPECT_GT(closed.snapshot.revision(), posted.snapshot.revision());
    EXPECT_EQ(store.publicationCount(), 1U) << "one content, one source: publishing twice is the same content's table entry";
}
