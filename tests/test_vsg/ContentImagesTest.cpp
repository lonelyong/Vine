/**
 * @brief The by-name image policy: which image a declared sampler binding gets (see
 * `.ai/design/vsg-reimplementation.md` §11.16aj, `api/ContentImages.hpp`).
 *
 * Device-free by construction: the images are `vsg::ImageView` / `vsg::Sampler` create-infos and the plan is
 * values, so what is checked here is the POLICY - the names the engine's ABI reserves, and that the map is
 * picked by the identity the plan resolved rather than by position. The pixel half (a program whose
 * `shadow_map` reads the depth its pass declared, and the white stand-in when the pass declared none) lives
 * in ContentPassTest.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>

#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>

#include <vine/vsg/api/ContentImages.hpp>
#include <vine/vsg/api/ProgramAbi.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>

using vine::vsg::AbiBinding;
using vine::vsg::AbiDescriptorKind;
using vine::vsg::ImageOrigin;
using vine::vsg::imageOriginOf;
using vine::vsg::InputImages;
using vine::vsg::ProgramAbi;
using vine::vsg::SamplerImage;
using vine::vsg::samplesShadowMap;
using vine::vsg::shadowImageOf;

namespace
{

/// @brief A declared sampler binding, as the scan records it (name, set, binding).
AbiBinding sampler(const char* name, std::uint32_t set, std::uint32_t binding)
{
    AbiBinding declared;
    declared.set     = set;
    declared.binding = binding;
    declared.kind    = AbiDescriptorKind::Sampler2D;
    declared.name    = name;
    return declared;
}

}  // namespace

TEST(ContentImagesTest, TheNameStatesWhereTheImageComesFrom)
{
    // The three names the engine's ABI reserves, and the catch-all the engine's screen programs live in.
    EXPECT_EQ(imageOriginOf("diffuseMap"), ImageOrigin::Material);
    EXPECT_EQ(imageOriginOf("skyMap"), ImageOrigin::Environment);
    EXPECT_EQ(imageOriginOf("shadow_map"), ImageOrigin::Shadow);
    for (const char* name : { "albedo_tex", "normal_tex", "spec_tex", "pos_tex", "screen_tex", "map_tex", "" })
    {
        EXPECT_EQ(imageOriginOf(name), ImageOrigin::Input) << "the name '" << name << "' is an input texture";
    }
    // A name is not a pattern: a longer or differently spelled one is a different declaration.
    EXPECT_EQ(imageOriginOf("diffuseMap2"), ImageOrigin::Input);
    EXPECT_EQ(imageOriginOf("shadowMap"), ImageOrigin::Input);
    EXPECT_EQ(imageOriginOf("SkyMap"), ImageOrigin::Input);
}

TEST(ContentImagesTest, AProgramSamplesTheShadowItAsksForByNameWhateverItsBinding)
{
    ProgramAbi material_only;
    material_only.bindings.push_back(sampler("diffuseMap", 0U, 1U));
    EXPECT_FALSE(samplesShadowMap(material_only));
    EXPECT_FALSE(samplesShadowMap(ProgramAbi{}));

    // Where the text puts it does not matter: the name is what says the program can read the map.
    ProgramAbi engine_shaped;
    engine_shaped.bindings.push_back(sampler("shadow_map", 0U, 3U));
    engine_shaped.bindings.push_back(sampler("diffuseMap", 0U, 1U));
    EXPECT_TRUE(samplesShadowMap(engine_shaped));

    ProgramAbi elsewhere;
    elsewhere.bindings.push_back(sampler("shadow_map", 3U, 7U));
    EXPECT_TRUE(samplesShadowMap(elsewhere));
}

TEST(ContentImagesTest, TheMapIsTheInputThePlanResolvedAndNotTheFirstDepthItFinds)
{
    // Two inputs: a colour source that ALSO offers a sampleable depth (the trap - a G-buffer has a depth
    // too), and the map itself. The plan says which one is the map by the LIGHT the target states, so the
    // picker must return the map's image whichever order they sit in and whatever is sampleable.
    const ::vsg::ref_ptr<::vsg::ImageView> source_depth = ::vsg::ImageView::create();
    const ::vsg::ref_ptr<::vsg::ImageView> map_depth    = ::vsg::ImageView::create();
    const ::vsg::ref_ptr<::vsg::Sampler>   depth_sampler = ::vsg::Sampler::create();
    ASSERT_NE(source_depth, nullptr);
    ASSERT_NE(map_depth, nullptr);
    ASSERT_NE(source_depth, map_depth) << "the two views are different images";

    int sun = 0;   // the light's IDENTITY is all the facts carry
    int other_sun = 0;

    vine::vsg::core::CompiledInput inputs[2];
    inputs[0].color_attachments = 1U;
    inputs[0].depth_sampleable  = true;   // the source's depth is sampleable too (see the trap above)
    inputs[1].depth_sampleable  = true;
    inputs[1].shadow.light      = &sun;
    inputs[1].shadow.has_view_projection = true;

    vine::vsg::core::CompiledPass pass;
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(inputs, 2U);
    pass.shadow.light                 = &sun;
    pass.shadow.has_view_projection   = true;

    const ::vsg::ref_ptr<::vsg::ImageView> offered_colors[]{ ::vsg::ImageView::create() };
    const InputImages offered[]{ InputImages{ offered_colors, source_depth }, InputImages{ {}, map_depth } };
    const std::span<const InputImages> offer(offered, 2U);

    SamplerImage image;
    ASSERT_TRUE(shadowImageOf(pass, offer, depth_sampler, image));
    EXPECT_EQ(image.view, map_depth) << "the map is the input whose target states the light, not input 0";
    EXPECT_EQ(image.sampler, depth_sampler) << "a depth is read with the layer's depth sampler (NEAREST)";

    // ... and the same offer with the map FIRST must give the same answer (position is not what decides).
    const InputImages swapped[]{ InputImages{ {}, map_depth }, InputImages{ offered_colors, source_depth } };
    vine::vsg::core::CompiledInput swapped_inputs[2];
    swapped_inputs[0]                    = inputs[1];
    swapped_inputs[1]                    = inputs[0];
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(swapped_inputs, 2U);
    SamplerImage from_swapped;
    ASSERT_TRUE(shadowImageOf(pass, std::span<const InputImages>(swapped, 2U), depth_sampler, from_swapped));
    EXPECT_EQ(from_swapped.view, map_depth);

    // A pass that resolved no map: the caller binds the white stand-in instead (the engine never reads the
    // map while the shadow block's switch is off, so the stand-in's value is the multiply's identity).
    vine::vsg::core::CompiledPass no_shadow = pass;
    no_shadow.shadow = vine::vsg::core::ShadowFacts{};
    SamplerImage none;
    EXPECT_FALSE(shadowImageOf(no_shadow, offer, depth_sampler, none));
    EXPECT_EQ(none.view, nullptr);

    // ... and the three ways an input can fail to BE the map: it states no light, it states another one, or
    // nobody published how to read it.
    vine::vsg::core::CompiledInput no_light[2];
    no_light[1].depth_sampleable = true;
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(no_light, 2U);
    pass.shadow.light = &sun;
    EXPECT_FALSE(shadowImageOf(pass, offer, depth_sampler, none));

    vine::vsg::core::CompiledInput another_light[2];
    another_light[1].depth_sampleable   = true;
    another_light[1].shadow.light       = &other_sun;
    another_light[1].shadow.has_view_projection = true;
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(another_light, 2U);
    EXPECT_FALSE(shadowImageOf(pass, offer, depth_sampler, none))
        << "a map that belongs to another light is not the map this pass resolved";

    vine::vsg::core::CompiledInput unreadable[2];
    unreadable[1].depth_sampleable      = true;
    unreadable[1].shadow.light          = &sun;   // states the light but no matrix: not a readable map
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(unreadable, 2U);
    EXPECT_FALSE(shadowImageOf(pass, offer, depth_sampler, none));

    // ... and the caller's own hole: the map's input offers no depth view this frame.
    const InputImages missing[]{ InputImages{ offered_colors, source_depth }, InputImages{ {}, {} } };
    pass.inputs = std::span<const vine::vsg::core::CompiledInput>(inputs, 2U);
    EXPECT_FALSE(shadowImageOf(pass, std::span<const InputImages>(missing, 2U), depth_sampler, none))
        << "the image is the caller's to offer: a hole is not a guess";
}
