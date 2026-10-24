/**
 * @brief MaterialInstance: the sparse overlay over an asset (graphics-material.md 2.2/2.3, F2/F10, T1).
 *
 * What these cases pin is the part a host cannot see from the outside: that an instance's values are the
 * parent's EXCEPT the items it states, that its revision is a composite of the two (so an asset edit reaches
 * every instance without anybody walking the instances), and that the transparency classification reads the
 * resolved mode rather than the kind of object it was handed.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Material.hpp>
#include <vine/graphics/MaterialInstance.hpp>
#include <vine/graphics/RenderCommand.hpp>

using namespace vn::graphics;
using vn::intrusive_ptr;

namespace
{

/// @brief A material with every value stated, so "inherited" and "overridden" are distinguishable.
intrusive_ptr<Material> makeStatedMaterial()
{
    auto material = vn::make_intrusive<Material>();
    material->setBaseColor(vn::Colorf(0.25f, 0.5f, 0.75f, 1.0f));
    material->setEmissive(vn::Colorf(0.1f, 0.2f, 0.3f, 0.5f));
    material->setMetallic(0.75f);
    material->setRoughness(0.125f);
    material->setOcclusion(0.5f);
    material->setAlphaMode(AlphaMode::Opaque);
    material->setAlphaCutoff(0.25f);
    return material;
}

}  // namespace

TEST(MaterialInstanceTest, AnInstanceInheritsWhatItDoesNotState)
{
    // T1: the parent's hue with the instance's roughness - one object, both facts, and no copy of the asset.
    const intrusive_ptr<Material> asset = makeStatedMaterial();
    auto instance = vn::make_intrusive<MaterialInstance>(asset);
    instance->setRoughness(0.9f);

    EXPECT_EQ(instance->roughness(), 0.9f) << "the stated item is the instance's";
    EXPECT_EQ(instance->baseColor(), asset->baseColor()) << "everything else follows the asset";
    EXPECT_EQ(instance->emissive(), asset->emissive());
    EXPECT_EQ(instance->metallic(), asset->metallic());
    EXPECT_EQ(instance->occlusion(), asset->occlusion());
    EXPECT_EQ(instance->alphaMode(), asset->alphaMode());
    EXPECT_EQ(instance->alphaCutoff(), asset->alphaCutoff());
    EXPECT_EQ(instance->overrideMask(), materialItemBit(MaterialItem::Roughness))
        << "exactly one item is stated";
}

TEST(MaterialInstanceTest, EditingTheAssetReachesEveryInstanceThatDoesNotOverrideTheItem)
{
    // R1/P3: the resolution cache is keyed on (parent revision, own revision), so an asset edit invalidates
    // every instance's cache WITHOUT the asset knowing which instances exist.
    const intrusive_ptr<Material> asset = makeStatedMaterial();
    auto stated   = vn::make_intrusive<MaterialInstance>(asset);
    auto follower = vn::make_intrusive<MaterialInstance>(asset);
    stated->setBaseColor(vn::Colorf(1.0f, 0.0f, 0.0f, 1.0f));

    const std::uint64_t follower_before = follower->revision();
    const std::uint64_t stated_before   = stated->revision();
    EXPECT_EQ(follower->baseColor(), asset->baseColor());

    asset->setBaseColor(vn::Colorf(0.0f, 1.0f, 0.0f, 1.0f));

    EXPECT_EQ(follower->baseColor(), asset->baseColor()) << "the unoverridden item follows the asset";
    EXPECT_EQ(stated->baseColor(), vn::Colorf(1.0f, 0.0f, 0.0f, 1.0f)) << "the overridden item does not";
    EXPECT_NE(follower->revision(), follower_before) << "the composite revision moved with the asset";
    EXPECT_NE(stated->revision(), stated_before) << "and it moves for an instance whose item is overridden too: "
                                                   "the resolved bytes are cached under one number";
}

TEST(MaterialInstanceTest, ClearingAnOverrideInheritsAgain)
{
    const intrusive_ptr<Material> asset = makeStatedMaterial();
    auto instance = vn::make_intrusive<MaterialInstance>(asset);
    instance->setMetallic(1.0f);
    ASSERT_EQ(instance->metallic(), 1.0f);

    const std::uint64_t before = instance->revision();
    instance->clearOverride(MaterialItem::Metallic);

    EXPECT_EQ(instance->metallic(), asset->metallic()) << "cleared: the asset's value applies";
    EXPECT_EQ(instance->overrideMask(), 0U);
    EXPECT_NE(instance->revision(), before) << "clearing a stated item is an edit of the instance";

    // Clearing what is not overridden is not an edit (the same rule the setters follow).
    const std::uint64_t quiet = instance->revision();
    instance->clearOverride(MaterialItem::Metallic);
    EXPECT_EQ(instance->revision(), quiet);
}

TEST(MaterialInstanceTest, ATextureSlotIsOneItem)
{
    // A slot is (map, read state): stating one must take BOTH from the instance, or a swapped map would be
    // filtered with the asset's sampler.
    const intrusive_ptr<Material> asset = makeStatedMaterial();
    auto asset_map = intrusive_ptr<Texture>(new Texture2D(4, 4, vn::imaging::PixelFormat::Rgba8Unorm));
    vn::graphics::Sampler asset_sampler;
    asset_sampler.address_u = vn::graphics::Sampler::AddressMode::Repeat;
    asset->setTexture(TextureSlot::Normal, asset_map, asset_sampler);

    auto instance = vn::make_intrusive<MaterialInstance>(asset);
    EXPECT_EQ(instance->texture(TextureSlot::Normal), asset_map.get()) << "an absent override inherits";

    auto own_map = intrusive_ptr<Texture>(new Texture2D(4, 4, vn::imaging::PixelFormat::Rgba8Unorm));
    vn::graphics::Sampler own_sampler;
    own_sampler.address_u = vn::graphics::Sampler::AddressMode::ClampToEdge;
    instance->setTexture(TextureSlot::Normal, own_map, own_sampler);

    ASSERT_EQ(instance->overrideMask(), materialItemBit(MaterialItem::NormalTexture));
    EXPECT_EQ(instance->texture(TextureSlot::Normal), own_map.get()) << "the map AND the state are the "
                                                                              "instance's";
    EXPECT_EQ(instance->sampler(TextureSlot::Normal).address_u, vn::graphics::Sampler::AddressMode::ClampToEdge);

    // A NULL texture still STATES the slot: that is how an instance removes a map its asset carries.
    instance->setTexture(TextureSlot::Normal, nullptr);
    EXPECT_FALSE(instance->hasTexture(TextureSlot::Normal));
    EXPECT_EQ(instance->overrideMask(), materialItemBit(MaterialItem::NormalTexture));

    instance->clearOverride(MaterialItem::NormalTexture);
    EXPECT_TRUE(instance->hasTexture(TextureSlot::Normal)) << "cleared: the asset's whole slot is back";
    EXPECT_EQ(instance->texture(TextureSlot::Normal), asset_map.get());
}

TEST(MaterialInstanceTest, TheTransparencySortReadsTheResolvedMode)
{
    // 2.7 / C4: Blend is transparent whatever the opacity, Mask is opaque to the sort whatever the opacity, and
    // everything else is transparent exactly when the drawable's opacity says so. An instance must classify by
    // the mode it RESOLVES, not by having no asset of its own.
    const intrusive_ptr<Material> asset = makeStatedMaterial();
    auto instance = vn::make_intrusive<MaterialInstance>(asset);

    RenderCommand command(vn::intrusive_ptr<Geometry>(), MaterialBasePtr(instance), vn::math::Mat4d{});
    command.opacity = 1.0f;
    EXPECT_FALSE(command.isTransparent()) << "an opaque instance at full opacity is opaque";

    instance->setAlphaMode(AlphaMode::Blend);
    EXPECT_TRUE(command.isTransparent()) << "Blend is transparent by its mode alone";

    instance->setAlphaMode(AlphaMode::Mask);
    command.opacity = 0.25f;
    EXPECT_FALSE(command.isTransparent()) << "Mask never enters the transparent queue: its alpha cuts, so a "
                                             "masked leaf still occludes itself";

    instance->clearOverride(MaterialItem::AlphaMode);
    EXPECT_TRUE(command.isTransparent()) << "the asset says Opaque, so the opacity decides again";

    command.material = nullptr;
    EXPECT_TRUE(command.isTransparent()) << "the engine's default material is opaque: opacity decides";
}

TEST(MaterialInstanceTest, ASwapOfParentIsAnEdit)
{
    const intrusive_ptr<Material> first  = makeStatedMaterial();
    const intrusive_ptr<Material> second = makeStatedMaterial();
    second->setRoughness(0.875f);

    auto instance = vn::make_intrusive<MaterialInstance>(first);
    ASSERT_EQ(instance->roughness(), first->roughness());
    const std::uint64_t before = instance->revision();

    instance->setParent(second);
    EXPECT_EQ(instance->roughness(), 0.875f) << "the unoverridden items follow the new asset";
    EXPECT_NE(instance->revision(), before) << "pointing at another asset changes the resolved values";
}
