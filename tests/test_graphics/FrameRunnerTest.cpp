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
#include "FrameRunner.hpp"
#include "PublicationStore.hpp"
#include "SnapshotSource.hpp"

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
using vn::graphics::detail::FrameRunner;
using vn::graphics::detail::PublicationStore;
using vn::graphics::detail::RunnerMode;
using vn::graphics::detail::SnapshotSource;
using vn::intrusive_ptr;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A frame value whose one pass draws one view of @p content (the shape RenderPass::describe produces).
FrameDescription onePassFrame(const vn::graphics::DrawListSource& content){
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

/// @brief Two triangles, one of them under a transform: enough that a wrong fold shows up in a matrix.
intrusive_ptr<Group> twoLeafAssembly()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.5F, 0.0F, 0.0F), Vec3f(0.0F, 0.5F, 0.0F) });
    mesh->bumpRevision();

    auto root = intrusive_ptr<Group>(new Group());
    auto arm  = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    arm->setMatrix(vn::math::translate(0.2, 0.1, 0.0));
    arm->addChild(vn::graphics::geometryFromShape(*mesh));
    root->addChild(arm);
    auto other = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    other->setPositions({ Vec3f(-0.6F, 0.0F, 0.0F), Vec3f(-0.1F, 0.0F, 0.0F), Vec3f(-0.6F, 0.5F, 0.0F) });
    other->bumpRevision();
    root->addChild(vn::graphics::geometryFromShape(*other));
    return root;
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

/// @brief Asserts two command lists are the same picture, field for field.
void expectSamePicture(const std::vector<RenderCommand>& actual, const std::vector<RenderCommand>& expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        EXPECT_EQ(actual[index].geometry.get(), expected[index].geometry.get()) << "command " << index;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(actual[index].model_matrix(row, column), expected[index].model_matrix(row, column))
                    << "command " << index << ": element (" << row << ", " << column << ")";
            }
        }
    }
}

} // namespace

TEST(FrameRunnerTest, BothModesExpandAFrameToTheSamePicture)
{
    // THE TWO MODES ARE ONE CODE PATH. The inline runner is the caller's thread and the threaded one hands the frame
    // value over, waits for that frame, and gives it back expanded: the answer a frame gets must not depend on which.
    // The content's own answer is the third witness - it is what a frame named before any publication existed.
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    // The content's own answer, for the SAME view the frames state (a query on the thread that owns it).
    CollectView view;
    view.eye = Vec3d(0.0, 0.0, 10.0);
    const std::vector<RenderCommand> from_content = *scene.collectRenderCommands(view, vn::graphics::kAllLayers);

    // A JOB IS ONE FRAME'S ENGINE HALF (see RenderEngine::runEngineHalf): the expansion of a value that already names
    // the publications it draws, which is what a caller without a device has to hand over.
    PublicationStore inline_store;
    ASSERT_NE(inline_store.publish(scene).status, ScenePublishStatus::InsideBatch);
    SnapshotSource* const inline_source = inline_store.sourceOf(scene);
    ASSERT_NE(inline_source, nullptr);
    FrameRunner     inline_runner{ RunnerMode::Inline };
    FrameDescription from_inline = onePassFrame(*inline_source);
    inline_runner.run([&] { expandFrame(from_inline); });
    EXPECT_EQ(inline_runner.frames(), 1U) << "jobs are counted where they run: inline, that is the caller's thread";

    PublicationStore threaded_store;
    ASSERT_NE(threaded_store.publish(scene).status, ScenePublishStatus::InsideBatch);
    SnapshotSource* const threaded_source = threaded_store.sourceOf(scene);
    ASSERT_NE(threaded_source, nullptr);
    FrameRunner     threaded_runner{ RunnerMode::Threaded };
    FrameDescription from_thread = onePassFrame(*threaded_source);
    threaded_runner.run([&] { expandFrame(from_thread); });
    EXPECT_EQ(threaded_runner.frames(), 1U) << "the engine thread ran that frame";

    ASSERT_EQ(from_content.size(), 2U);
    expectSamePicture(commandsOf(from_inline), from_content);
    expectSamePicture(commandsOf(from_thread), from_content);
}

TEST(FrameRunnerTest, ManyFramesThroughTheEngineThreadStayEquivalent)
{
    // A thread that survives frames is the point: the request/completion handshake has to work over and over, with
    // nothing published in between (so every frame after the first expands from the same snapshot).
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(store.publish(scene));
    SnapshotSource* const source = store.sourceOf(scene);
    ASSERT_NE(source, nullptr);
    FrameRunner runner{ RunnerMode::Threaded };

    CollectView view;
    view.eye = Vec3d(0.0, 0.0, 10.0);

    constexpr int kFrames = 64;
    for (int frame_index = 0; frame_index < kFrames; ++frame_index) {
        FrameDescription frame = onePassFrame(*source);
        runner.run([&] { expandFrame(frame); });
        ASSERT_EQ(commandsOf(frame).size(), 2U) << "frame " << frame_index;
        const std::vector<RenderCommand> reference = *scene.collectRenderCommands(view, vn::graphics::kAllLayers);
        expectSamePicture(commandsOf(frame), reference);
    }
    EXPECT_EQ(runner.frames(), static_cast<std::uint64_t>(kFrames));
    EXPECT_EQ(store.publicationCount(), 1U);
}

TEST(FrameRunnerTest, StoppedRunnerRunsFramesOnTheCallerInsteadOfWaiting)
{
    // stop() is idempotent, and what is left is an INLINE runner - not a runner that waits for a thread that is gone.
    // That is the property a caller without a mode switch depends on: asking for a frame never deadlocks.
    auto root = twoLeafAssembly();
    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(store.publish(scene));
    SnapshotSource* const source = store.sourceOf(scene);
    ASSERT_NE(source, nullptr);
    FrameRunner runner{ RunnerMode::Threaded };
    runner.stop();
    runner.stop(); // idempotent
    EXPECT_EQ(runner.frames(), 0U);

    FrameDescription frame = onePassFrame(*source);
    // The job is the engine half as it always is: a stopped runner runs it on the caller instead of waiting for a
    // thread that is gone - it does not reinterpret how the draw should be answered.
    runner.run([&] { expandFrame(frame); });
    EXPECT_EQ(commandsOf(frame).size(), 2U);
    EXPECT_EQ(frame.passes.front().draw->draws.front().content, source)
        << "the draw names the content's publication, not the content - which is what makes the frame portable";
    EXPECT_EQ(runner.busy(), false) << "nothing is left in flight";
    EXPECT_EQ(runner.mode(), RunnerMode::Threaded) << "the mode is a lifetime decision, not a live state";
}
