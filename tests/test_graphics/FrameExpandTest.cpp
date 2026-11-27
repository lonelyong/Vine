#include <gtest/gtest.h>

#include <vector>

#include <vine/intrusive_ptr.hpp>
#include <vine/math/Transform3.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraSnapshot.hpp>
#include <vine/graphics/View.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/MatrixTransform.hpp>
#include <vine/graphics/DrawItem.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>

#include "FrameExpand.hpp"
#include "PublicationStore.hpp"
#include "SnapshotSource.hpp"

using vn::graphics::Camera;
using vn::graphics::CameraSnapshot;
using vn::graphics::View;
using vn::graphics::DrawPacket;
using vn::graphics::FrameDescription;
using vn::graphics::GeometryPtr;
using vn::graphics::Group;
using vn::graphics::MatrixTransform;
using vn::graphics::PassDraw;
using vn::graphics::PassDesc;
using vn::graphics::DrawItem;
using vn::graphics::RenderPass;
using vn::graphics::Scene;
using vn::graphics::ScenePublishResult;
using vn::graphics::ScenePublishStatus;
using vn::graphics::SceneSnapshotRef;
using vn::graphics::detail::bindSceneDraws;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::expandPass;
using vn::graphics::detail::PublicationStore;
using vn::graphics::detail::SnapshotSource;
using vn::graphics::DrawKind;
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
    // The view the frame was DESCRIBED for: values only, exactly what a pass hands over (see DrawPacket). The
    // clip box is [-1, 1]^3 with the eye on +Z, and the content below is authored to sit inside it.
    CameraSnapshot camera;
    camera.present    = true;
    camera.eye        = Vec3d(0.0, 0.0, 10.0);
    camera.target     = Vec3d(0.0, 0.0, 0.0);
    camera.up         = Vec3d(0.0, 1.0, 0.0);
    camera.view       = vn::math::Mat4d{};
    camera.projection = vn::math::Mat4d{};

    DrawPacket draw;
    draw.kind       = DrawKind::Scene;
    draw.view.camera = camera;
    draw.content    = content;
    draw.view.layer_mask = vn::graphics::kAllLayers;

    PassDraw pass;
    pass.packets.push_back(draw);

    FrameDescription frame;
    frame.passes.emplace_back();
    frame.passes.back().draw = std::move(pass);
    return frame;
}

/// @brief The view the frames above state, so the content's own answer is asked with the same numbers.
View theView()
{
    View view;
    view.camera.eye = Vec3d(0.0, 0.0, 10.0);
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

/// @brief The source a frame built NOW binds its scene draws to (what RenderEngine::buildFrameValue does).
SnapshotSource* frameSourceFor(PublicationStore& store, Scene& scene)
{
    return store.sourceFor(scene, store.publicationFor(scene));
}

/// @brief Collects the commands of the frame's one draw.
const std::vector<DrawItem>& commandsOf(const FrameDescription& frame)
{
    static const std::vector<DrawItem> kNone;
    if (frame.passes.empty() || !frame.passes.front().draw.has_value() || frame.passes.front().draw->packets.empty()) {
        return kNone;
    }
    const auto& commands = frame.passes.front().draw->packets.front().commands;
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
    const auto from_content = scene.collectDrawItems(theView());

    // The engine side's answer: publish the content, bind the draw to its publication, then expand.
    PublicationStore store;
    EXPECT_TRUE(scene.publish()) << "the host's verb: the scene posts the picture a frame takes";
    FrameDescription from_snapshot = onePassFrame(frameSourceFor(store, scene));
    expandFrame(from_snapshot);

    const std::vector<DrawItem>& expected = *from_content;
    const std::vector<DrawItem>& actual   = commandsOf(from_snapshot);
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
    SnapshotSource* const source = frameSourceFor(store, scene);
    ASSERT_NE(source, nullptr) << "the bound publication is the source the frame binds its draws to";

    FrameDescription frame = onePassFrame(source);
    expandFrame(frame);
    EXPECT_EQ(commandsOf(frame).size(), 1U) << "the bound publication holds one drawable";

    // The content grows, and the frame is NOT told: a frame of the same declarations still draws the bound picture.
    root->addChild(triangleAt(0.6));
    FrameDescription stale = onePassFrame(source);
    expandFrame(stale);
    EXPECT_EQ(commandsOf(stale).size(), 1U) << "the content moved, but the engine was not handed a new publication";

    // THE FRAME'S OWN PUBLICATION DOES NOT OVERRIDE THE BINDING. A bound scene is the host's picture: a frame built
    // after the scene published something else still takes the bound value, or the bind would be undone silently.
    EXPECT_EQ(store.publicationFor(scene).revision(), first.snapshot.revision()) << "the frame still takes the bound picture";
    FrameDescription unbound_by_frame = onePassFrame(source);
    expandFrame(unbound_by_frame);
    EXPECT_EQ(commandsOf(unbound_by_frame).size(), 1U) << "the frame did not take the scene's new state";

    // Handing over the new publication is what changes the picture.
    const ScenePublishResult second = scene.publish();
    ASSERT_TRUE(second.posted());
    ASSERT_NE(second.snapshot.revision(), first.snapshot.revision());
    store.bindSnapshot(scene, second.snapshot);
    FrameDescription fresh = onePassFrame(frameSourceFor(store, scene));
    expandFrame(fresh);
    EXPECT_EQ(commandsOf(fresh).size(), 2U) << "the frame draws the publication it was last bound to";

    // An empty handle binds an EMPTY PICTURE - the same answer as a scene whose content draws nothing. It is not the
    // withdrawal: that is `unbindSnapshot`, and the last step below shows what it hands back.
    store.bindSnapshot(scene, SceneSnapshotRef{});
    FrameDescription cleared = onePassFrame(frameSourceFor(store, scene));
    expandFrame(cleared);
    EXPECT_TRUE(commandsOf(cleared).empty()) << "an empty publication draws nothing, and says so by drawing nothing";

    // THE WITHDRAWAL HANDS THE FRAME BACK TO THE SCENE (review 19 Z6): binding an empty picture and withdrawing the
    // override are two statements, and the frame built after the withdrawal draws what the scene last published.
    store.unbindSnapshot(scene);
    EXPECT_EQ(store.publicationFor(scene).revision(), second.snapshot.revision()) << "no override: the scene's latest";
    FrameDescription live = onePassFrame(frameSourceFor(store, scene));
    expandFrame(live);
    EXPECT_EQ(commandsOf(live).size(), 2U) << "the frame draws the content the scene published last";
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
    ASSERT_TRUE(scene.publish());
    FrameDescription frame = onePassFrame(frameSourceFor(store, scene));
    expandFrame(frame);
    const std::vector<DrawItem>* const first = &commandsOf(frame);
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
    ASSERT_TRUE(scene.publish());
    SnapshotSource* const source = frameSourceFor(store, scene);
    ASSERT_NE(source, nullptr);

    // Per pass: the pass twice, as a two-pass frame would ask it - the second pass must get the same list as the
    // first, from the same snapshot, without collecting anything again.
    FrameDescription piecewise = onePassFrame(nullptr);
    PassDraw&       pass       = *piecewise.passes.front().draw;
    bindSceneDraws(pass, source);
    expandPass(pass);
    ASSERT_EQ(pass.packets.size(), 1U);
    const std::size_t first_pass_commands = commandsOf(piecewise).size();
    EXPECT_EQ(first_pass_commands, 1U);
    pass.packets.front().commands.reset(); // a second pass describes itself from scratch
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
    // full-screen draw and a draw nothing was published for all stay exactly as they are.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));

    Scene scene;
    scene.setRoot(root);

    PublicationStore store;
    ASSERT_TRUE(scene.publish());
    SnapshotSource* const source = frameSourceFor(store, scene);

    PublicationStore other_store;
    SnapshotSource* const other_source = frameSourceFor(other_store, scene);
    ASSERT_NE(source, other_source) << "two stores publish the same content as two pictures";

    PassDraw pass;
    pass.packets.emplace_back(); // scene draw, nothing bound
    pass.packets.back().kind = DrawKind::Scene;
    pass.packets.emplace_back(); // full-screen draw
    pass.packets.back().kind = DrawKind::Fullscreen;
    pass.packets.emplace_back(); // scene draw already naming a source
    pass.packets.back().kind    = DrawKind::Scene;
    pass.packets.back().content = other_source;

    bindSceneDraws(pass, source);
    EXPECT_EQ(pass.packets[0].content, source) << "an unbound scene draw is pointed at the publication";
    EXPECT_EQ(pass.packets[1].content, nullptr) << "a full-screen draw names no content";
    EXPECT_EQ(pass.packets[2].content, other_source) << "a draw that already names a source keeps it";

    bindSceneDraws(pass, nullptr);
    EXPECT_EQ(pass.packets[0].content, source) << "nothing published changes nothing - the bind is not a clear";
}

TEST(FrameExpandTest, ADrawNoPublicationAnswersDrawsNothing)
{
    // The honest answer for a content nothing published: the draw names no source, the expansion asks nobody, and the
    // backend records nothing - never a quiet read of the live content instead.
    FrameDescription frame = onePassFrame(nullptr);
    expandFrame(frame);
    ASSERT_EQ(frame.passes.front().draw->packets.size(), 1U);
    EXPECT_EQ(frame.passes.front().draw->packets.front().commands, nullptr) << "no source, no collection";
    EXPECT_TRUE(commandsOf(frame).empty());
}

TEST(FrameExpandTest, AViewValueDrivesSelectAndDescribeTheSameWay)
{
    // ONE VIEW, TWO CONSUMERS. A pass describes itself into a View (camera value + device-pixel area + layer mask)
    // and the pure select takes that SAME value. If the two sides ever assembled their inputs differently, the
    // picture a frame draws and the picture a host collects would differ by a number nobody stated.
    auto root = intrusive_ptr<Group>(new Group());
    root->addChild(triangleAt(0.0));
    root->addChild(triangleAt(0.6));
    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(root);

    auto camera = intrusive_ptr<Camera>(new Camera());
    camera->setPerspective(60.0, 0.1, 100.0);
    camera->lookAt(Vec3d(0.0, 0.0, 10.0), Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 1.0, 0.0));

    // THE DECLARATION IS THE AUTHORING SHAPE (see E-04): the node below is what the engine builds from this value.
    PassDesc desc;
    desc.camera     = camera;
    desc.content    = scene;
    desc.viewport   = vn::graphics::Viewport{ 0, 0, 800, 600 };
    desc.layer_mask = vn::graphics::kAllLayers;
    RenderPass pass;
    vn::graphics::detail::PassNode::adopt(pass, std::move(desc));

    PassDraw described;
    ASSERT_TRUE(pass.describe(scene.get(), described));
    ASSERT_EQ(described.packets.size(), 1U);
    const View& view = described.packets.front().view;

    // The description took the camera AS A VALUE and the area the pass stated: the two halves a host could not
    // restate differently.
    ASSERT_TRUE(view.camera.present);
    EXPECT_EQ(view.viewport.width, 800);
    EXPECT_EQ(view.viewport.height, 600);
    EXPECT_EQ(view.layer_mask, vn::graphics::kAllLayers);

    // The publication answers the view (the frame boundary does this bind, see detail::bindSceneDraws).
    PublicationStore store;
    ASSERT_TRUE(scene->publish());
    SnapshotSource* const source = frameSourceFor(store, *scene);
    ASSERT_NE(source, nullptr);
    bindSceneDraws(described, source);

    expandPass(described);
    ASSERT_NE(described.packets.front().commands, nullptr) << "the expansion collected through the View";
    const std::vector<DrawItem>& described_commands = *described.packets.front().commands;

    // The pure select, fed the very same value, answers the same list.
    const vn::graphics::detail::SceneSnapshotPtr snapshot = source->snapshot();
    ASSERT_NE(snapshot, nullptr);

    const std::vector<DrawItem> selected = vn::graphics::detail::selectSnapshot(*snapshot, view).commands;

    ASSERT_EQ(selected.size(), described_commands.size()) << "one View, one picture";
    for (std::size_t index = 0; index < selected.size(); ++index) {
        EXPECT_EQ(selected[index].geometry.get(), described_commands[index].geometry.get()) << "command " << index;
        EXPECT_EQ(selected[index].primitive_index, described_commands[index].primitive_index) << "command " << index;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                EXPECT_DOUBLE_EQ(selected[index].model_matrix(row, column),
                                 described_commands[index].model_matrix(row, column))
                    << "command " << index << ": model matrix element (" << row << ", " << column << ")";
            }
        }
    }

    // THE MASK TRAVELS IN THE VIEW: a picture that selects no layer draws nothing, and the select says so.
    View filtered = view;
    filtered.layer_mask = 0U;
    EXPECT_TRUE(vn::graphics::detail::selectSnapshot(*snapshot, filtered).commands.empty())
        << "the mask is part of the view, not an argument beside it";
}
