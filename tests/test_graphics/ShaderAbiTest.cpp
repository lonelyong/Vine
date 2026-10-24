/**
 * @brief The engine's shader ABI: attribute locations and the L1 data-block layouts.
 *
 * The attribute table and the block structs are the contract a backend materialises
 * (see .ai/design/graphics-rendering.md §11/§12). Their values are pinned here so a change
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

using namespace vn::graphics;

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
    // The instance matrix is four consecutive slots past the custom-channel range: the shader
    // declares them as one mat4, and the backend binds one per-instance stream to feed all four.
    EXPECT_EQ(attributeLocation(VertexAttribute::InstanceModel), 12u);
    EXPECT_EQ(instanceMatrixLocation(0u), 12u);
    EXPECT_EQ(instanceMatrixLocation(3u), 15u);
}

TEST(ShaderAbiTest, TheCanonicalPredicateMatchesTheLocations)
{
    // "Is this one of the engine's own channels, or a forwarded custom one?" is asked by every
    // consumer that walks a geometry's channels, so it is answered by the ABI (and not by a range
    // test: the reserved texcoord slot is exactly the value a range test gets wrong).
    for (const auto attribute : { VertexAttribute::Position, VertexAttribute::Normal, VertexAttribute::Color,
                                  VertexAttribute::TexCoord0, VertexAttribute::InstanceModel }) {
        EXPECT_TRUE(isCanonicalAttributeLocation(attributeLocation(attribute)));
    }
    EXPECT_FALSE(isCanonicalAttributeLocation(3u)) << "3 upward is the custom-channel range";
    EXPECT_FALSE(isCanonicalAttributeLocation(7u)) << "the texcoord slot's neighbours are not canonical";
    EXPECT_FALSE(isCanonicalAttributeLocation(11u)) << "the slot before the instance matrix is custom";
    EXPECT_TRUE(isCanonicalAttributeLocation(13u)) << "the matrix's second column";
    EXPECT_TRUE(isCanonicalAttributeLocation(15u)) << "the matrix's fourth column";
    EXPECT_FALSE(isCanonicalAttributeLocation(16u)) << "the slot after the matrix is custom";
}

TEST(ShaderAbiTest, GeometryAttachesCanonicalChannelsWhereTheAbiSays)
{
    // The geometry setters must not carry their own copy of the locations: they attach (and look up)
    // a canonical channel AT the ABI's location, which is what this pins. A setter with a literal in
    // it fails the moment the ABI moves that role — and it fails HERE rather than as a geometry that
    // silently loses its normals in a backend.
    auto geometry = GeometryPtr(new Geometry());
    geometry->setPositions(packAttribute(std::vector<vn::math::Vec3f>{ { 0.0f, 0.0f, 0.0f } }));
    geometry->setNormals(packAttribute(std::vector<vn::math::Vec3f>{ { 0.0f, 0.0f, 1.0f } }));
    geometry->setTexcoords2(packAttribute(std::vector<vn::math::Vec2f>{ { 0.0f, 0.0f } }));

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

TEST(ShaderAbiTest, ShadowBlockIsTheViewToLightMatrixPlusItsParameters)
{
    // A shadow is sampled by mapping the shaded fragment into the LIGHT's clip space, and the
    // shading already has the fragment's VIEW position (a varying in the forward stage, a G-buffer
    // attachment in the deferred one). So the matrix that block carries is view -> light clip, and
    // its parameters ride in the slot that follows it — the same shape as the draw block, which is
    // what makes it fit a backend that binds only one such block per pass.
    EXPECT_EQ(sizeof(VineShadowBlock), 80u);
    EXPECT_EQ(alignof(VineShadowBlock), 16u);
    EXPECT_EQ(offsetof(VineShadowBlock, view_to_light), 0u);
    EXPECT_EQ(offsetof(VineShadowBlock, params), 64u);
}

TEST(ShaderAbiTest, TheMaterialBlockIsThePbrRowsAndEqualityCoversThem)
{
    // Four 16-byte std140 rows (graphics-material.md §3.4), and the members end exactly at byte 64 - there is no
    // tail padding left. A backend asks "does the GPU need this material again?" by comparing two blocks through
    // the type's own defaulted operator==: a field list written out in a backend is a second copy of this layout,
    // and one that falls behind it fails silently - the symptom is a material edit that never reaches the GPU.
    // Flipping one bit of every member byte and demanding that the comparison notices pins that coverage without
    // naming the members.
    EXPECT_EQ(sizeof(VineMaterialBlock), 64u) << "two colour rows, the factor row and the flag row";
    EXPECT_EQ(alignof(VineMaterialBlock), 16u);
    EXPECT_EQ(offsetof(VineMaterialBlock, base_color), 0u);
    EXPECT_EQ(offsetof(VineMaterialBlock, emissive), 16u);
    EXPECT_EQ(offsetof(VineMaterialBlock, params), 32u);
    EXPECT_EQ(offsetof(VineMaterialBlock, flags), 48u);

    VineMaterialBlock a;
    // Every field is non-zero: a bit flipped inside a zero float is a denormal, and this test must
    // not depend on how denormals compare.
    a.base_color = { 0.1f, 0.2f, 0.3f, 1.0f };
    a.emissive   = { 0.4f, 0.5f, 0.6f, 2.0f };
    a.params     = { 0.7f, 0.8f, 0.9f, 1.0f };
    a.flags      = { 2u << kMaterialAlphaModeShift, 1u, 0u, 0u };

    VineMaterialBlock b = a;
    EXPECT_TRUE(a == b);

    auto* bytes = reinterpret_cast<unsigned char*>(&b);
    for (std::size_t offset = 0; offset < sizeof(VineMaterialBlock); ++offset) {
        bytes[offset] ^= 0x01u;
        EXPECT_TRUE(a != b) << "a change at byte " << offset << " must be visible to the comparison";
        bytes[offset] ^= 0x01u;
    }
    EXPECT_TRUE(a == b) << "the scan must leave the block as it found it";

    // The defaults are the default material's values, so content without a material shades like content with a
    // default one (see ContentSourcesTest.NoMaterialIsTheDefaultMaterialNotAMiss).
    const VineMaterialBlock defaults{};
    EXPECT_EQ(defaults.base_color[0], 0.8f);
    EXPECT_EQ(defaults.base_color[3], 1.0f);
    EXPECT_EQ(defaults.params[0], 0.0f);
    EXPECT_EQ(defaults.params[1], 0.5f);
    EXPECT_EQ(defaults.params[2], 1.0f);
    EXPECT_EQ(defaults.params[3], 0.5f);
    EXPECT_EQ(defaults.flags[0], 0u) << "no texture slots, opaque";
}

TEST(ShaderAbiTest, TheShaderBlockNamesAreTheL1Names)
{
    // The L1 name and the GLSL block type are the same string, so the contract and the
    // source need no translation table; renaming one without the other is the drift
    // this pins.
    const std::string forward_fs = asByteString(shaders::kBuiltinForwardFrag);
    EXPECT_NE(forward_fs.find("uniform VineMaterialBlock"), std::string::npos);
    EXPECT_NE(forward_fs.find("uniform VineLightsBlock"), std::string::npos);

    const std::string gbuffer_fs = asByteString(shaders::kBuiltinGbufferFrag);
    EXPECT_NE(gbuffer_fs.find("uniform VineMaterialBlock"), std::string::npos);
}

TEST(ShaderAbiTest, TheForwardFragmentsAlphaIsTheDrawablesOpacityAlone)
{
    // The engine has ONE transparency channel - the per-drawable opacity - and it is the one the
    // collection pass sorts by, the one the deferred path carries, and the one the opacity pixel gate
    // measures. The forward stage used to multiply the material's own alpha and the sampled texel's
    // alpha into its output alpha as well: the object then blended (blending is always on for the
    // content pipelines) while the engine had classified it opaque and sorted it front-to-back, and
    // the SAME asset stayed opaque on the deferred path (whose lighting program writes alpha 1). This
    // pins the one-input rule in the text, because the structural gates around the block layout and
    // the variant defines are all blind to an extra factor in a float expression.
    const std::string forward_fs = asByteString(shaders::kBuiltinForwardFrag);
    EXPECT_NE(forward_fs.find("float alpha = draw.params.x;"), std::string::npos)
        << "the fragment alpha must be the drawable's opacity";
    EXPECT_EQ(forward_fs.find("material.base_color.a"), std::string::npos)
        << "the material's alpha must not scale the fragment alpha (a shared material would make "
           "every drawable that uses it translucent, in an order nobody sorted)";
    EXPECT_EQ(forward_fs.find("alpha *= texel.a"), std::string::npos)
        << "the sampled texel's alpha must not scale the fragment alpha either (same reason, and the "
           "deferred path discards it, so the two paths would disagree about one asset)";

    // The block the shading declares is the engine's material ABI and nothing else: fields no accessor
    // could set and no stage read are gone rather than declared (see VineMaterialBlock).
    EXPECT_EQ(forward_fs.find("alphaMask"), std::string::npos);
    const std::string gbuffer_fs = asByteString(shaders::kBuiltinGbufferFrag);
    EXPECT_EQ(gbuffer_fs.find("alphaMask"), std::string::npos);
}

}  // namespace
