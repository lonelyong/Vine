#include <gtest/gtest.h>

#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/CollectView.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/Scene.hpp>

#include "FrameExpand.hpp"
#include "FrameExpander.hpp"
#include "PublicationStore.hpp"

using vn::graphics::CameraSnapshot;
using vn::graphics::CollectView;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MatrixTransform;
using vn::graphics::PassDraw;
using vn::graphics::RenderCommand;
using vn::graphics::Scene;
using vn::graphics::ScenePublishResult;
using vn::graphics::ScenePublishStatus;
using vn::graphics::SceneSnapshotRef;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::FrameExpander;
using vn::graphics::detail::PublicationStore;
using vn::intrusive_ptr;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A frame value whose one pass draws one view of @p content - the shape RenderPass::describe produces.
FrameDescription onePassFrame(const vn::graphics::DrawListSource& content)
{
    // The view the frame was DESCRIBED for: values only, exactly what a pass hands over (see DrawDescription). The
    // clip box is [-1, 1]^3 with the eye on +Z, and the content below is authored to sit inside it.
    CameraSnapshot camera;
    camera.present    = true;
    camera.eye        = Vec3d(0.0, 0.0, 10.0);
    camera.target     = Vec3d(0.0, 0.0, 0.0);
    camera.up         = Vec3d(0.0, 1.0, 0.0);
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};

    DrawDescription draw;
    draw.kind       = DrawDescription::Kind::Scene;
    draw.camera     = camera;
    draw.content    = &content;
    draw.layer_mask = vn::graphics::kAllLayers;

    PassDraw pass;
    pass.draws.push_back(draw);

    FrameDescription frame;
    frame.passes.emplace_back();
    frame.passes.back().draw = std::move(pass);
    return frame;
}

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

/// @brief Collects the commands of the frame's one draw.
const std::vector<RenderCommand>& commandsOf(const FrameDescription& frame)
{
    static const std::vector<RenderCommand> kNone;
    if (frame.passes.empty() || !frame.passes.front().draw.has_value() || frame.passes.front().draw->draws.empty()) {
        return kNone;
    }
    const auto& commands = frame.passes.front().draw->draws.front().commands;
    return commands != nullptr ? *commands : kNone;
}

} // namespace

TEST(FrameExpanderTest, AFrameExpandsTheSamePictureTheSceneWould)
{
    // THE FRAME'S ENGINE HALF. A frame describes a VIEW and names whose content answers it; expanding it through the
    // expander asks that content's published snapshot instead - the exact seam the threaded mode needs, proven on the
    // calling thread first. Both paths must produce the same commands, or the snapshot is a second renderer.
    auto root = intrusive_ptr<Group>(new Group());
    auto arm  = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    arm->setMatrix(vn::math::translate(0.2, 0.1, 0.0));
    arm->addChild(triangleAt(0.0));
    root->addChild(arm);
    root->addChild(triangleAt(-0.6));

    Scene scene;
    scene.setRoot(root);

    // The content's answer, for the SAME view value.
    FrameDescription from_content = onePassFrame(scene);
    expandFrame(from_content);

    // The engine side's answer: publish the content, then bind and expand.
    PublicationStore store;
    FrameExpander    expander{ store };
    EXPECT_TRUE(store.publish(scene)) << "the first publish is the handshake";
    FrameDescription from_snapshot = onePassFrame(scene);
    expander.run(from_snapshot);

    const std::vector<RenderCommand>& expected = commandsOf(from_content);
    const std::vector<RenderCommand>& actual   = commandsOf(from_snapshot);
    ASSERT_EQ(expected.size(), 2U) << "both leaves are in the clip box";
    ASSERT_EQ(actual.size(), expected.size()) << "the two sides drew a different number of commands";
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(actual[index].geometry.get(), expected[index].geometry.get()) << "command " << index;
        EXPECT_EQ(actual[index].primitive_index, expected[index].primitive_index) << "command " << index;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(actual[index].model_matrix(row, column), expected[index].model_matrix(row, column))
                    << "command " << index << ": model matrix element (" << row << ", " << column << ")";
            }
        }
    }
    EXPECT_EQ(expander.unbound(), 0U) << "the draw named the content, and the bind found its snapshot";
}

TEST(FrameExpanderTest, ABoundPublicationIsThePictureTheFrameDraws)
{
    // THE EXPLICIT HANDOFF (see RenderEngine::bindSnapshot): what the engine draws is the publication it was BOUND to,
    // not whatever the content says when the frame runs. That is the whole point of publishing as a value - an editor
    // can publish, hand over, and keep editing while the frame draws the picture it named.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    FrameExpander    expander{ store };
    const ScenePublishResult first = scene.publish();
    ASSERT_TRUE(first.posted());
    store.bindSnapshot(scene, first.snapshot);

    FrameDescription frame = onePassFrame(scene);
    expander.run(frame);
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "the bound publication holds one drawable";

    // The content grows, and the frame is NOT told: a frame of the same declarations still draws the bound picture.
    root->addChild(triangleAt(0.6));
    FrameDescription stale = onePassFrame(scene);
    expander.run(stale);
    EXPECT_EQ(commandsOf(stale).size(), 1U) << "the content moved, but the engine was not handed a new publication";
    EXPECT_EQ(expander.unbound(), 0U) << "the draws found the source they were bound to";

    // THE FRAME'S OWN PUBLICATION DOES NOT OVERRIDE THE BINDING. A bound scene is the host's picture; the implicit
    // publication that runs once per frame must leave it alone, or a host's bind would be undone silently every frame.
    const ScenePublishResult implicit_answer = store.publish(scene);
    EXPECT_EQ(implicit_answer.status, ScenePublishStatus::Unchanged) << "a bound scene is not published by the frame";
    EXPECT_EQ(implicit_answer.snapshot.revision(), first.snapshot.revision()) << "and the answer is still the bound picture";
    FrameDescription unbound_by_frame = onePassFrame(scene);
    expander.run(unbound_by_frame);
    EXPECT_EQ(commandsOf(unbound_by_frame).size(), 1U) << "the frame did not take the scene's new state";

    // Handing over the new publication is what changes the picture.
    const ScenePublishResult second = scene.publish();
    ASSERT_TRUE(second.posted());
    ASSERT_NE(second.snapshot.revision(), first.snapshot.revision());
    store.bindSnapshot(scene, second.snapshot);
    FrameDescription fresh = onePassFrame(scene);
    expander.run(fresh);
    EXPECT_EQ(commandsOf(fresh).size(), 2U) << "the frame draws the publication it was last bound to";

    // An empty handle clears the binding: the same answer as a scene whose content draws nothing.
    store.bindSnapshot(scene, SceneSnapshotRef{});
    FrameDescription cleared = onePassFrame(scene);
    expander.run(cleared);
    EXPECT_TRUE(commandsOf(cleared).empty()) << "an empty publication draws nothing, and says so by drawing nothing";
}

TEST(FrameExpanderTest, ARunOnTheSameFrameTwiceBindsOnceAndExpandsOnce)
{
    // A frame value is expanded in place, so a second run over it must not collect again (the draw already carries its
    // list) and must not mistake the snapshot source it was bound to for an unknown source.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    FrameExpander    expander{ store };
    ASSERT_TRUE(store.publish(scene));
    FrameDescription frame = onePassFrame(scene);
    expander.run(frame);
    EXPECT_EQ(expander.unbound(), 0U);

    const std::vector<RenderCommand>* first = &commandsOf(frame);
    EXPECT_EQ(first->size(), 1U);

    expander.run(frame);
    EXPECT_EQ(expander.unbound(), 0U) << "the snapshot source the frame now names is this store's own";
    EXPECT_EQ(commandsOf(frame).size(), 1U);
}

TEST(FrameExpanderTest, PerPassAndWholeFrameFormsGiveTheSamePicture)
{
    // The engine measures its passes one by one, so the expander exposes the frame's steps as a bind+expand per pass.
    // The whole-frame form is their composition and must not be a second behaviour - a caller choosing one or the
    // other is choosing WHO measures, not WHAT is drawn.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    FrameExpander    expander{ store };
    ASSERT_TRUE(store.publish(scene));

    // Per pass: the pass twice, as a two-pass frame would ask it - the second pass must get the same list as the
    // first, from the same snapshot, without collecting anything again.
    FrameDescription piecewise = onePassFrame(scene);
    PassDraw&       pass       = *piecewise.passes.front().draw;
    expander.runPass(pass);
    ASSERT_EQ(pass.draws.size(), 1U);
    const std::size_t first_pass_commands = commandsOf(piecewise).size();
    EXPECT_EQ(first_pass_commands, 1U);
    pass.draws.front().commands.reset(); // a second pass describes itself from scratch
    pass.draws.front().content = &scene;
    expander.runPass(pass);
    EXPECT_EQ(commandsOf(piecewise).size(), first_pass_commands);

    // The whole-frame form, on a frame of its own.
    FrameDescription whole = onePassFrame(scene);
    expander.run(whole);
    ASSERT_EQ(commandsOf(whole).size(), first_pass_commands);
    for (std::size_t index = 0; index < first_pass_commands; ++index) {
        EXPECT_EQ(commandsOf(whole)[index].geometry.get(), commandsOf(piecewise)[index].geometry.get());
    }
}

TEST(FrameExpanderTest, ADrawWhoseContentWasNeverPublishedIsCountedNotSilentlyPublished)
{
    // The fallback is the honest one: a source the store does not publish is expanded AS IT IS (which is correct on
    // the calling thread - the inline mode - and a cross-thread read anywhere else). Counting it is how a host finds
    // out it forgot to publish, instead of drawing from content on the engine's thread and never knowing.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    FrameExpander    expander{ store };
    FrameDescription frame = onePassFrame(scene);
    expander.run(frame);
    EXPECT_EQ(expander.unbound(), 1U) << "the draw named a content this store does not publish";
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "it is still expanded, from the content itself";
    EXPECT_EQ(store.publicationCount(), 0U);
}
