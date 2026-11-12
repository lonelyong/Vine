/**
 * @brief T35: the frame's VALUES are what crosses the thread, not the content objects.
 *
 * The thread contract (`.ai/design/graphics-threading.md` 2.1 / 2.2) says the engine half reads only the immutable
 * publication and the values a pass took when it described itself. This case puts a frame's engine half on another
 * thread, blocks it BEFORE it reads anything, edits a program's text, a material's values and a light's colour on the
 * content thread, and then lets it run:
 *
 *   * frame N must keep the values it was described with (program revision, material colour, light colour), and
 *   * frame N+1 - published and described after the edit - must see the new ones.
 *
 * The gates are `program_description` / `material_description` / `material_ref` on the collected command, and
 * `DrawDescription::light_values`. A recorder that re-read a live object would pass the "N+1 sees the new values" half
 * and fail the "N stayed the same" half; under ThreadSanitizer it would also report the race this case makes real.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <future>
#include <thread>
#include <vector>

#include <vine/Colorf.hpp>
#include <vine/intrusive_ptr.hpp>
#include <vine/math/Vector3.hpp>
#include <vine/models/TriangleMesh.hpp>

#include <vine/graphics/Camera.hpp>
#include <vine/graphics/FrameDescription.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/graphics/StateNode.hpp>

#include "FrameExpand.hpp"
#include "PublicationStore.hpp"

using vn::Colorf;
using vn::intrusive_ptr;
using vn::graphics::Camera;
using vn::graphics::CameraSnapshot;
using vn::graphics::DrawDescription;
using vn::graphics::FrameDescription;
using vn::graphics::geometryFromShape;
using vn::graphics::Geometry;
using vn::graphics::Light;
using vn::graphics::Material;
using vn::graphics::PassDraw;
using vn::graphics::RenderCommand;
using vn::graphics::RenderPass;
using vn::graphics::Scene;
using vn::graphics::ScenePublishStatus;
using vn::graphics::ShaderProgram;
using vn::graphics::ShaderStage;
using vn::graphics::ShaderStageType;
using vn::graphics::StateNode;
using vn::graphics::detail::bindSceneDraws;
using vn::graphics::detail::expandFrame;
using vn::graphics::detail::PublicationStore;
using vn::math::Vec3d;
using vn::math::Vec3f;

namespace
{

/// @brief One frame whose single pass draws one view of @p scene, as `RenderPass::describe` produces it.
///
/// The pass states the view and the store says whose publication answers it - exactly the two steps a frame's
/// content half performs (see detail::bindSceneDraws).
FrameDescription describedFrame(RenderPass& pass, const intrusive_ptr<Scene>& scene, PublicationStore& store)
{
    PassDraw description;
    EXPECT_TRUE(pass.describe(scene.get(), description));
    bindSceneDraws(description, store.sourceOf(*scene));

    FrameDescription frame;
    frame.passes.emplace_back();
    frame.passes.back().draw = std::move(description);
    return frame;
}

/// @brief The commands the frame's one drawing call collected (empty when it collected nothing).
const std::vector<RenderCommand>& commandsOf(const FrameDescription& frame)
{
    static const std::vector<RenderCommand> kNone;
    if (frame.passes.empty() || !frame.passes.front().draw.has_value() || frame.passes.front().draw->draws.empty()) {
        return kNone;
    }
    const auto& commands = frame.passes.front().draw->draws.front().commands;
    return commands != nullptr ? *commands : kNone;
}

/// @brief A two-stage program whose text says which revision it is (the stage source is the witness).
intrusive_ptr<ShaderProgram> programWhoseTextSays(const char8_t* text)
{
    auto program = intrusive_ptr<ShaderProgram>(new ShaderProgram());
    program->replaceStages({ ShaderStage{ ShaderStageType::Vertex, vn::String(text), vn::String(u8"main") },
                             ShaderStage{ ShaderStageType::Fragment, vn::String(text), vn::String(u8"main") } });
    return program;
}

/// @brief One triangle, so the frame has a real draw to collect.
intrusive_ptr<Geometry> oneTriangle()
{
    auto mesh = intrusive_ptr<vn::models::TriangleMesh>(new vn::models::TriangleMesh());
    mesh->setPositions({ Vec3f(0.0F, 0.0F, 0.0F), Vec3f(0.5F, 0.0F, 0.0F), Vec3f(0.0F, 0.5F, 0.0F) });
    mesh->bumpRevision();
    return geometryFromShape(*mesh);
}

} // namespace

TEST(ValueChannelTest, AFrameInFlightKeepsItsValuesWhileTheContentIsEdited)
{
    // THE CONTENT (content thread): a leaf under a state node carrying a material and a program, one sun.
    auto program  = programWhoseTextSays(u8"program text v1");
    auto material = intrusive_ptr<Material>(new Material());
    material->setBaseColor(Colorf{ 0.9F, 0.1F, 0.1F, 1.0F });

    auto state_node = intrusive_ptr<StateNode>(new StateNode());
    state_node->setProgram(program);
    state_node->setMaterial(material);
    state_node->addChild(oneTriangle());

    auto scene = intrusive_ptr<Scene>(new Scene());
    scene->setRoot(state_node);
    auto light = Light::createDirectional(Vec3d(0.0, 0.0, -1.0));
    light->setColor(Colorf{ 0.5F, 0.5F, 1.0F, 1.0F });
    scene->addLight(light);

    auto camera = intrusive_ptr<Camera>(new Camera());
    RenderPass pass;
    pass.setContent(scene);
    pass.setCamera(camera);
    pass.setLayerMask(vn::graphics::kAllLayers);

    // FRAME N: published and described on the content side, exactly as a frame's content half does it.
    PublicationStore store;
    ASSERT_EQ(store.publish(*scene).status, ScenePublishStatus::Posted);
    FrameDescription frame = describedFrame(pass, scene, store);

    const std::uint64_t program_revision_v1  = program->revision();
    const std::uint64_t material_revision_v1 = material->revision();

    // THE ENGINE HALF, HELD BEFORE IT READS ANYTHING: it signals ready, waits for the content edits, and only then
    // binds, expands and captures what the frame carries.
    std::promise<void>              ready;
    std::promise<void>              go;
    vn::graphics::ShaderProgramDescriptionPtr captured_program;
    vn::graphics::MaterialDescriptionPtr      captured_material;
    vn::graphics::ContentRef                  captured_material_ref;
    std::optional<std::vector<vn::graphics::LightValue>> captured_lights;

    std::thread engine_thread([&] {
        ready.set_value();
        go.get_future().wait();

        expandFrame(frame); // expand on the engine thread (the real engine half's reads)

        const std::vector<RenderCommand>& commands = commandsOf(frame);
        ASSERT_EQ(commands.size(), 1U);
        captured_program      = commands.front().program_description;
        captured_material     = commands.front().material_description;
        captured_material_ref = commands.front().material_ref;
        if (!frame.passes.empty() && frame.passes.front().draw.has_value() && !frame.passes.front().draw->draws.empty()) {
            captured_lights = frame.passes.front().draw->draws.front().light_values;
        }
    });

    ready.get_future().wait();

    // THE EDITS, WHILE FRAME N'S ENGINE HALF IS STILL IN FLIGHT. None of them moves a node epoch: a program's text and
    // a material's values are edited through live objects, and a light is not published at all.
    program->replaceStages({ ShaderStage{ ShaderStageType::Vertex, vn::String(u8"program text v2"), vn::String(u8"main") },
                             ShaderStage{ ShaderStageType::Fragment, vn::String(u8"program text v2"), vn::String(u8"main") } });
    material->setBaseColor(Colorf{ 0.1F, 0.9F, 0.1F, 1.0F });
    light->setColor(Colorf{ 1.0F, 0.7F, 0.2F, 1.0F });

    go.set_value();
    engine_thread.join();

    // FRAME N IS UNCHANGED: the values it was described with are the ones its engine half read.
    ASSERT_NE(captured_program, nullptr);
    EXPECT_EQ(captured_program->revision(), program_revision_v1) << "frame N keeps the program text it scripted";
    EXPECT_EQ(captured_program->stages().front().source.as_std_str(), "program text v1");
    ASSERT_NE(captured_material, nullptr);
    EXPECT_EQ(captured_material->values().base_color, (Colorf{ 0.9F, 0.1F, 0.1F, 1.0F })) << "frame N keeps its material values";
    EXPECT_EQ(captured_material_ref.revision, material_revision_v1);
    ASSERT_TRUE(captured_lights.has_value());
    ASSERT_EQ(captured_lights->size(), 1U);
    EXPECT_EQ(captured_lights->front().color, (Colorf{ 0.5F, 0.5F, 1.0F, 1.0F })) << "frame N keeps the light it was lit with";

    // THE CONTENT DID MOVE: the next publication/description is what carries the new values.
    EXPECT_NE(program->revision(), program_revision_v1);
    EXPECT_NE(material->revision(), material_revision_v1);

    // FRAME N+1: publish, describe, run - all after the edits.
    ASSERT_EQ(store.publish(*scene).status, ScenePublishStatus::Posted);
    FrameDescription next = describedFrame(pass, scene, store);
    expandFrame(next);

    const std::vector<RenderCommand>& next_commands = commandsOf(next);
    ASSERT_EQ(next_commands.size(), 1U);
    ASSERT_NE(next_commands.front().program_description, nullptr);
    EXPECT_EQ(next_commands.front().program_description->revision(), program->revision());
    EXPECT_EQ(next_commands.front().program_description->stages().front().source.as_std_str(), "program text v2");
    ASSERT_NE(next_commands.front().material_description, nullptr);
    EXPECT_EQ(next_commands.front().material_description->values().base_color, (Colorf{ 0.1F, 0.9F, 0.1F, 1.0F }));
    ASSERT_TRUE(next.passes.front().draw.has_value());
    ASSERT_TRUE(next.passes.front().draw->draws.front().light_values.has_value());
    ASSERT_EQ(next.passes.front().draw->draws.front().light_values->size(), 1U);
    EXPECT_EQ(next.passes.front().draw->draws.front().light_values->front().color, (Colorf{ 1.0F, 0.7F, 0.2F, 1.0F }));
}
