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
using vn::graphics::ScenePublishResult;
using vn::graphics::ScenePublishStatus;
using vn::graphics::SceneSnapshotRef;
using vn::graphics::detail::bindSceneDraws;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::expandPass;
using vn::graphics::detail::PublicationStore;
using vn::graphics::detail::SnapshotSource;
using vn::intrusive_ptr;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief A frame value whose one pass draws one view of @p content - the shape RenderPass::describe produces.
///
/// @param content The publication's source the draw names (null: nothing was published for it), exactly what the
///                frame's bind step wrote.
FrameDescription onePassFrame(const vn::graphics::DrawListSource* content)
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
    draw.content    = content;
    draw.layer_mask = vn::graphics::kAllLayers;

    PassDraw pass;
    pass.draws.push_back(draw);

    FrameDescription frame;
    frame.passes.emplace_back();
    frame.passes.back().draw = std::move(pass);
    return frame;
}

/// @brief The view the frames above state, so the content's own answer is asked with the same numbers.
CollectView theView()
{
    CollectView view;
    view.eye = Vec3d(0.0, 0.0, 10.0);
    return view;
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

TEST(FrameExpandTest, AFrameExpandsTheSamePictureTheSceneWould)
{
    // THE FRAME'S ENGINE HALF. A frame describes a VIEW and names the publication that answers it; expanding it asks
    // that publication - the exact seam the threaded mode needs, proven on the calling thread first. Both paths must
    // produce the same commands, or the snapshot is a second renderer.
    auto root = intrusive_ptr<Group>(new Group());
    auto arm  = intrusive_ptr<MatrixTransform>(new MatrixTransform());
    arm->setMatrix(vn::math::translate(0.2, 0.1, 0.0));
    arm->addChild(triangleAt(0.0));
    root->addChild(arm);
    root->addChild(triangleAt(-0.6));

    Scene scene;
    scene.setRoot(root);

    // The content's own answer, for the SAME view value (a query on the thread that owns it - see Scene).
    const auto from_content = scene.collectRenderCommands(theView(), vn::graphics::kAllLayers);

    // The engine side's answer: publish the content, bind the draw to its publication, then expand.
    PublicationStore store;
    EXPECT_TRUE(store.publish(scene)) << "the first publish is the handshake";
    FrameDescription from_snapshot = onePassFrame(store.sourceOf(scene));
    expandFrame(from_snapshot);

    const std::vector<RenderCommand>& expected = *from_content;
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
}

TEST(FrameExpandTest, ABoundPublicationIsThePictureTheFrameDraws)
{
    // THE EXPLICIT HANDOFF (see RenderEngine::bindSnapshot): what the engine draws is the publication it was BOUND to,
    // not whatever the content says when the frame runs. That is the whole point of publishing as a value - an editor
    // can publish, hand over, and keep editing while the frame draws the picture it named.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    const ScenePublishResult first = scene.publish();
    ASSERT_TRUE(first.posted());
    store.bindSnapshot(scene, first.snapshot);
    SnapshotSource* const source = store.sourceOf(scene);
    ASSERT_NE(source, nullptr) << "binding a snapshot attaches the source the frame binds its draws to";

    FrameDescription frame = onePassFrame(source);
    expandFrame(frame);
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "the bound publication holds one drawable";

    // The content grows, and the frame is NOT told: a frame of the same declarations still draws the bound picture.
    root->addChild(triangleAt(0.6));
    FrameDescription stale = onePassFrame(source);
    expandFrame(stale);
    EXPECT_EQ(commandsOf(stale).size(), 1U) << "the content moved, but the engine was not handed a new publication";

    // THE FRAME'S OWN PUBLICATION DOES NOT OVERRIDE THE BINDING. A bound scene is the host's picture; the implicit
    // publication that runs once per frame must leave it alone, or a host's bind would be undone silently every frame.
    const ScenePublishResult implicit_answer = store.publish(scene);
    EXPECT_EQ(implicit_answer.status, ScenePublishStatus::Unchanged) << "a bound scene is not published by the frame";
    EXPECT_EQ(implicit_answer.snapshot.revision(), first.snapshot.revision()) << "and the answer is still the bound picture";
    FrameDescription unbound_by_frame = onePassFrame(source);
    expandFrame(unbound_by_frame);
    EXPECT_EQ(commandsOf(unbound_by_frame).size(), 1U) << "the frame did not take the scene's new state";

    // Handing over the new publication is what changes the picture.
    const ScenePublishResult second = scene.publish();
    ASSERT_TRUE(second.posted());
    ASSERT_NE(second.snapshot.revision(), first.snapshot.revision());
    store.bindSnapshot(scene, second.snapshot);
    FrameDescription fresh = onePassFrame(source);
    expandFrame(fresh);
    EXPECT_EQ(commandsOf(fresh).size(), 2U) << "the frame draws the publication it was last bound to";

    // An empty handle clears the binding: the same answer as a scene whose content draws nothing.
    store.bindSnapshot(scene, SceneSnapshotRef{});
    FrameDescription cleared = onePassFrame(source);
    expandFrame(cleared);
    EXPECT_TRUE(commandsOf(cleared).empty()) << "an empty publication draws nothing, and says so by drawing nothing";
}

TEST(FrameExpandTest, ASecondExpansionKeepsTheListTheDrawAlreadyCarries)
{
    // A frame value is expanded in place, so a second expansion must not collect again: the draw already carries its
    // list, and "a list I already have" is not asked for twice.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(store.publish(scene));
    FrameDescription frame = onePassFrame(store.sourceOf(scene));
    expandFrame(frame);
    const std::vector<RenderCommand>* const first = &commandsOf(frame);
    EXPECT_EQ(first->size(), 1U);

    expandFrame(frame);
    EXPECT_EQ(&commandsOf(frame), first) << "the shared list is the one the draw already held";
}

TEST(FrameExpandTest, PerPassAndWholeFrameFormsGiveTheSamePicture)
{
    // The engine measures its passes one by one, so the expansion exposes both a per-pass form and a whole-frame form.
    // The whole-frame form is their composition and must not be a second behaviour - a caller choosing one or the
    // other is choosing WHO measures, not WHAT is drawn.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(store.publish(scene));
    SnapshotSource* const source = store.sourceOf(scene);
    ASSERT_NE(source, nullptr);

    // Per pass: the pass twice, as a two-pass frame would ask it - the second pass must get the same list as the
    // first, from the same snapshot, without collecting anything again.
    FrameDescription piecewise = onePassFrame(nullptr);
    PassDraw&       pass       = *piecewise.passes.front().draw;
    bindSceneDraws(pass, source);
    expandPass(pass);
    ASSERT_EQ(pass.draws.size(), 1U);
    const std::size_t first_pass_commands = commandsOf(piecewise).size();
    EXPECT_EQ(first_pass_commands, 1U);
    pass.draws.front().commands.reset(); // a second pass describes itself from scratch
    bindSceneDraws(pass, source);
    expandPass(pass);
    EXPECT_EQ(commandsOf(piecewise).size(), first_pass_commands);

    // The whole-frame form, on a frame of its own.
    FrameDescription whole = onePassFrame(source);
    expandFrame(whole);
    ASSERT_EQ(commandsOf(whole).size(), first_pass_commands);
    for (std::size_t index = 0; index < first_pass_commands; ++index) {
        EXPECT_EQ(commandsOf(whole)[index].geometry.get(), commandsOf(piecewise)[index].geometry.get());
    }
}

TEST(FrameExpandTest, BindSceneDrawsBindsOnlyWhatNothingBoundYet)
{
    // THE BIND STEP is "whose publication answers this view" and nothing else: a draw that already names a source, a
    // screen draw and a draw nothing was published for all stay exactly as they are.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(store.publish(scene));
    SnapshotSource* const source = store.sourceOf(scene);

    PublicationStore other_store;
    ASSERT_NE(other_store.publish(scene).status, ScenePublishStatus::InsideBatch);
    SnapshotSource* const other_source = other_store.sourceOf(scene);
    ASSERT_NE(source, other_source) << "two stores publish the same content as two pictures";

    PassDraw pass;
    pass.draws.emplace_back(); // scene draw, nothing bound
    pass.draws.back().kind = DrawDescription::Kind::Scene;
    pass.draws.emplace_back(); // screen draw
    pass.draws.back().kind = DrawDescription::Kind::Screen;
    pass.draws.emplace_back(); // scene draw already naming a source
    pass.draws.back().kind    = DrawDescription::Kind::Scene;
    pass.draws.back().content = other_source;

    bindSceneDraws(pass, source);
    EXPECT_EQ(pass.draws[0].content, source) << "an unbound scene draw is pointed at the publication";
    EXPECT_EQ(pass.draws[1].content, nullptr) << "a screen draw names no content";
    EXPECT_EQ(pass.draws[2].content, other_source) << "a draw that already names a source keeps it";

    bindSceneDraws(pass, nullptr);
    EXPECT_EQ(pass.draws[0].content, source) << "nothing published changes nothing - the bind is not a clear";
}

TEST(FrameExpandTest, ADrawNoPublicationAnswersDrawsNothing)
{
    // The honest answer for a content nothing published: the draw names no source, the expansion asks nobody, and the
    // backend records nothing - never a quiet read of the live content instead.
    FrameDescription frame = onePassFrame(nullptr);
    expandFrame(frame);
    ASSERT_EQ(frame.passes.front().draw->draws.size(), 1U);
    EXPECT_EQ(frame.passes.front().draw->draws.front().commands, nullptr) << "no source, no collection";
    EXPECT_TRUE(commandsOf(frame).empty());
}
