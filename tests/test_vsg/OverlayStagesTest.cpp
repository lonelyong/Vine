/**
 * @brief The backend owns NO GLSL: every stage it compiles comes from the SDK.
 *
 * The vsg backend used to carry a shader directory of its own (a full-screen triangle and a screen
 * copy fragment stage, embedded into vine/vsg/EmbeddedShaders.hpp). Both were behind engine-visible
 * pictures — a program-less ScreenPass drew the copy, and every full-screen program was written
 * against that triangle — while the text lived in one backend, so a second backend could have
 * changed them. They are SDK programs now (BuiltinShaders::fullscreenVertexProgram /
 * screenCopyProgram) and there is no generated vine/vsg header to include.
 *
 * What a device-free test can still pin, and does here:
 *  * `detail::fullscreenVertexSource()` serves the SDK's program stage, so the two cannot drift and
 *    a full-screen picture cannot be sampled through a triangle the engine did not state;
 *  * that stage is the interface every fragment stage is written against (vine_uv in, its range and
 *    orientation, no vertex buffer / push constant);
 *  * the copy program declares the ABI the node builder wires, and its BINDING is the attachment.
 */

#include <gtest/gtest.h>

#include <vine/graphics/BuiltinShaders.hpp>
#include <vine/vsg/VsgPipelineFactory.hpp>
#include <vine/vsg/VsgUtils.hpp>

#include <string>
#include <string_view>

using namespace vine::vsg;

namespace
{

/** @brief The GLSL source of @p program's first stage (a COPY: stdstr() returns by value). */
std::string stageSource(const vine::intrusive_ptr<vine::graphics::ShaderProgram>& program)
{
    if (program == nullptr || program->stageCount() == 0u) {
        return {};
    }
    const auto* stage = program->stage(0);
    return (stage != nullptr) ? stage->source.stdstr() : std::string{};
}

}  // namespace

TEST(OverlayStagesTest, TheBackendServesTheSdksFullscreenTriangle)
{
    const auto program = vine::graphics::fullscreenVertexProgram();
    ASSERT_NE(program, nullptr);
    const std::string sdk_source = stageSource(program);
    ASSERT_FALSE(sdk_source.empty());

    const std::string& served = detail::fullscreenVertexSource();
    EXPECT_EQ(std::string_view(served), std::string_view(sdk_source));
}

TEST(OverlayStagesTest, TheTriangleDeclaresTheInterfaceFragmentStagesWriteAgainst)
{
    const std::string source = stageSource(vine::graphics::fullscreenVertexProgram());
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("layout(location = 0) out vec2 vine_uv;"), std::string_view::npos);
    // Generated from gl_VertexIndex: no vertex buffer, no push constant, no camera matrices.
    EXPECT_NE(source.find("gl_VertexIndex"), std::string_view::npos);
    EXPECT_EQ(source.find("layout(location = 0) in "), std::string_view::npos);
}

TEST(OverlayStagesTest, TheCopyProgramDeclaresTheOverlayAbi)
{
    const auto program = vine::graphics::screenCopyProgram();
    ASSERT_NE(program, nullptr);
    const std::string text = stageSource(program);
    ASSERT_FALSE(text.empty());
    // The node builder binds the pass input as "screen_tex" at binding 0 and the
    // fullscreen vertex stage writes vine_uv as location 0.
    EXPECT_NE(text.find("layout(binding = 0) uniform sampler2D screen_tex;"), std::string_view::npos);
    EXPECT_NE(text.find("layout(location = 0) in vec2 vine_uv;"), std::string_view::npos);
    EXPECT_NE(text.find("layout(location = 0) out vec4 out_color;"), std::string_view::npos);
    EXPECT_NE(text.find("texture(screen_tex, vine_uv)"), std::string_view::npos);
    // A copy is NOT a lights pass: it declares no push block, so it shades the same with or without
    // a camera (the pass still needs one — the backend builds its view from it).
    EXPECT_EQ(text.find("push_constant"), std::string_view::npos);
}

TEST(OverlayStagesTest, TheCopyProgramsBindingIsTheAttachment)
{
    // Binding i reads the source's colour attachment i (the full-screen program ABI), so asking for
    // another attachment is a different program text — not a backend setting.
    const const std::string zero = stageSource(vine::graphics::screenCopyProgram(0));
    const const std::string two  = stageSource(vine::graphics::screenCopyProgram(2));
    ASSERT_FALSE(zero.empty());
    ASSERT_FALSE(two.empty());
    EXPECT_NE(zero.find("layout(binding = 0)"), std::string_view::npos);
    EXPECT_NE(two.find("layout(binding = 2)"), std::string_view::npos);
    EXPECT_EQ(two.find("layout(binding = 0)"), std::string_view::npos);
    EXPECT_NE(vine::graphics::screenCopyProgram(2)->name(), vine::graphics::screenCopyProgram(0)->name());
}
