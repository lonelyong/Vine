/**
 * @brief The engine's shader ABI: attribute locations and the L1 data-block layouts.
 *
 * The attribute table and the block structs are the contract a backend materialises
 * (see .ai/design/graphics-shader.md §11/§12). Their values are pinned here so a change
 * that would silently re-map a shader's inputs, or shift a block's std140/D3D-cbuffer
 * offsets, fails a unit test instead of showing up as a wrong picture.
 */

#include <gtest/gtest.h>

#include <vine/graphics/EmbeddedShaders.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/ShaderAbi.hpp>

#include <cstddef>
#include <vector>
#include <string>

using namespace vine::graphics;

namespace
{

/**
 * @brief Reinterprets UTF-8 shader text as a searchable byte string.
 *
 * @param text Shader source.
 * @return The same bytes as a std::string.
 */
std::string asByteString(std::u8string_view text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

TEST(ShaderAbiTest, AttributeLocationsAreTheCanonicalValues)
{
    // 8 is the reserved texcoord slot, deliberately not the next number after the
    // custom-channel range: a forwarded custom channel keeps its source location, so
    // a canonical role must not sit where a custom channel could land.
    EXPECT_EQ(attributeLocation(VertexAttribute::Position), 0u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Normal), 1u);
    EXPECT_EQ(attributeLocation(VertexAttribute::Color), 2u);
    EXPECT_EQ(attributeLocation(VertexAttribute::TexCoord0), 8u);
}

TEST(ShaderAbiTest, TheCanonicalPredicateMatchesTheLocations)
{
    // "Is this one of the engine's own channels, or a forwarded custom one?" is asked by every
    // consumer that walks a geometry's channels, so it is answered by the ABI (and not by a range
    // test: the reserved texcoord slot is exactly the value a range test gets wrong).
    for (const auto attribute : { VertexAttribute::Position, VertexAttribute::Normal, VertexAttribute::Color,
                                  VertexAttribute::TexCoord0 }) {
        EXPECT_TRUE(isCanonicalAttributeLocation(attributeLocation(attribute)));
    }
    EXPECT_FALSE(isCanonicalAttributeLocation(3u)) << "3 upward is the custom-channel range";
    EXPECT_FALSE(isCanonicalAttributeLocation(7u)) << "the texcoord slot's neighbours are not canonical";
}

TEST(ShaderAbiTest, GeometryAttachesCanonicalChannelsWhereTheAbiSays)
{
    // The geometry setters must not carry their own copy of the locations: they attach (and look up)
    // a canonical channel AT the ABI's location, which is what this pins. A setter with a literal in
    // it fails the moment the ABI moves that role — and it fails HERE rather than as a geometry that
    // silently loses its normals in a backend.
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(packAttribute(std::vector<vine::math::Vec3f>{ { 0.0f, 0.0f, 0.0f } }));
    geometry->setNormals(packAttribute(std::vector<vine::math::Vec3f>{ { 0.0f, 0.0f, 1.0f } }));
    geometry->setTexcoords(packAttribute(std::vector<vine::math::Vec2f>{ { 0.0f, 0.0f } }));

    EXPECT_TRUE(geometry->hasBuffer(attributeLocation(VertexAttribute::Position)));
    EXPECT_TRUE(geometry->hasBuffer(attributeLocation(VertexAttribute::Normal)));
    EXPECT_TRUE(geometry->hasBuffer(attributeLocation(VertexAttribute::TexCoord0)));
    EXPECT_EQ(geometry->buffer(attributeLocation(VertexAttribute::Position)), geometry->buffer(0))
        << "positionCount() and the backend must be reading the SAME channel";
    EXPECT_EQ(Geometry::kTexCoordLocation, attributeLocation(VertexAttribute::TexCoord0));
}

TEST(ShaderAbiTest, ViewBlockIsFourMatricesAndTwoVec4)
{
    // std140 and D3D cbuffer packing agree on this shape: 16-byte aligned, all
    // members mat4/vec4, so one layout serves both APIs.
    EXPECT_EQ(sizeof(VineViewBlock), 288u);
    EXPECT_EQ(alignof(VineViewBlock), 16u);
    EXPECT_EQ(offsetof(VineViewBlock, view), 0u);
    EXPECT_EQ(offsetof(VineViewBlock, inv_view), 64u);
    EXPECT_EQ(offsetof(VineViewBlock, proj), 128u);
    EXPECT_EQ(offsetof(VineViewBlock, view_proj), 192u);
    EXPECT_EQ(offsetof(VineViewBlock, cam_pos), 256u);
    EXPECT_EQ(offsetof(VineViewBlock, frame), 272u);
}

TEST(ShaderAbiTest, DrawBlockIsOneMatrixPlusTheParameterSlot)
{
    EXPECT_EQ(sizeof(VineDrawBlock), 80u);
    EXPECT_EQ(alignof(VineDrawBlock), 16u);
    EXPECT_EQ(offsetof(VineDrawBlock, model), 0u);
    EXPECT_EQ(offsetof(VineDrawBlock, params), 64u);
}

TEST(ShaderAbiTest, TheShaderBlockNamesAreTheL1Names)
{
    // The L1 name and the GLSL block type are the same string, so the contract and the
    // source need no translation table; renaming one without the other is the drift
    // this pins.
    const std::string forward_fs = asByteString(shaders::kVineForwardFrag);
    EXPECT_NE(forward_fs.find("uniform VineMaterialBlock"), std::string::npos);
    EXPECT_NE(forward_fs.find("uniform VineLightsBlock"), std::string::npos);

    const std::string gbuffer_fs = asByteString(shaders::kGbufferGeometryFrag);
    EXPECT_NE(gbuffer_fs.find("uniform VineMaterialBlock"), std::string::npos);
}

}  // namespace
