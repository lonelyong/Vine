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
#include "FrameWorker.hpp"

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
using vn::graphics::ScenePublishStatus;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::FrameMode;
using vn::graphics::detail::FrameWorker;
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

TEST(FrameWorkerTest, AWorkerExpandsTheSameFrameTheSceneWould)
{
    // THE FRAME'S ENGINE HALF. A frame describes a VIEW and names whose content answers it; expanding it through the
    // worker asks that content's published snapshot instead - the exact seam the threaded mode needs, proven on the
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
    FrameWorker worker;
    EXPECT_TRUE(worker.publish(scene)) << "the first publish is the handshake";
    FrameDescription from_snapshot = onePassFrame(scene);
    worker.run(from_snapshot, FrameMode::Published);

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
    EXPECT_EQ(worker.unbound(), 0U) << "the draw named the content, and the bind found its snapshot";
}

TEST(FrameWorkerTest, PublishAnswersPostedUnchangedAndInsideBatch)
{
    // The public Scene::publish() states its three answers through the same path a frame uses: the first call posts a
    // snapshot, a second with nothing changed reuses it, and a batch holds the previous snapshot until it closes.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    FrameWorker worker;
    EXPECT_EQ(worker.publish(scene).status, ScenePublishStatus::Posted);
    EXPECT_EQ(worker.publish(scene).status, ScenePublishStatus::Unchanged);

    scene.beginEdit();
    root->children().front()->setOpacity(0.5F);
    EXPECT_EQ(worker.publish(scene).status, ScenePublishStatus::InsideBatch);
    scene.endEdit();
    EXPECT_EQ(worker.publish(scene).status, ScenePublishStatus::Posted);
}

TEST(FrameWorkerTest, ARunOnTheSameFrameTwiceBindsOnceAndExpandsOnce)
{
    // A frame value is expanded in place, so a second run over it must not collect again (the draw already carries its
    // list) and must not mistake the snapshot source it was bound to for an unknown source.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    FrameWorker worker;
    ASSERT_TRUE(worker.publish(scene));
    FrameDescription frame = onePassFrame(scene);
    worker.run(frame, FrameMode::Published);
    EXPECT_EQ(worker.unbound(), 0U);

    const std::vector<RenderCommand>* first = &commandsOf(frame);
    EXPECT_EQ(first->size(), 1U);

    worker.run(frame, FrameMode::Published);
    EXPECT_EQ(worker.unbound(), 0U) << "the snapshot source the frame now names is this worker's own";
    EXPECT_EQ(commandsOf(frame).size(), 1U);
}

TEST(FrameWorkerTest, PerPassAndWholeFrameFormsGiveTheSamePicture)
{
    // The engine measures its passes one by one, so the worker exposes the frame's steps as a bind+expand per pass.
    // The whole-frame form is their composition and must not be a second behaviour - a caller choosing one or the
    // other is choosing WHO measures, not WHAT is drawn.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    FrameWorker worker;
    ASSERT_TRUE(worker.publish(scene));

    // Per pass: the pass twice, as a two-pass frame would ask it - the second pass must get the same list as the
    // first, from the same snapshot, without collecting anything again.
    FrameDescription piecewise = onePassFrame(scene);
    PassDraw&       pass       = *piecewise.passes.front().draw;
    worker.runPass(pass, FrameMode::Published);
    ASSERT_EQ(pass.draws.size(), 1U);
    const std::size_t first_pass_commands = commandsOf(piecewise).size();
    EXPECT_EQ(first_pass_commands, 1U);
    pass.draws.front().commands.reset(); // a second pass describes itself from scratch
    pass.draws.front().content = &scene;
    worker.runPass(pass, FrameMode::Published);
    EXPECT_EQ(commandsOf(piecewise).size(), first_pass_commands);

    // The whole-frame form, on a frame of its own.
    FrameDescription whole = onePassFrame(scene);
    worker.run(whole, FrameMode::Published);
    ASSERT_EQ(commandsOf(whole).size(), first_pass_commands);
    for (std::size_t index = 0; index < first_pass_commands; ++index) {
        EXPECT_EQ(commandsOf(whole)[index].geometry.get(), commandsOf(piecewise)[index].geometry.get());
    }
}

TEST(FrameWorkerTest, TheInlineModeBindsNothingBecauseItIsTheContentsOwnThread)
{
    // THE MODE IS ONE PREDICATE. In the inline mode the caller IS the content's thread, so the draw keeps the content it
    // names: re-pointing it would spend a snapshot's copy to reproduce the answer the content gives for free. That also
    // makes `unbound()` a meaningful number instead of "every draw of an inline frame".
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    FrameWorker worker;
    ASSERT_TRUE(worker.publish(scene)) << "the publication exists, even though the inline mode will not use it";
    FrameDescription frame = onePassFrame(scene);
    worker.run(frame, FrameMode::Direct);
    EXPECT_EQ(worker.unbound(), 0U) << "an inline run binds nothing, so nothing is unbound";
    EXPECT_EQ(frame.passes.front().draw->draws.front().content, &scene) << "the draw still names the content";
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "and it expanded all the same";
}

TEST(FrameWorkerTest, ADrawWhoseContentWasNeverPublishedIsCountedNotSilentlyPublished)
{
    // The fallback is the honest one: a source this worker does not publish is expanded AS IT IS (which is correct on
    // the calling thread - the inline mode - and a cross-thread read anywhere else). Counting it is how a host finds
    // out it forgot to publish, instead of drawing from content on the engine's thread and never knowing.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    FrameWorker worker;
    FrameDescription frame = onePassFrame(scene);
    worker.run(frame, FrameMode::Published);
    EXPECT_EQ(worker.unbound(), 1U) << "the draw named a content this worker does not publish";
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "it is still expanded, from the content itself";
    EXPECT_EQ(worker.publicationCount(), 0U);
}
