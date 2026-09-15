/**
 * @brief Texture, cube-map and built-in-shading phases: the upload/wiring paths no other gate covers.
 */

#include "selftest_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace vine::graphics;

namespace selftest
{

/**
 * @brief Builds a clip-space quad whose vertices carry texture coordinates.
 *
 * The positions double as clip coordinates (the sampling program passes x/y straight through), so the quad
 * is exactly the middle 80% of the target and the UVs can be asserted at known pixels rather than searched
 * for. The UVs follow the GL convention (v = 0 at the TOP), which is what lets the readback tell a correct u
 * axis apart from a mirrored one.
 *
 * @return The quad geometry (two triangles, one UV per vertex).
 */
GeometryPtr makeTexturedQuad()
{
    auto geom = makeVisibleQuad(0.4f, 0.0f);
    // One UV per vertex, in the order makeVisibleQuad() emits its corners: a channel whose element count
    // does not match the vertex count is dropped whole, which would silently leave the shader with no UVs.
    const float corners[6][2] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f },
                                  { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } };
    vine::geometry::Vec2fArray texcoords;
    for (const auto& corner : corners) {
        texcoords.emplace_back(corner[0], corner[1]);
    }
    geom->setTexcoords2(vine::graphics::packAttribute(texcoords));
    return geom;
}

/**
 * @brief Builds a program that outputs the material's texture, sampled by UV.
 *
 * The locations are the program ABI, not a free choice: the texcoord attribute is declared at location 8 and the
 * texture at set 0 binding 1 (see assembleProgramShaderSet). A fragment stage that samples a name the
 * ShaderSet does not declare is not an error at any layer — the assignment is silently dropped — so a
 * mismatch here would show up as an untextured quad rather than as a failure.
 *
 * @return The sampling program.
 */
ShaderProgramPtr makeTextureSampleProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"layout(location = 8) in vec2 vine_TexCoord0;\n"
                u8"layout(location = 0) out vec2 uv;\n"
                u8"void main()\n"
                u8"{\n"
                u8"    uv = vine_TexCoord0;\n"
                u8"    gl_Position = vec4(vine_Vertex.xy, 0.5, 1.0);\n"
                u8"}\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec2 uv;\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"layout(binding = 1) uniform sampler2D diffuseMap;\n"
                u8"void main() { outColor = texture(diffuseMap, uv); }\n";
    program->addStage(fs);
    return program;
}

/**
 * @brief Builds a program that samples a CUBE map, one named face per vertical band of the quad.
 *
 * Sampling every face from ONE draw is what makes a layer-order mistake observable: each band reads back one
 * face's colour, so an interleave that is off by a layer makes two bands swap colours. Nothing else could
 * catch that — the byte count is the same whichever order the layers are staged in, so the copies stay valid
 * and no validation layer has anything to say.
 *
 * @return The cube map sampling program.
 */
ShaderProgramPtr makeCubeSampleProgram()
{
    auto program = ShaderProgramPtr(new ShaderProgram());
    vine::graphics::ShaderStage vs;
    vs.type   = vine::graphics::ShaderStageType::Vertex;
    vs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec3 vine_Vertex;\n"
                u8"layout(location = 8) in vec2 vine_TexCoord0;\n"
                u8"layout(location = 0) out vec2 uv;\n"
                u8"void main()\n"
                u8"{\n"
                u8"    uv = vine_TexCoord0;\n"
                u8"    gl_Position = vec4(vine_Vertex.xy, 0.5, 1.0);\n"
                u8"}\n";
    program->addStage(vs);
    vine::graphics::ShaderStage fs;
    fs.type   = vine::graphics::ShaderStageType::Fragment;
    // The band's axis direction is the exact centre of a cube face, so no filtering decides which face is
    // read: the answer is decided by which layer the sampler found there.
    fs.source = u8"#version 450\n"
                u8"layout(location = 0) in vec2 uv;\n"
                u8"layout(location = 0) out vec4 outColor;\n"
                u8"layout(binding = 1) uniform samplerCube diffuseMap;\n"
                u8"void main()\n"
                u8"{\n"
                u8"    const int band = int(clamp(uv.x, 0.0, 0.999) * 6.0);\n"
                u8"    vec3 dirs[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),\n"
                u8"                            vec3(0, -1, 0), vec3(0, 0, 1), vec3(0, 0, -1));\n"
                u8"    outColor = texture(diffuseMap, dirs[band]);\n"
                u8"}\n";
    program->addStage(fs);
    return program;
}

/**
 * @brief Builds a cube map whose six faces each carry their own unmistakable colour.
 *
 * Every level repeats the face's colour, so the readback holds whichever level the sampler picks: this
 * asserts the wiring and the layer order, not the mip selection, and making a level disagree would only make
 * the test brittle.
 *
 * @param size      Face size in pixels.
 * @param mip_count Levels per face; more than one drives the layer interleave at every level rather than
 *                  only at the base.
 * @return The cube map, with all six faces filled.
 */
vine::intrusive_ptr<CubeMap> makeSixColourCube(int size, int mip_count)
{
    // In CubeMap::Face order, which is Vulkan's layer order: +X, -X, +Y, -Y, +Z, -Z.
    const std::uint8_t colours[6][3] = { { 255u, 0u, 0u },     // +X red
                                         { 0u, 255u, 0u },     // -X green
                                         { 0u, 0u, 255u },     // +Y blue
                                         { 255u, 255u, 0u },   // -Y yellow
                                         { 255u, 0u, 255u },   // +Z magenta
                                         { 0u, 255u, 255u } }; // -Z cyan

    auto cube =
        vine::intrusive_ptr<CubeMap>(new CubeMap(size, vine::imaging::PixelFormat::Rgba8Unorm, mip_count));

    for (int face = 0; face < 6; ++face) {
        auto image = vine::intrusive_ptr<vine::imaging::Image>(
            new vine::imaging::Image(size, size, vine::imaging::PixelFormat::Rgba8Unorm, mip_count));
        for (int level = 0; level < mip_count; ++level) {
            const int width  = image->mipWidth(level);
            const int height = image->mipHeight(level);
            auto*     pixels = reinterpret_cast<std::uint8_t*>(image->mipData(level).data());
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    std::uint8_t* texel = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                    static_cast<std::size_t>(x)) *
                                                       4u;
                    texel[0] = colours[face][0];
                    texel[1] = colours[face][1];
                    texel[2] = colours[face][2];
                    texel[3] = 255u;
                }
            }
        }
        cube->setFaceImage(static_cast<CubeMap::Face>(face), image);
    }

    return cube;
}

bool runTexturePhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    // A full 4-level chain, so the upload carries mip levels rather than a single base image. Every level
    // repeats the two tones: the assertion is about the wiring and the u axis, not about which level the
    // sampler picks, and making a level disagree would only make the test brittle.
    constexpr int kSize   = 8;
    constexpr int kLevels = 4;
    auto          colours = vine::intrusive_ptr<vine::imaging::Image>(
        new vine::imaging::Image(kSize, kSize, vine::imaging::PixelFormat::Rgba8Unorm, kLevels));
    for (int level = 0; level < kLevels; ++level) {
        const int width  = colours->mipWidth(level);
        const int height = colours->mipHeight(level);
        auto*     pixels = reinterpret_cast<std::uint8_t*>(colours->mipData(level).data());
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool    left  = (x * 2) < width;
                std::uint8_t* texel = pixels + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                                static_cast<std::size_t>(x)) *
                                                   4u;
                texel[0] = left ? 255u : 0u;
                texel[1] = 0u;
                texel[2] = left ? 0u : 255u;
                texel[3] = 255u;
            }
        }
    }

    vine::intrusive_ptr<Texture> texture(
        new Texture2D(kSize, kSize, vine::imaging::PixelFormat::Rgba8Unorm, kLevels));
    texture->setSource(0, colours);

    auto material = MaterialPtr(new Material());
    material->setTexture(texture);
    RenderCommand quad(makeTexturedQuad(), material, Mat4d());
    quad.program = makeTextureSampleProgram();

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"textured");
    target->setSize(96, 54);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    for (int i = 0; i < std::max(frames, 2); ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(25, 25, 45, 255), true,
                                 vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the textured target\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }

    // The quad is the middle 80% of the target, so x = 38 and x = 57 sit inside it at u = 0.24 and u = 0.73,
    // far enough from the u = 0.5 boundary at x = 48 that linear filtering blends neither sample.
    constexpr int kRow   = 27;
    constexpr int kLeft  = 38;
    constexpr int kRight = 57;
    const int     left_r  = image.at(kLeft, kRow, 0);
    const int     left_b  = image.at(kLeft, kRow, 2);
    const int     right_r = image.at(kRight, kRow, 0);
    const int     right_b = image.at(kRight, kRow, 2);
    const int     corner_r = image.at(image.corner(5), 0);
    const int     corner_b = image.at(image.corner(5), 2);

    if (left_r <= left_b + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the textured quad's left half is (%d,_,%d), not the texture's RED — the "
                     "texture was not sampled at all, or u is mirrored\n",
                     left_r, left_b);
        ok = false;
    }
    if (right_b <= right_r + 20) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the textured quad's right half is (%d,_,%d), not the texture's BLUE — the "
                     "UVs did not reach the shader, so both halves sampled one texel\n",
                     right_r, right_b);
        ok = false;
    }
    if (corner_r != 25 || corner_b != 45) {
        std::fprintf(stderr,
                     "[selftest] FAIL: the textured target's corner is (%d,_,%d), not the clear colour (25,_,45) — "
                     "the quad is not confined to the middle 80%% of the target\n",
                     corner_r, corner_b);
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] texture: the quad sampled its texture by UV — left half (%d,_,%d) = the red half, "
                     "right half (%d,_,%d) = the blue half, over a clear corner (%d,_,%d)\n",
                     left_r, left_b, right_r, right_b, corner_r, corner_b);
    }

    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

/**
 * @brief Builds a visible quad whose texcoord channel carries ONE direction for all six of its vertices.
 *
 * One direction per vertex, all equal, so the interpolated direction a fragment samples with IS that
 * direction and the sampled face is decided by the data rather than by the rasteriser.
 *
 * @param direction Direction (in the shader's space) every vertex carries.
 * @return The quad geometry.
 */
GeometryPtr makeDirectionQuad(const vine::math::Vec3f& direction)
{
    auto geom = makeVisibleQuad(0.4f, 0.0f);
    vine::geometry::Vec3fArray directions;
    for (int i = 0; i < 6; ++i) {
        directions.push_back(direction);
    }
    geom->setTexcoords3(vine::graphics::packAttribute(directions));
    return geom;
}

/**
 * @brief How many channels lead in pixel (@p x, @p y) and by how much: true when every listed channel leads.
 *
 * @param image    Read-back image.
 * @param x        Pixel column.
 * @param y        Pixel row.
 * @param leaders  Which of R/G/B must lead (1 = must lead, 0 = must trail).
 * @param margin   How much a leading channel must exceed a trailing one by.
 * @return true when the pattern holds.
 */
bool channelsLead(const PixelImage& image, int x, int y, const int leaders[3], int margin)
{
    const int value[3] = { image.at(x, y, 0), image.at(x, y, 1), image.at(x, y, 2) };
    for (int leader = 0; leader < 3; ++leader) {
        for (int trailer = 0; trailer < 3; ++trailer) {
            if (leaders[leader] != 1 || leaders[trailer] != 0) {
                continue;
            }
            if (value[leader] <= value[trailer] + margin) {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Locates the run of pixels the quad covers on the middle row.
 *
 * The custom-program phases can hard-code their sample positions (their vertex stage passes the quad's
 * positions through as clip coordinates). The engine's forward shader transforms them through the camera
 * instead, so this phase finds what was drawn and samples INSIDE it — which also keeps the assertions
 * independent of the target size and the projection.
 *
 * @param image Read-back image.
 * @param first Receives the first drawn column.
 * @param last  Receives the last drawn column.
 * @return true when the row has a run of drawn pixels.
 */
bool drawnRunOnMiddleRow(const PixelImage& image, int& first, int& last)
{
    const int row   = image.height / 2;
    first           = -1;
    last            = -1;
    for (int x = 0; x < image.width; ++x) {
        const bool drawn = image.at(x, row, 0) != 25 || image.at(x, row, 1) != 25 || image.at(x, row, 2) != 45;
        if (!drawn) {
            continue;
        }
        if (first < 0) {
            first = x;
        }
        last = x;
    }
    return first >= 0 && last > first;
}

bool runBuiltinSamplingPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"built-in sampling");
    target->setSize(96, 54);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    const auto draw_and_read = [&](const RenderCommand& command, PixelImage& image) -> bool {
        for (int i = 0; i < std::max(frames, 2); ++i) {
            FrameScope frame(renderer);
            PassScope  pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(25, 25, 45, 255), true,
                                  vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ command }, camera.get());
        }
        return readTarget(renderer, target.get(), image);
    };

    // The materials below are the engine's own defaults except for the specular term, which is switched OFF:
    // the shaded colour of a white-lit surface is then a scalar multiple of the sampled texel, so what these
    // assertions measure is the TEXTURE, not the scene's light levels.
    const auto shaded_material = [](const vine::intrusive_ptr<Texture>& texture) {
        auto material = MaterialPtr(new Material());
        material->setTexture(texture);
        material->setSpecular(vine::Colorf(0.0f, 0.0f, 0.0f, 0.0f));
        return material;
    };

    // The 2-D map: the quad's u axis runs left to right, so its left half must sample the red half.
    PixelImage uv_image;
    if (!draw_and_read(RenderCommand(makeTexturedQuad(), shaded_material(makeTwoToneTexture()), Mat4d()), uv_image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the built-in sampling target\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }
    int uv_first = -1;
    int uv_last  = -1;
    if (!drawnRunOnMiddleRow(uv_image, uv_first, uv_last)) {
        std::fprintf(stderr, "[selftest] FAIL: the built-in path drew nothing for a textured quad\n");
        ok = false;
    }
    else {
        const int row   = uv_image.height / 2;
        const int left  = uv_first + (uv_last - uv_first) / 4;
        const int right = uv_last - (uv_last - uv_first) / 4;
        const int left_r  = uv_image.at(left, row, 0);
        const int left_b  = uv_image.at(left, row, 2);
        const int right_r = uv_image.at(right, row, 0);
        const int right_b = uv_image.at(right, row, 2);
        if (left_r <= left_b + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the engine's forward shader sampled a 2-D map at u = 0.25 as "
                         "(%d,_,%d), not the texture's RED — the diffuse-map branch did not compile, so the "
                         "sampler never ran\n",
                         left_r, left_b);
            ok = false;
        }
        if (right_b <= right_r + 20) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the engine's forward shader sampled a 2-D map at u = 0.75 as "
                         "(%d,_,%d), not the texture's BLUE — the UVs did not reach the shader\n",
                         right_r, right_b);
            ok = false;
        }
        if (ok) {
            std::fprintf(stderr,
                         "[selftest] built-in sampling: the engine's own forward shader sampled a 2-D map by "
                         "UV (left (%d,_,%d) = red, right (%d,_,%d) = blue)\n",
                         left_r, left_b, right_r, right_b);
        }
    }

    // The cube map: one frame per face, the direction written into the texcoord channel. The face order is
    // CubeMap::Face order (+X, -X, +Y, -Y, +Z, -Z) and the colours are makeSixColourCube()'s: red, green,
    // blue, yellow, magenta, cyan — stated as "which channels lead", which a positive scalar shading cannot
    // change, so the assertion is about WHICH FACE was sampled and not about the light levels.
    auto cube_material = shaded_material(makeSixColourCube(8, 3));

    const vine::math::Vec3f directions[6] = { { 1.0f, 0.0f, 0.0f },  { -1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
                                             { 0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },  { 0.0f, 0.0f, -1.0f } };
    const char* face_name[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    const int   leaders[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 1, 0, 1 }, { 0, 1, 1 } };
    int         measured[6][3] = {};

    for (int face = 0; face < 6 && ok; ++face) {
        PixelImage image;
        if (!draw_and_read(RenderCommand(makeDirectionQuad(directions[face]), cube_material, Mat4d()), image)) {
            std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the cube sampling target\n");
            ok = false;
            break;
        }
        int first = -1;
        int last  = -1;
        if (!drawnRunOnMiddleRow(image, first, last)) {
            std::fprintf(stderr, "[selftest] FAIL: the built-in path drew nothing for a %s direction quad\n",
                         face_name[face]);
            ok = false;
            break;
        }
        const int row = image.height / 2;
        const int x   = first + (last - first) / 2;
        measured[face][0] = image.at(x, row, 0);
        measured[face][1] = image.at(x, row, 1);
        measured[face][2] = image.at(x, row, 2);
        if (!channelsLead(image, x, row, leaders[face], 20)) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the direction %s sampled (%d,%d,%d), which is not the colour of "
                         "face %s — the cube map was not sampled by direction\n",
                         face_name[face], measured[face][0], measured[face][1], measured[face][2], face_name[face]);
            ok = false;
        }
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] built-in sampling: the engine's own forward shader sampled a cube map by "
                     "direction — %s=(%d,%d,%d), %s=(%d,%d,%d), %s=(%d,%d,%d), %s=(%d,%d,%d), %s=(%d,%d,%d), "
                     "%s=(%d,%d,%d)\n",
                     face_name[0], measured[0][0], measured[0][1], measured[0][2], face_name[1], measured[1][0],
                     measured[1][1], measured[1][2], face_name[2], measured[2][0], measured[2][1], measured[2][2],
                     face_name[3], measured[3][0], measured[3][1], measured[3][2], face_name[4], measured[4][0],
                     measured[4][1], measured[4][2], face_name[5], measured[5][0], measured[5][1], measured[5][2]);
    }

    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

bool runCubeMapPhase(vine::vsg::VsgRenderer& renderer, const CameraPtr& camera, int frames)
{
    bool ok = true;

    // Three levels, so the interleave runs for more than one level: a repack that only got the base level
    // right would still upload something that looks valid at level 0.
    //
    // The face size is 256 for the same reason it is not 8 any more: the bytes were once staged with a
    // per-layer step of `row stride * width * height` (width TIMES too far, because vsg multiplies the
    // element size by texel counts), and at 8x8 the 16 MiB minimum staging buffer absorbed that step
    // completely. The step only runs off the buffer once a layer is big enough to matter — so the phase has
    // to run at a size like this, which is also the size the demo loads, or it cannot see the defect it is
    // here for.
    constexpr int kSize   = 256;
    constexpr int kLevels = 3;
    auto          cube    = makeSixColourCube(kSize, kLevels);

    auto material = MaterialPtr(new Material());
    material->setTexture(cube);
    RenderCommand quad(makeTexturedQuad(), material, Mat4d());
    quad.program = makeCubeSampleProgram();

    auto target = RenderTargetPtr(new RenderTarget());
    target->setName(u8"cube map");
    target->setSize(96, 54);
    target->attachColor(RenderTarget::ColorFormat::RGBA8);
    target->attachDepth(RenderTarget::DepthFormat::D32);
    auto pass = RenderPassPtr(new RenderPass());

    for (int i = 0; i < std::max(frames, 2); ++i) {
        FrameScope frame(renderer);
        {
            PassScope pass_scope(renderer, pass.get(), 0, target.get(), vine::Color(25, 25, 45, 255), true,
                                 vine::graphics::DepthMode::TestAndWrite);
            renderer.render(std::vector<RenderCommand>{ quad }, camera.get());
        }
    }

    PixelImage image;
    if (!readTarget(renderer, target.get(), image)) {
        std::fprintf(stderr, "[selftest] FAIL: readColorBuffer() refused the cube map target\n");
        renderer.releasePass(pass.get());
        renderer.releaseRenderTarget(target.get());
        return false;
    }

    // The quad is the middle 80% of the target, so a band is 6.4 px wide and its centre sits at 32 + 6.4k.
    constexpr int kRow = 27;
    const int     band_x[6] = { 32, 38, 45, 51, 58, 64 };
    const char*   face_name[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    // In CubeMap::Face order, written out again rather than shared with makeSixColourCube().
    const int expected[6][3] = { { 255, 0, 0 },   { 0, 255, 0 },   { 0, 0, 255 },
                                 { 255, 255, 0 }, { 255, 0, 255 }, { 0, 255, 255 } };

    for (int band = 0; band < 6; ++band) {
        const int r = image.at(band_x[band], kRow, 0);
        const int g = image.at(band_x[band], kRow, 1);
        const int b = image.at(band_x[band], kRow, 2);
        if (std::abs(r - expected[band][0]) > 8 || std::abs(g - expected[band][1]) > 8 ||
            std::abs(b - expected[band][2]) > 8) {
            std::fprintf(stderr,
                         "[selftest] FAIL: the cube map's %s band is (%d,%d,%d), not face %d's colour — the six "
                         "layers did not reach the sampler in CubeMap::Face order\n",
                         face_name[band], r, g, b, band);
            ok = false;
        }
    }
    if (ok) {
        std::fprintf(stderr,
                     "[selftest] cube map: all six faces sampled in CubeMap::Face order (+X,-X,+Y,-Y,+Z,-Z), "
                     "one pixel read back per face (%dx%d faces, %d levels)\n",
                     kSize, kSize, kLevels);
    }

    renderer.releasePass(pass.get());
    renderer.releaseRenderTarget(target.get());
    return ok;
}

}  // namespace selftest
