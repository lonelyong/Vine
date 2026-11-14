/**
 * @brief The material block's PACKING RULE (the SDK's one `packMaterialBlock`), device-free.
 *
 * Three silent failures this pins: a factor that reaches one backend but not the other; a value that is clamped on
 * the way (a material is data the host authored - the shader clamps what it draws); and a flag row assembled
 * differently from the slot bits the caller actually binds.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/Colorf.hpp>
#include <vine/graphics/MaterialAbi.hpp>
#include <vine/graphics/MaterialBase.hpp>

using vn::graphics::AlphaMode;
using vn::graphics::kMaterialAlphaModeShift;
using vn::graphics::kMaterialFlagBaseColor;
using vn::graphics::kMaterialFlagEmissive;
using vn::graphics::kMaterialFlagNormal;
using vn::graphics::MaterialParameters;
using vn::graphics::packMaterialBlock;
using vn::graphics::VineMaterialBlock;

TEST(MaterialBlockTest, TheFactorsLandInTheirRowsAndTheCapacityStripesInTheFlags)
{
    MaterialParameters values;
    values.base_color   = vn::Colorf(0.25, 0.5, 0.75, 0.875);
    values.emissive     = vn::Colorf(0.1, 0.2, 0.3, 0.4);
    values.metallic     = 0.6F;
    values.roughness    = 0.2F;
    values.occlusion    = 0.9F;
    values.alpha_mode   = AlphaMode::Blend;
    values.alpha_cutoff = 0.35F;

    VineMaterialBlock block;
    packMaterialBlock(values, kMaterialFlagBaseColor | kMaterialFlagNormal, block);

    EXPECT_FLOAT_EQ(block.base_color[0], 0.25F);
    EXPECT_FLOAT_EQ(block.base_color[3], 0.875F);
    EXPECT_FLOAT_EQ(block.emissive[2], 0.3F);
    EXPECT_FLOAT_EQ(block.emissive[3], 0.4F);
    EXPECT_FLOAT_EQ(block.params[0], 0.6F) << "params.x is metallic";
    EXPECT_FLOAT_EQ(block.params[1], 0.2F) << "params.y is roughness";
    EXPECT_FLOAT_EQ(block.params[2], 0.9F) << "params.z is occlusion";
    EXPECT_FLOAT_EQ(block.params[3], 0.35F) << "params.w is the alpha cutoff";

    EXPECT_EQ(block.flags[0], kMaterialFlagBaseColor | kMaterialFlagNormal | (2U << kMaterialAlphaModeShift))
        << "bits 0-4 are the slot mask the caller stated, bits 8-9 the alpha mode's ordinal (Blend = 2)";
    EXPECT_EQ(block.flags[1], 0U) << "the reserved flag words are written as zero";
    EXPECT_EQ(block.flags[2], 0U);
    EXPECT_EQ(block.flags[3], 0U);
}

TEST(MaterialBlockTest, TheValuesArePackedVerbatim)
{
    // No clamping and no sanitising: a metallic of 2 or a negative cutoff is the host's data, and the shader clamps
    // what it draws. A packing that "helpfully" cleaned these would make the shader's clamp untestable and the host's
    // value unreadable from the block.
    MaterialParameters values;
    values.metallic     = 2.0F;
    values.roughness    = -0.5F;
    values.alpha_cutoff = -1.0F;

    VineMaterialBlock block;
    packMaterialBlock(values, 0U, block);

    EXPECT_FLOAT_EQ(block.params[0], 2.0F);
    EXPECT_FLOAT_EQ(block.params[1], -0.5F);
    EXPECT_FLOAT_EQ(block.params[3], -1.0F);
}

TEST(MaterialBlockTest, TheDefaultMaterialPacksToTheBlockTheAbiDeclares)
{
    // Content without a material must look like content with the default one: the default parameters pack to exactly
    // the block's declared member defaults, so a backend need not special-case "no material" beyond not packing.
    VineMaterialBlock block;
    packMaterialBlock(MaterialParameters{}, 0U, block);

    const VineMaterialBlock declared{};
    EXPECT_EQ(block.base_color, declared.base_color);
    EXPECT_EQ(block.emissive, declared.emissive);
    EXPECT_EQ(block.params, declared.params);
    EXPECT_EQ(block.flags, declared.flags);
    EXPECT_EQ(block.flags[0], 0U) << "a default material carries no map, and AlphaMode::Opaque is 0";
}

TEST(MaterialBlockTest, TheSlotMaskIsTheCallersStatement)
{
    // The bits say which slots the caller will BIND a map at. A backend that could not upload one leaves its bit
    // clear, so the shader takes that slot's constant fallback instead of sampling a stand-in.
    MaterialParameters values;
    values.alpha_mode = AlphaMode::Opaque;

    VineMaterialBlock with_mask;
    packMaterialBlock(values, kMaterialFlagEmissive | 0x1FU, with_mask);
    EXPECT_EQ(with_mask.flags[0], kMaterialFlagEmissive | 0x1FU) << "the mask is passed through, not re-derived";

    VineMaterialBlock without_mask;
    packMaterialBlock(values, 0U, without_mask);
    EXPECT_EQ(without_mask.flags[0], 0U);
}
