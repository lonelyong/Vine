/**
 * @brief Definitions of the harness's shared pixel types and scene builders.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace vine::graphics;

namespace selftest
{

GeometryPtr makeTriangle(float x)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    positions.emplace_back(x, 0.0f, 0.0f);
    positions.emplace_back(x, 1.0f, 0.0f);
    positions.emplace_back(x, 0.0f, 1.0f);
    geom->setPositions(vine::graphics::packAttribute(positions));
    vine::geometry::Vec3fArray normals;
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    normals.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setNormals(vine::graphics::packAttribute(normals));
    return geom;
}

ShaderProgramPtr makeUserProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    // Clip z = 0.5: the backend is reverse-Z, so z = 0 would put this geometry
    // on the far plane where the cleared depth already is and the strict
    // "greater" depth test would reject every fragment (see the program
    // contract in SceneBridge and the variant probe below).
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"void main() { gl_Position = vec4(vine_Vertex.xy, 0.5, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.2, 0.2, 1.0); }\n";
    program->addStage(fs);
    return program;
}

GeometryPtr makeChannelTriangle()
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    positions.emplace_back(0.0f, 0.0f, 0.0f);
    positions.emplace_back(0.0f, 1.0f, 0.0f);
    positions.emplace_back(0.0f, 0.0f, 1.0f);
    geom->setPositions(vine::graphics::packAttribute(positions));
    // Custom per-vertex channel at location 3: one distinct colour per vertex
    // (red / green / blue). The backend must forward it as vine_Attribute3.
    vine::graphics::AttributeChannel channel = vine::graphics::AttributeChannel::packed(
        { 1.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 1.0f },
        3u);
    geom->addBuffer(3u, channel);
    return geom;
}

GeometryPtr makeVisibleQuad(float half, float z, float normal_z)
{
    auto geom = GeometryPtr(new Geometry());
    vine::geometry::Vec3fArray positions;
    const float corners[6][2] = { { -half, -half }, { half, -half }, { half, half },
                                 { -half, -half }, { half, half },  { -half, half } };
    for (const auto& corner : corners) {
        positions.emplace_back(corner[0], corner[1], z);
    }
    geom->setPositions(vine::graphics::packAttribute(positions));
    vine::geometry::Vec3fArray normals;
    for (int i = 0; i < 6; ++i) {
        normals.emplace_back(0.0f, 0.0f, normal_z);
    }
    geom->setNormals(vine::graphics::packAttribute(normals));
    return geom;
}

bool readTarget(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderTarget* target, PixelImage& image,
                int attachment)
{
    if (!renderer.readColorBuffer(target, attachment, image.pixels)) {
        return false;
    }
    image.width  = target->width();
    image.height = target->height();
    return image.pixels.size() == static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u;
}

void driveContentPass(vine::vsg::VsgRenderer& renderer, vine::graphics::RenderPass* pass,
                      vine::graphics::RenderTarget* target, const std::vector<RenderCommand>& commands,
                      const CameraPtr& camera, const vine::Color& clear_color,
                      vine::graphics::DepthMode depth_mode, int frames)
{
    for (int i = 0; i < frames; ++i) {
        FrameScope frame(renderer);
        PassScope  pass_scope(renderer, pass, 0, target, clear_color, /*clear_depth*/ true, depth_mode);
        renderer.render(commands, camera.get());
    }
}

ShaderProgramPtr makeAttributeProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"layout(location = 3) in vec3 vine_Attribute3;\n"
                u8"layout(location = 0) out vec3 vColor;\n"
                u8"void main() { gl_Position = vec4(vine_Vertex.xy, 0.5, 1.0); vColor = vine_Attribute3; }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vColor;\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(vColor, 1.0); }\n";
    program->addStage(fs);
    return program;
}

ShaderProgramPtr makeMidDepthProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"void main() { gl_Position = vec4(vine_Vertex.xy, 0.5, 1.0); }\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(1.0, 0.2, 0.2, 1.0); }\n";
    program->addStage(fs);
    return program;
}

ShaderProgramPtr makeDeferredProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"void main() { outColor = vec4(0.55, 0.6, 0.65, 1.0); }\n";
    program->addStage(fs);
    return program;
}

ShaderProgramPtr makeDepthSamplingProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    // The full-screen program ABI (see makeFullscreenProgramNode): binding i
    // samples the source's i-th colour attachment, binding N (= the colour count)
    // samples its depth. One colour attachment, so the depth is binding 1.
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"layout(binding = 1) uniform sampler2D sourceDepth;\n"
                u8"void main()\n"
                u8"{\n"
                u8"    vec2 uv = gl_FragCoord.xy / vec2(textureSize(sourceDepth, 0));\n"
                u8"    outColor = vec4(vec3(texture(sourceDepth, uv).r), 1.0);\n"
                u8"}\n";
    program->addStage(fs);
    return program;
}

CameraPtr makeCamera()
{
    auto cam = CameraPtr(new Camera());
    cam->setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0),
                               vine::math::Vec3d(0.0, 0.0, 0.0),
                               vine::math::Vec3d(0.0, 1.0, 0.0));
    cam->setProjectionMatrixAsPerspective(60.0, 1280.0 / 720.0, 0.1, 1000.0);
    return cam;
}

RenderTargetPtr makeMrtTarget()
{
    auto rt = RenderTargetPtr(new RenderTarget());
    rt->setSize(640, 360);
    rt->attachColor(RenderTarget::ColorFormat::RGBA8);
    rt->attachColor(RenderTarget::ColorFormat::RGBA16F);
    rt->attachColor(RenderTarget::ColorFormat::RGBA32F);
    rt->attachDepth(RenderTarget::DepthFormat::D24);
    return rt;
}

vine::intrusive_ptr<Texture> makeTwoToneTexture()
{
    constexpr int kSize = 8;
    auto          colours = vine::intrusive_ptr<vine::imaging::Image>(
        new vine::imaging::Image(kSize, kSize, vine::imaging::PixelFormat::Rgba8Unorm, 1));
    auto* pixels = reinterpret_cast<std::uint8_t*>(colours->mipData(0).data());
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            std::uint8_t* texel = pixels + (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4u;
            const bool    left  = x < kSize / 2;
            texel[0]            = left ? 255u : 0u;
            texel[1]            = 0u;
            texel[2]            = left ? 0u : 255u;
            texel[3]            = 255u;
        }
    }
    vine::intrusive_ptr<Texture> texture(new Texture2D(kSize, kSize, vine::imaging::PixelFormat::Rgba8Unorm, 1));
    texture->setSource(0, colours);
    return texture;
}

}  // namespace selftest
