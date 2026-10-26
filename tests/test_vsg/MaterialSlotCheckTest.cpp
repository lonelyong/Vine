/**
 * @brief The material slot contract, asked of the facts (graphics-material.md 2.9 / C2 / F11).
 *
 * The check is a pure function over `MaterialFacts`, which is what lets these cases run with no device: a slot's
 * map knows its kind and its pixel format, and that is the whole question.
 */

#include <gtest/gtest.h>

#include <vine/vsg/internal/MaterialSlotCheck.hpp>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/Texture.hpp>
#include <vine/imaging/PixelFormat.hpp>

using vn::vsg::materialSlotName;
using vn::vsg::materialSlotViolation;
using vn::vsg::SlotViolation;
using vn::vsg::slotViolationText;

namespace
{

/// @brief The slot index of @p slot, as the check takes it.
std::size_t indexOf(vn::graphics::TextureSlot slot)
{
    return static_cast<std::size_t>(slot);
}

/// @brief A 2-D map of @p format (a description is enough: the check reads kind and format only).
vn::intrusive_ptr<vn::graphics::Texture> map2d(vn::imaging::PixelFormat format)
{
    return vn::intrusive_ptr<vn::graphics::Texture>(new vn::graphics::Texture2D(4, 4, format));
}

/// @brief A cube map of @p format.
vn::intrusive_ptr<vn::graphics::Texture> cubeMap(vn::imaging::PixelFormat format)
{
    return vn::intrusive_ptr<vn::graphics::Texture>(new vn::graphics::CubeMap(4, format));
}

}  // namespace

TEST(MaterialSlotCheckTest, AnAbsentSlotIsNotAViolation)
{
    // "The material does not have this map" is a legal material (graphics-material.md 2.5): the flags say so and
    // the slot reads as its constant. Only a map that does NOT fit its slot is the diagnostic's business.
    const vn::vsg::MaterialFacts facts{};
    for (std::uint32_t slot = 0; slot < vn::graphics::kTextureSlotCount; ++slot) {
        EXPECT_EQ(materialSlotViolation(facts, slot), SlotViolation::None);
    }
}

TEST(MaterialSlotCheckTest, OnlyTheBaseColourSlotTakesACubeMap)
{
    // N6 / C2: the other four parameterise a surface with a UV pair, and a direction is not one - the content
    // layer cannot even bind it (a cube view where the text declares sampler2D is an invalid descriptor), so this
    // is the violation a host is most likely to meet.
    const auto            cube_in_data = cubeMap(vn::imaging::PixelFormat::Rgba8Unorm);
    const auto            cube_in_base = cubeMap(vn::imaging::PixelFormat::Rgba8Srgb);
    vn::vsg::MaterialFacts facts{};
    facts.slots[indexOf(vn::graphics::TextureSlot::Normal)].texture = cube_in_data.get();
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::Normal)), SlotViolation::CubeInDataSlot);

    vn::vsg::MaterialFacts base{};
    base.slots[indexOf(vn::graphics::TextureSlot::BaseColor)].texture = cube_in_base.get();
    EXPECT_EQ(materialSlotViolation(base, indexOf(vn::graphics::TextureSlot::BaseColor)), SlotViolation::None)
        << "the base colour slot is the one that may be a cube (a sky box is exactly that)";
}

TEST(MaterialSlotCheckTest, ColourSlotsWantColourFormats)
{
    // A byte map that claims to be linear in a colour slot is the silent gamma shift: the hardware does not
    // decode it, so every colour arrives brighter than the file said.
    const auto            linear_base     = map2d(vn::imaging::PixelFormat::Rgba8Unorm);
    const auto            linear_emissive = map2d(vn::imaging::PixelFormat::Bgra8Unorm);
    vn::vsg::MaterialFacts facts{};
    facts.slots[indexOf(vn::graphics::TextureSlot::BaseColor)].texture = linear_base.get();
    facts.slots[indexOf(vn::graphics::TextureSlot::Emissive)].texture  = linear_emissive.get();
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::BaseColor)),
              SlotViolation::LinearFormatInColorSlot);
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::Emissive)),
              SlotViolation::LinearFormatInColorSlot);

    // The spellings that FIT: an sRGB byte map, and a float map (an HDR emission is linear by definition - it is
    // not a violation, and guessing would make the report noise).
    const auto srgb_base     = map2d(vn::imaging::PixelFormat::Rgba8Srgb);
    const auto hdr_emissive  = map2d(vn::imaging::PixelFormat::Rgba16Float);
    facts.slots[indexOf(vn::graphics::TextureSlot::BaseColor)].texture = srgb_base.get();
    facts.slots[indexOf(vn::graphics::TextureSlot::Emissive)].texture  = hdr_emissive.get();
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::BaseColor)), SlotViolation::None);
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::Emissive)), SlotViolation::None);
}

TEST(MaterialSlotCheckTest, DataSlotsWantLinearFormats)
{
    // The other direction, and the one that costs accuracy rather than brightness: an sRGB roughness map is
    // decoded by the hardware, so an authored 0.5 arrives as 0.21.
    const auto            srgb_mr   = map2d(vn::imaging::PixelFormat::Rgba8Srgb);
    const auto            srgb_ao   = map2d(vn::imaging::PixelFormat::R8Srgb);
    vn::vsg::MaterialFacts facts{};
    facts.slots[indexOf(vn::graphics::TextureSlot::MetallicRoughness)].texture = srgb_mr.get();
    facts.slots[indexOf(vn::graphics::TextureSlot::Occlusion)].texture         = srgb_ao.get();
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::MetallicRoughness)),
              SlotViolation::SrgbFormatInDataSlot);
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::Occlusion)),
              SlotViolation::SrgbFormatInDataSlot);

    const auto linear_mr     = map2d(vn::imaging::PixelFormat::Rgba8Unorm);
    const auto linear_normal = map2d(vn::imaging::PixelFormat::Rgba8Unorm);
    facts.slots[indexOf(vn::graphics::TextureSlot::MetallicRoughness)].texture = linear_mr.get();
    facts.slots[indexOf(vn::graphics::TextureSlot::Normal)].texture            = linear_normal.get();
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::MetallicRoughness)), SlotViolation::None);
    EXPECT_EQ(materialSlotViolation(facts, indexOf(vn::graphics::TextureSlot::Normal)), SlotViolation::None);
}

TEST(MaterialSlotCheckTest, AnOutOfRangeSlotSaysNothing)
{
    // A slot index that is not a slot is the caller's mistake, not a material's: the check answers "nothing" and
    // the name helper has no name, which is what a caller iterating a bigger range sees.
    const vn::vsg::MaterialFacts facts{};
    EXPECT_EQ(materialSlotViolation(facts, vn::graphics::kTextureSlotCount), SlotViolation::None);
    EXPECT_EQ(materialSlotName(vn::graphics::kTextureSlotCount), nullptr);
}

TEST(MaterialSlotCheckTest, TheSentenceNamesTheBindingTheShaderUses)
{
    // The message names the slot by the name its BINDING is resolved by (api/ContentImages::materialSlotOf), so a
    // host can connect the sentence to its shader's declaration without a second table.
    EXPECT_STREQ(materialSlotName(indexOf(vn::graphics::TextureSlot::BaseColor)), "vn_baseColorMap");
    EXPECT_STREQ(materialSlotName(indexOf(vn::graphics::TextureSlot::MetallicRoughness)), "vn_metallicRoughnessMap");
    EXPECT_STREQ(materialSlotName(indexOf(vn::graphics::TextureSlot::Normal)), "vn_normalMap");
    EXPECT_STREQ(materialSlotName(indexOf(vn::graphics::TextureSlot::Occlusion)), "vn_occlusionMap");
    EXPECT_STREQ(materialSlotName(indexOf(vn::graphics::TextureSlot::Emissive)), "vn_emissiveMap");

    EXPECT_STREQ(slotViolationText(SlotViolation::None), "");
    EXPECT_STRNE(slotViolationText(SlotViolation::CubeInDataSlot), "");
    EXPECT_STRNE(slotViolationText(SlotViolation::LinearFormatInColorSlot), "");
    EXPECT_STRNE(slotViolationText(SlotViolation::SrgbFormatInDataSlot), "");
}
