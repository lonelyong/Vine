#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vine/Buffer.hpp>
#include <vine/Color.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/ShaderProgram.hpp>
#include <vine/intrusive_ptr.hpp>

#include "TestGeometryBuilders.hpp"

namespace vn::test
{

/**
 * @brief The content one backend case draws: a program, one geometry and its single drawing call.
 *
 * @details
 * - Every call builds FRESH buffers, which is what makes it a fresh stream - and therefore a fresh upload - in the
 *   stream layer (see `StreamUploads`: the identity is the buffer, its revision and the segment).
 * - Shared by the backend cases that need "something to upload" (see UploadScratchTest, FrameUploadsTest) so the
 *   fixture is stated once: the program is a view-block vertex stage and a constant red fragment stage.
 */
struct TriangleFixture {
    vn::intrusive_ptr<vn::graphics::ShaderProgram> program;
    vn::intrusive_ptr<vn::graphics::Geometry>      geometry;
    vn::intrusive_ptr<vn::graphics::Material>      material;
    /// The position stream the geometry reads: the buffer whose revision the stream layer keys an upload by.
    vn::intrusive_ptr<vn::Buffer<float>> positions;
    std::vector<vn::graphics::RenderCommand> commands;
};

/**
 * @brief Builds a triangle drawing @p positions (three floats per vertex) through a constant-red program.
 *
 * @param positions Position scalars, in the order the vertex stage reads them.
 * @return The fixture; its one command draws the geometry's first primitive.
 */
inline TriangleFixture triangleFromPositions(std::vector<float> positions)
{
    TriangleFixture fixture;

    const vn::intrusive_ptr<vn::graphics::ShaderProgram> program(new vn::graphics::ShaderProgram());
    {
        vn::graphics::ShaderStage vertex;
        vertex.type   = vn::graphics::ShaderStageType::Vertex;
        vertex.source = vn::String(reinterpret_cast<const char8_t*>("layout(location = 0) in vec3 position;\n"
                                                                    "layout(set = 0, binding = 0, std140) uniform VineViewBlock {\n"
                                                                    "    mat4 view; mat4 inv_view; mat4 proj; mat4 view_proj; vec4 cam_pos; vec4 frame; } vb;\n"
                                                                    "void main() { gl_Position = vb.view_proj * vec4(position, 1.0); }\n"));
        vn::graphics::ShaderStage fragment;
        fragment.type   = vn::graphics::ShaderStageType::Fragment;
        fragment.source = vn::String(reinterpret_cast<const char8_t*>("layout(location = 0) out vec4 outColor;\n"
                                                                      "void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }\n"));
        program->addStage(vertex);
        program->addStage(fragment);
    }
    fixture.program = program;

    const vn::intrusive_ptr<vn::graphics::Geometry>          geometry(new vn::graphics::Geometry());
    const vn::intrusive_ptr<vn::Buffer<float>>               vertices =
        vn::intrusive_ptr<vn::Buffer<float>>(new vn::Buffer<float>(std::move(positions)));
    const vn::intrusive_ptr<vn::Buffer<std::uint32_t>> indices =
        vn::intrusive_ptr<vn::Buffer<std::uint32_t>>(new vn::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
    geometry->setPositions(vertices);
    setPrimitiveIndices(*geometry, 0U, indices);
    geometry->setRevision(1U);
    fixture.geometry  = geometry;
    fixture.positions = vertices;

    const vn::intrusive_ptr<vn::graphics::Material> material(new vn::graphics::Material());
    material->setBaseColor(vn::Colorf(0.2F, 0.3F, 0.4F, 1.0F));
    fixture.material = material;

    vn::graphics::RenderCommand command;
    command.geometry = geometry;
    command.material = material;
    command.program  = program;
    fixture.commands.push_back(command);
    return fixture;
}

/** @brief The ordinary triangle: three vertices in [-0.4, 0.6], a few bytes of stream. */
inline TriangleFixture makeTriangle()
{
    return triangleFromPositions({ -0.4F, -0.4F, 0.5F, 0.4F, -0.4F, 0.5F, 0.0F, 0.6F, 0.5F });
}

/**
 * @brief The triangle with a position stream LARGER than vsg's minimum staging block.
 *
 * @details
 * - One-off uploads are what the staging release exists for: a model load stages its own size and that weight must not
 *   outlive its copy. The stream upload is the whole position channel, so a buffer above `minimumStagingBufferSize`
 *   (16 MiB) makes vsg size that block's staging above the reusable floor.
 *
 * @return The fixture; the first three vertices form the drawn triangle.
 */
inline TriangleFixture makeBigTriangle()
{
    constexpr std::size_t kBytes   = 17U * 1024U * 1024U; // one MiB over vsg's 16 MiB minimum
    constexpr std::size_t kVertices = kBytes / (3U * sizeof(float));

    std::vector<float> positions(kVertices * 3U, 0.0F);
    positions[0] = -0.4F;
    positions[1] = -0.4F;
    positions[2] = 0.5F;
    positions[3] = 0.4F;
    positions[4] = -0.4F;
    positions[5] = 0.5F;
    positions[6] = 0.0F;
    positions[7] = 0.6F;
    positions[8] = 0.5F;
    return triangleFromPositions(std::move(positions));
}

/** @brief A camera looking at (0, 0, 0) from (0, 0, 1.5), through an orthographic window of [-1, 1] squared. */
inline vn::intrusive_ptr<vn::graphics::Camera> makeCamera()
{
    vn::intrusive_ptr<vn::graphics::Camera> camera(new vn::graphics::Camera());
    camera->lookAt(vn::math::Vec3d(0.0, 0.0, 1.5), vn::math::Vec3d(0.0, 0.0, 0.0), vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setOrthographic(-1.0, 1.0, -1.0, 1.0, 0.5, 4.0);
    return camera;
}

} // namespace vn::test
