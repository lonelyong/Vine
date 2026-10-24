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

TEST(ShaderAbiTest, TheAlphaModeDecidesWhatTheMaterialsAlphaMeans)
{
    // ONE PRODUCT, THREE USES (graphics-material.md 2.7/3.5). The effective alpha is
    // `draw.params.x` (the drawable's opacity) x the material's own alpha x its base map's alpha, and the
    // material's MODE says what that product means: Opaque ignores both material alphas, Mask uses the product
    // as a threshold and keeps its survivors fully opaque, Blend uses it as the blend factor. This pins the
    // three branches in the shipped text, because the structural gates around the block layout and the variant
    // defines are blind to a factor that went missing from a float expression.
    const std::string forward_fs = asByteString(shaders::kBuiltinForwardFrag);
    EXPECT_NE(forward_fs.find("(material.flags.x >> VINE_MATERIAL_ALPHA_MODE_SHIFT) & 3u"), std::string::npos)
        << "the mode rides the block's flag row (bits 8-9) - a shader must not invent its own numbering";
    EXPECT_NE(forward_fs.find("if (draw.params.x * surface.alpha < material.params.w)"), std::string::npos)
        << "Mask cuts on the PRODUCT of the two alphas against the material's cutoff";
    EXPECT_NE(forward_fs.find("discard;"), std::string::npos) << "Mask discards, it does not fade";
    EXPECT_NE(forward_fs.find("alpha = draw.params.x * surface.alpha;"), std::string::npos)
        << "Blend outputs the product, which is what the transparent queue sorted by";
    // The material's alpha is read through the surface (one place, both paths), not multiplied in ad hoc here:
    // the G-buffer writer asks the same function the same question. What this pins is that the stage's own
    // branches use `surface.alpha` and never a texel it sampled itself.
    EXPECT_EQ(forward_fs.find("vine_color.a"), std::string::npos)
        << "a vertex colour's alpha is still not an opacity input (it means what the model says it means)";
    const std::string material_fs = asByteString(shaders::kBuiltinForwardFrag);
    EXPECT_NE(material_fs.find("surface.alpha      = clamp(material.base_color.a, 0.0, 1.0);"), std::string::npos)
        << "the inlined sample function is where the material's own alpha enters";
    EXPECT_NE(material_fs.find("surface.alpha      *= texel.a;"), std::string::npos)
        << "and the base slot's own alpha multiplies it - the shape of a cut-out leaf lives there";

    // The G-buffer records opaque and masked content only (N4): a blended surface has no single value to store.
    const std::string gbuffer_fs = asByteString(shaders::kBuiltinGbufferFrag);
    EXPECT_NE(gbuffer_fs.find("(material.flags.x >> VINE_MATERIAL_ALPHA_MODE_SHIFT) & 3u"), std::string::npos)
        << "the writer reads the same mode bits";
    EXPECT_NE(gbuffer_fs.find("if (alpha_mode == 2u)"), std::string::npos) << "Blend is not recorded";
    EXPECT_NE(gbuffer_fs.find("draw.params.x * surface.alpha < material.params.w"), std::string::npos)
        << "and Mask cuts the same holes the forward stage does - the same two inputs, not a copy of one";
    EXPECT_EQ(forward_fs.find("alphaMask"), std::string::npos);
    EXPECT_EQ(gbuffer_fs.find("alphaMask"), std::string::npos);
}

}  // namespace
