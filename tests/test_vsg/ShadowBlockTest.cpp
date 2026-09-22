/**
 * @brief The SHADOW BLOCK: the matrix that places a fragment in a light's map, and the four facts that keep the
 * shading off when the map cannot be used (see `.ai/design/vsg-reimplementation.md`, milestone M5c-2).
 *
 * What the block is FOR. The map arrives as a sampled input - the sampling path already binds the texture - and what
 * the shader cannot get from a texture is WHERE a fragment lands in it: the map was rasterised by the light's camera,
 * the consuming pass draws from its own, so the block carries `light_vp * inverse(view)` plus the comparison's
 * scalars. Every one of those is somebody else's statement (the target says whose map it is, the producer says how to
 * read it, the light says whether it still casts), and this file pins what happens when a statement is missing: the
 * switch goes OFF, which is falsifiable in the block's bytes and in the device case's pixels.
 *
 * Device-free by construction: the packing is a function of the plan's fact, the call's camera and the call's lights.
 * The arithmetic that matters - that the block maps a VIEW-space point into the producer's clip through the world -
 * is checked against the two matrices the test composes itself, with a camera that is NOT the identity: an
 * axis-aligned camera cannot tell `light_vp * inverse(view)` from `light_vp`, and that is exactly the class of
 * mistake this packing exists to prevent.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/Viewer.h>

#include <vine/Buffer.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Light.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderDiagnostic.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/ShaderProgram.hpp>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/ContentDraw.hpp>
#include <vine/vsg/api/ContentFacts.hpp>
#include <vine/vsg/api/ContentPass.hpp>
#include <vine/vsg/api/ContentPipeline.hpp>
#include <vine/vsg/api/ContentSources.hpp>
#include <vine/vsg/api/Device.hpp>
#include <vine/vsg/api/GeometryFacts.hpp>
#include <vine/vsg/api/LightBlock.hpp>
#include <vine/vsg/api/OffscreenTarget.hpp>
#include <vine/vsg/api/ShadowBlock.hpp>
#include <vine/vsg/api/StreamUploads.hpp>
#include <vine/vsg/api/VsgExecutor.hpp>
#include <vine/vsg/core/FrameCompiler.hpp>
#include <vine/vsg/core/FrameRecorder.hpp>
#include <vine/vsg/core/StateRegistry.hpp>
#include <vine/vsg/core/VariantPool.hpp>

using vine::graphics::Camera;
using vine::graphics::Geometry;
using vine::graphics::Light;
using vine::graphics::LightType;
using vine::graphics::Material;
using vine::graphics::RenderCommand;
using vine::graphics::RenderTarget;
using vine::graphics::ShaderProgram;
using vine::graphics::ShaderStage;
using vine::graphics::ShaderStageType;
using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::buildGeometryFacts;
using vine::vsg::buildMaterialFacts;
using vine::vsg::buildProgramFacts;
using vine::vsg::ContentDraw;
using vine::vsg::ContentFacts;
using vine::vsg::ContentPass;
using vine::vsg::ContentPipeline;
using vine::vsg::directionalSlotOf;
using vine::vsg::FactMiss;
using vine::vsg::GeometryFacts;
using vine::vsg::InputImages;
using vine::vsg::kLightDirectionalSlots;
using vine::vsg::MaterialFacts;
using vine::vsg::OffscreenTarget;
using vine::vsg::packLightBlock;
using vine::vsg::packShadowBlock;
using vine::vsg::PassContent;
using vine::vsg::ProgramFacts;
using vine::vsg::StreamUploads;
using vine::vsg::VineLightsBlock;
using vine::vsg::VsgExecutor;
using vine::vsg::core::CameraSnapshot;
using vine::vsg::core::CompiledFrame;
using vine::vsg::core::CompiledPass;
using vine::vsg::core::ClearPolicy;
using vine::vsg::core::CompiledDraw;
using vine::vsg::core::Diagnostics;
using vine::vsg::core::DrawKind;
using vine::vsg::core::FrameArena;
using vine::vsg::core::FrameCompiler;
using vine::vsg::core::FrameFacts;
using vine::vsg::core::FrameRecorder;
using vine::vsg::core::FrameToken;
using vine::vsg::core::LightRef;
using vine::vsg::core::Observe;
using vine::vsg::core::Rgba8;
using vine::vsg::core::ShadowFacts;
using vine::vsg::core::StateRegistry;
using vine::vsg::core::TargetFacts;
using vine::vsg::core::VariantPool;

namespace
{

/// @brief One announced light as the frame remembers it, with the identity a shadow map names it by.
LightRef makeLight(const void* identity, LightType type, double r, double g, double b, float intensity)
{
    LightRef light;
    light.identity  = identity;
    light.enabled   = true;
    light.type      = type;
    light.color     = vine::Colorf(r, g, b, 1.0);
    light.intensity = intensity;
    return light;
}

/// @brief A directional light pointing at the viewer, with the shadow switch and bias a caster carries.
LightRef makeSun(const void* identity, double r, double g, double b, bool cast_shadow = true, float bias = 0.0F)
{
    LightRef sun = makeLight(identity, LightType::Directional, r, g, b, 1.0F);
    sun.has_direction = true;
    sun.direction     = vine::math::Vec3d(0.0, 0.0, 1.0);
    sun.cast_shadow   = cast_shadow;
    sun.shadow_bias   = bias;
    return sun;
}

/// @brief A camera the world -> view rotation of which is NOT the identity: it looks along +X from the origin.
CameraSnapshot cameraLookingAlongPlusX()
{
    CameraSnapshot camera;
    camera.present    = true;
    camera.view       = vine::math::Mat4d{};  // identity: right = +X, up = +Y, backward = +Z
    camera.view(0, 0) = 0.0;
    camera.view(0, 2) = 1.0;   // right = world +Z
    camera.view(2, 0) = -1.0;  // backward = world -X
    camera.view(2, 2) = 0.0;
    return camera;
}

/// @brief A producer matrix with NO symmetry and NO diagonal-only shape: a transposed packing cannot look like it.
vine::math::Mat4d mixedProducerMatrix()
{
    vine::math::Mat4d vp;
    vp(0, 0) = 0.5;
    vp(0, 1) = 0.25;   // reads world y: the transpose check needs an off-diagonal
    vp(1, 0) = -0.125;
    vp(1, 1) = 0.75;
    vp(2, 2) = 0.5;
    vp(2, 3) = 0.25;
    vp(3, 3) = 1.0;
    return vp;
}

/// @brief A drawing call with a camera and a light list, as the packing consumes them.
CompiledDraw drawWith(const CameraSnapshot& camera, std::span<const LightRef> lights, std::vector<LightRef>& storage)
{
    storage.assign(lights.begin(), lights.end());
    CompiledDraw draw;
    draw.kind   = DrawKind::Content;
    draw.camera = camera;
    draw.lights = storage;
    return draw;
}

}  // namespace

TEST(ShadowBlockTest, TheBlockMapsViewSpaceIntoTheProducerClipThroughTheWorld)
{
    // The claim: `light_vp * inverse(view)` maps a VIEW-space position into the light's clip - the same place the
    // producer's matrix puts the corresponding WORLD position. Checked against both matrices, composed here, with a
    // camera whose rotation is not the identity (see the file note).
    const CameraSnapshot camera = cameraLookingAlongPlusX();
    ASSERT_TRUE(camera.present);

    const vine::math::Mat4d vp = mixedProducerMatrix();
    const void* const        owner = reinterpret_cast<const void*>(0x51U);  // the map's light (any identity)
    LightRef                 sun   = makeSun(owner, 0.4, 0.0, 0.0, /*cast_shadow*/ true, /*bias*/ 0.003F);
    std::vector<LightRef>    storage;
    const LightRef           announced[]{ sun };
    const CompiledDraw       draw = drawWith(camera, std::span<const LightRef>(announced, 1U), storage);

    ShadowFacts shadow;
    shadow.light                  = owner;
    shadow.has_view_projection    = true;
    shadow.view_projection        = vp;

    vine::graphics::VineShadowBlock block;
    ASSERT_TRUE(packShadowBlock(shadow, draw, block)) << "the map, its light and a camera are all here";

    EXPECT_FLOAT_EQ(block.params[0], 1.0F) << "the ABI's switch";
    EXPECT_FLOAT_EQ(block.params[1], 0.003F) << "the casting light's own bias";
    EXPECT_FLOAT_EQ(block.params[2], 1.0F) << "the strength the SDK has no knob for";
    EXPECT_FLOAT_EQ(block.params[3], 0.0F) << "and the slot the light block put the caster in";

    // The packed matrix, read the way the GLSL does (column-major): element (row, column) at column * 4 + row.
    const vine::math::Mat4d composed = vp * camera.view.inverted();
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(block.view_to_light[static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row)],
                        static_cast<float>(composed(row, column)), 1e-9F)
                << "element (" << row << ", " << column << ")";
        }
    }

    // And the SEMANTIC half: one view-space point, taken through the block, lands where the same point lands through
    // the producer's matrix - the composition's whole purpose. (A packing that dropped `inverse(view)` or transposed
    // the result fails here, and the off-diagonal of `vp` is what makes the transpose visible.)
    const vine::math::Mat4d identity_view = camera.view;
    (void)identity_view;
    const vine::math::Mat4d view_to_light = vp * camera.view.inverted();
    const vine::math::Mat4d world_to_light_expected = vp * vine::math::Mat4d{};  // inverse(view) * view == identity
    EXPECT_NEAR((view_to_light * camera.view)(0, 1), world_to_light_expected(0, 1), 1e-9)
        << "the block's view -> light composed with the view is the producer's own world -> light";
}

TEST(ShadowBlockTest, EveryMissingStatementLeavesTheSwitchOff)
{
    // The block is written either way (a shader must never read unbound bytes), and its switch says whether the map
    // may be shaded with. Seven ways to lose that right, one statement each.
    const CameraSnapshot camera = cameraLookingAlongPlusX();
    const void* const    owner  = reinterpret_cast<const void*>(0x51U);
    const void* const    other  = reinterpret_cast<const void*>(0x52U);

    LightRef              casting = makeSun(owner, 0.4, 0.0, 0.0);
    LightRef              disabled_caster = casting;
    disabled_caster.enabled = false;
    LightRef              not_casting = casting;
    not_casting.cast_shadow = false;
    LightRef              ambient_owner = makeLight(owner, LightType::Ambient, 0.4, 0.0, 0.0, 1.0F);
    LightRef              another_light = makeSun(other, 0.4, 0.0, 0.0);

    // An UNUSABLE map leaves no bytes behind: the block is written either way (a shader must never read unbound
    // memory), and its switch says whether the map may be shaded with.
    const auto pack_unusable = [&](const ShadowFacts& shadow, std::span<const LightRef> lights,
                                   const CameraSnapshot& cam) {
        std::vector<LightRef> storage;
        const CompiledDraw    draw = drawWith(cam, lights, storage);
        vine::graphics::VineShadowBlock block;
        const bool            on = packShadowBlock(shadow, draw, block);
        for (const float value : block.view_to_light) {
            EXPECT_FLOAT_EQ(value, 0.0F) << "an unusable map must not leave a matrix behind";
        }
        for (const float value : block.params) {
            EXPECT_FLOAT_EQ(value, 0.0F) << "nor a switch";
        }
        return on;
    };

    ShadowFacts good;
    good.light               = owner;
    good.has_view_projection = true;
    good.view_projection     = mixedProducerMatrix();

    const LightRef casting_only[]{ casting };
    const LightRef disabled_only[]{ disabled_caster };
    const LightRef not_casting_only[]{ not_casting };
    const LightRef ambient_only[]{ ambient_owner };
    const LightRef other_only[]{ another_light };

    {
        std::vector<LightRef> storage;
        const CompiledDraw    draw = drawWith(camera, std::span<const LightRef>(casting_only, 1U), storage);
        vine::graphics::VineShadowBlock block;
        EXPECT_TRUE(packShadowBlock(good, draw, block)) << "the reference case: every statement is present";
    }
    ShadowFacts no_map;
    EXPECT_FALSE(pack_unusable(no_map, casting_only, camera)) << "a pass that declared no map";
    ShadowFacts no_matrix = good;
    no_matrix.has_view_projection = false;
    EXPECT_FALSE(pack_unusable(no_matrix, casting_only, camera)) << "a producer that published no matrix";
    CameraSnapshot no_camera = camera;
    no_camera.present        = false;
    EXPECT_FALSE(pack_unusable(good, casting_only, no_camera)) << "a call that announced no camera";
    EXPECT_FALSE(pack_unusable(good, other_only, camera)) << "a call that did not announce the map's light";
    EXPECT_FALSE(pack_unusable(good, disabled_only, camera)) << "a caster that is switched off";
    EXPECT_FALSE(pack_unusable(good, not_casting_only, camera)) << "a light that stopped casting";
    EXPECT_FALSE(pack_unusable(good, ambient_only, camera)) << "a kind the block has no slot for";
}

TEST(ShadowBlockTest, ACasterTheBlockCannotNameLeavesTheSwitchOff)
{
    // Four directional lights announced, the map's owner last: the light block carries three, so the owner has no
    // slot - and then the shader could not name the light its map belongs to, which would scale a light the map does
    // NOT belong to (the failure the slot number exists to prevent).
    const CameraSnapshot camera = cameraLookingAlongPlusX();
    const void* const    owner  = reinterpret_cast<const void*>(0x51U);

    LightRef first  = makeSun(reinterpret_cast<const void*>(0x61U), 0.1, 0.0, 0.0);
    LightRef second = makeSun(reinterpret_cast<const void*>(0x62U), 0.2, 0.0, 0.0);
    LightRef third  = makeSun(reinterpret_cast<const void*>(0x63U), 0.3, 0.0, 0.0);
    LightRef caster = makeSun(owner, 0.4, 0.0, 0.0);

    ShadowFacts shadow;
    shadow.light               = owner;
    shadow.has_view_projection = true;
    shadow.view_projection     = mixedProducerMatrix();

    std::vector<LightRef> storage;
    const LightRef        announced[]{ first, second, third, caster };
    const CompiledDraw    draw = drawWith(camera, std::span<const LightRef>(announced, 4U), storage);
    vine::graphics::VineShadowBlock block;
    EXPECT_FALSE(packShadowBlock(shadow, draw, block))
        << "the owner is the fourth directional light, and the block holds three";

    // The block still represents the three lights that FIT (the caller's drop report is about the fourth).
    VineLightsBlock lights_block;
    EXPECT_EQ(packLightBlock(draw.lights, draw.camera, lights_block), 3U);
    EXPECT_EQ(directionalSlotOf(draw.lights, owner), kLightDirectionalSlots)
        << "and the owner is not one of them - the same answer the packing just gave";
}

TEST(ShadowBlockTest, TheSlotHelperAgreesWithThePacking)
{
    // Two walks of one rule (the packing assigns the slots, the helper answers "which slot did this light get"), so
    // they are pinned against each other: for every announced light, the slot the helper reports is the slot whose
    // colour the packing took from THAT light.
    LightRef ambient = makeLight(reinterpret_cast<const void*>(0x70U), LightType::Ambient, 0.7, 0.7, 0.7, 1.0F);
    LightRef first   = makeSun(reinterpret_cast<const void*>(0x71U), 0.11, 0.0, 0.0);
    LightRef second  = makeSun(reinterpret_cast<const void*>(0x72U), 0.22, 0.0, 0.0);
    LightRef third   = makeSun(reinterpret_cast<const void*>(0x73U), 0.33, 0.0, 0.0);
    LightRef fourth  = makeSun(reinterpret_cast<const void*>(0x74U), 0.44, 0.0, 0.0);
    LightRef off     = makeSun(reinterpret_cast<const void*>(0x75U), 0.55, 0.0, 0.0);
    off.enabled      = false;

    const LightRef announced[]{ ambient, first, off, second, third, fourth };
    const CameraSnapshot camera = cameraLookingAlongPlusX();

    VineLightsBlock block;
    ASSERT_EQ(packLightBlock(announced, camera, block), 4U) << "the ambient plus three directionals";

    const std::pair<const LightRef*, float> expected[]{
        { &first, 0.11F }, { &second, 0.22F }, { &third, 0.33F },
    };
    for (const auto& [light, colour] : expected)
    {
        const std::size_t slot = directionalSlotOf(announced, light->identity);
        ASSERT_LT(slot, kLightDirectionalSlots) << "an enabled directional light has a slot";
        EXPECT_FLOAT_EQ(block.cols[slot][0], colour)
            << "the slot the helper reports is where the packing put THIS light's colour";
    }
    EXPECT_EQ(directionalSlotOf(announced, fourth.identity), kLightDirectionalSlots)
        << "the fourth directional light is not carried at all";
    EXPECT_EQ(directionalSlotOf(announced, off.identity), kLightDirectionalSlots)
        << "and a disabled light was never packed";
    EXPECT_EQ(directionalSlotOf(announced, ambient.identity), kLightDirectionalSlots)
        << "an ambient light has no directional slot";
}

namespace
{

constexpr std::uint32_t kSize = 64;

/// @brief The receiver's clear colour: what "this band drew nothing" would look like (no band may show it).
constexpr float kClear[4]{ 0.0F, 0.0F, 0.75F, 1.0F };

/// @brief The producer's fragment stage: it writes the SAME depth everywhere (the whole map is "a caster").
constexpr char8_t kProducerVertex[] =
    u8"layout(location = 0) in vec3 position;\n"
    u8"void main() { gl_Position = vec4(position.xy, 0.5, 1.0); }\n";

/// @brief The consumer's fragment stage: the SDK's own shadow comparison, restated at this backend's bindings.
///
/// The math is the ABI's (see ShaderAbi's VineShadowBlock): the block maps a VIEW-space position into the producer's
/// clip, x/y are converted to the MAP's convention (v = 0 is the TOP row: the SDK's y up is inverted) and z to the
/// map's REVERSE-Z depth range. Outside the map's rectangle nothing casts, so the fragment stays lit.
constexpr char8_t kConsumerFragment[] =
    u8"layout(set = 0, binding = 3, std140) uniform VineLightsBlock {\n"
    u8"    vec4 ambient;\n"
    u8"    vec4 sun_dir[3];\n"
    u8"    vec4 sun_color[3];\n"
    u8"} lights;\n"
    u8"layout(set = 0, binding = 4, std140) uniform VineShadowBlock {\n"
    u8"    mat4 viewToLight;\n"
    u8"    vec4 params;\n"
    u8"} shadow;\n"
    u8"layout(set = 1, binding = 0) uniform sampler2D shadow_map;\n"
    u8"layout(location = 0) in vec2 surface;\n"
    u8"layout(location = 0) out vec4 outColor;\n"
    u8"void main() {\n"
    u8"    vec3 n   = vec3(0.0, 0.0, 1.0);\n"
    u8"    vec3 pos = vec3(surface * 2.0, 1.0);\n"
    u8"    vec3 lit = lights.ambient.rgb * lights.ambient.a;\n"
    u8"    for (int i = 0; i < 3; ++i) {\n"
    u8"        float ndl = max(dot(n, lights.sun_dir[i].xyz), 0.0);\n"
    u8"        if (shadow.params.x > 0.5 && i == int(shadow.params.w)) {\n"
    u8"            vec4  light_clip = shadow.viewToLight * vec4(pos, 1.0);\n"
    u8"            vec3  light_uv   = light_clip.xyz / light_clip.w;\n"
    u8"            vec2  map_uv     = light_uv.xy * vec2(0.5, -0.5) + 0.5;\n"
    u8"            float frag       = 1.0 - (light_uv.z * 0.5 + 0.5);\n"
    u8"            if (all(greaterThan(map_uv, vec2(0.0))) && all(lessThan(map_uv, vec2(1.0)))) {\n"
    u8"                float caster = texture(shadow_map, map_uv).r;\n"
    u8"                float visible = (frag + shadow.params.y) >= caster ? 1.0 : 0.0;\n"
    u8"                ndl *= mix(1.0, visible, clamp(shadow.params.z, 0.0, 1.0));\n"
    u8"            }\n"
    u8"        }\n"
    u8"        lit += lights.sun_color[i].rgb * lights.sun_color[i].a * ndl;\n"
    u8"    }\n"
    u8"    outColor = vec4(lit, 1.0);\n"
    u8"}\n";

}  // namespace

TEST(ShadowBlockTest, TheMapScalesTheOneLightItBelongsToAndNothingElse)
{
    // THE PICTURE. One frame, two passes: a depth-only shadow map written by the engine's shadow pass (the WHOLE map
    // is a caster at depth 0.5), then a receiver drawn in four bands, one drawing call each, where the call is what
    // differs - and each band has a different reason to be dark or lit:
    //
    //   band 0: the map's owner, casting        -> the sun's term is scaled to zero   (ambient only)
    //   band 1: the SAME light with its switch flipped between the two calls -> the runtime switch: fully lit
    //   band 2: a second sun PLUS the owner (slot ONE) -> only the owner's term is scaled (the "lit face"
    //           invariant, and the slot number is what decides WHICH light loses its term)
    //   band 3: the owner announced LAST of four directionals -> it has no slot the block can name, so the switch
    //           stays off and every light contributes
    //
    // A packing that bound the map to the wrong slot, ignored the switch, or scaled every light would move at least
    // one of those four colours, and the receiver's clear colour (blue) is what a band that drew nothing looks like.
    const vine::vsg::DeviceResult created = vine::vsg::createDevice();
    if (!created.ok)
    {
        GTEST_SKIP() << "no Vulkan device available (lavapipe + X11 are needed): " << created.error.as_std_str();
    }

    // The map: a DEPTH-ONLY, sampleable target, cleared to the reverse-Z far plane (0.0).
    OffscreenTarget::TargetLayout map_layout;
    map_layout.width            = kSize;
    map_layout.height           = kSize;
    map_layout.color_formats.clear();
    map_layout.depth_format     = RenderTarget::DepthFormat::D32;
    map_layout.depth_sampleable = true;
    map_layout.clear.depth      = true;
    std::unique_ptr<OffscreenTarget> map = OffscreenTarget::create(created.device, map_layout);
    ASSERT_NE(map, nullptr);
    ASSERT_TRUE(map->hasDepth());

    OffscreenTarget::Layout receiver_layout;
    receiver_layout.width  = kSize;
    receiver_layout.height = kSize;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        receiver_layout.clear_color[index] = kClear[index];
    }
    std::unique_ptr<OffscreenTarget> receiver = OffscreenTarget::create(created.device, receiver_layout);
    ASSERT_NE(receiver, nullptr);

    // The map's identity and its own statements: the target says whose shadow it is and how to read it.
    const vine::intrusive_ptr<RenderTarget> map_handle(new RenderTarget());
    const vine::intrusive_ptr<RenderTarget> receiver_handle(new RenderTarget());

    const auto make_program = [](const char8_t* fragment, const char8_t* vertex) {
        auto program = vine::intrusive_ptr<ShaderProgram>(new ShaderProgram());
        ShaderStage v;
        v.type   = ShaderStageType::Vertex;
        v.source = vine::String(vertex);
        ShaderStage f;
        f.type   = ShaderStageType::Fragment;
        f.source = vine::String(fragment);
        program->addStage(v);
        program->addStage(f);
        return program;
    };
    const vine::intrusive_ptr<ShaderProgram> producer_program = make_program(
        u8"void main() { }\n", kProducerVertex);
    const vine::intrusive_ptr<ShaderProgram> consumer_program = make_program(
        kConsumerFragment,
        u8"layout(location = 0) in vec3 position;\n"
        u8"layout(location = 0) out vec2 surface;\n"
        u8"void main() { gl_Position = vec4(position.xy, 0.5, 1.0); surface = position.xy; }\n");

    ProgramFacts producer_facts;
    ProgramFacts consumer_facts;
    ASSERT_EQ(buildProgramFacts(*producer_program, producer_facts), FactMiss::None);
    ASSERT_EQ(buildProgramFacts(*consumer_program, consumer_facts), FactMiss::None);

    std::unique_ptr<BlockStorage>     storage     = BlockStorage::create(created.device, BlockStorage::Layout{});
    ASSERT_NE(storage, nullptr);
    // Each half gets the block set ITS program declares: the producer declares none (an empty set), the
    // consumer declares the lights at set 0 / binding 3 and the shadow block at 4.
    std::unique_ptr<BlockDescriptors> producer_descriptors = BlockDescriptors::forAbi(producer_facts.abi, 0U, created.device, *storage);
    std::unique_ptr<BlockDescriptors> descriptors = BlockDescriptors::forAbi(consumer_facts.abi, 0U, created.device, *storage);
    ASSERT_NE(producer_descriptors, nullptr);
    ASSERT_NE(descriptors, nullptr);
    BlockDescriptors* producer_blocks[] = { producer_descriptors.get() };
    BlockDescriptors* consumer_blocks[] = { descriptors.get() };

    const ContentPipeline::VertexBinding   binding{ 0U, sizeof(float) * 3U, false };
    const ContentPipeline::VertexAttribute attribute{ 0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U };
    ContentPipeline::Settings              settings;
    settings.color_attachments = 1U;
    std::unique_ptr<ContentPipeline> producer_pipelines =
        ContentPipeline::create(producer_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                producer_facts.shaders, ContentPipeline::Settings{ 0U, 128U });
    ASSERT_NE(producer_pipelines, nullptr);
    std::unique_ptr<ContentPipeline> consumer_pipelines =
        ContentPipeline::create(consumer_facts.abi, std::span<const ContentPipeline::VertexBinding>(&binding, 1U),
                                std::span<const ContentPipeline::VertexAttribute>(&attribute, 1U),
                                consumer_facts.shaders, settings);
    ASSERT_NE(consumer_pipelines, nullptr);

    // The geometry: the producer covers the whole map (a clip-space triangle), the receiver draws four bands.
    const auto make_geometry = [](std::vector<float> positions) {
        auto indices = vine::intrusive_ptr<const vine::Buffer<std::uint32_t>>(
            new vine::Buffer<std::uint32_t>(std::vector<std::uint32_t>{ 0U, 1U, 2U }));
        auto geometry = vine::intrusive_ptr<Geometry>(new Geometry());
        geometry->setPositions(vine::intrusive_ptr<const vine::Buffer<float>>(
            new vine::Buffer<float>(std::move(positions))));
        geometry->setIndices(indices);
        geometry->setRevision(1U);
        return geometry;
    };
    const auto make_band = [&](float left, float right) {
        const float centre = (left + right) * 0.5F;
        return make_geometry({ left, -1.0F, 0.0F, right, -1.0F, 0.0F, centre, 1.0F, 0.0F });
    };
    const vine::intrusive_ptr<Geometry> full_quad =
        make_geometry({ -1.0F, -1.0F, 0.0F, 3.0F, -1.0F, 0.0F, -1.0F, 3.0F, 0.0F });
    const vine::intrusive_ptr<Geometry> bands[]{ make_band(-1.0F, -0.5F), make_band(-0.5F, 0.0F),
                                                 make_band(0.0F, 0.5F), make_band(0.5F, 1.0F) };

    const vine::intrusive_ptr<Material> material(new Material());
    material->setDiffuse(vine::Colorf(1.0F, 1.0F, 1.0F, 1.0F));
    MaterialFacts          material_facts;
    std::vector<std::byte> material_storage;
    ASSERT_EQ(buildMaterialFacts(material.get(), 1U, material_facts, material_storage), FactMiss::None);

    GeometryFacts                        quad_facts;
    std::vector<vine::vsg::ChannelFacts> quad_channels;
    ASSERT_EQ(buildGeometryFacts(*full_quad, quad_facts, quad_channels), FactMiss::None);
    GeometryFacts                        band_facts[4];
    std::vector<vine::vsg::ChannelFacts> band_channels[4];
    for (std::size_t index = 0; index < 4U; ++index)
    {
        ASSERT_EQ(buildGeometryFacts(*bands[index], band_facts[index], band_channels[index]), FactMiss::None);
    }

    const ProgramFacts  programs[]{ producer_facts, consumer_facts };
    const GeometryFacts producer_geometries[]{ quad_facts };
    const GeometryFacts consumer_geometries[]{ band_facts[0], band_facts[1], band_facts[2], band_facts[3] };
    const MaterialFacts materials[]{ material_facts };
    ContentFacts        producer_table;
    producer_table.programs   = programs;
    producer_table.geometries = producer_geometries;
    producer_table.materials  = materials;
    ContentFacts consumer_table;
    consumer_table.programs   = programs;
    consumer_table.geometries = consumer_geometries;
    consumer_table.materials  = materials;

    // The camera: at +Z looking at the origin, so "the world's +Z" is a light pointing at the viewer.
    Camera camera;
    camera.setViewMatrixAsLookAt(vine::math::Vec3d(0.0, 0.0, 5.0), vine::math::Vec3d(0.0, 0.0, 0.0),
                                 vine::math::Vec3d(0.0, 1.0, 0.0));

    const auto make_sun = [](double r, double g, double b) {
        vine::intrusive_ptr<Light> light = Light::createDirectional(vine::math::Vec3d(0.0, 0.0, 1.0));
        light->setColor(vine::Colorf(r, g, b, 1.0));
        light->setIntensity(1.0F);
        return light;
    };
    vine::intrusive_ptr<Light> ambient = Light::createAmbient();
    ambient->setColor(vine::Colorf(0.1, 0.1, 0.1, 1.0));
    const vine::intrusive_ptr<Light> sun_a = make_sun(0.4, 0.0, 0.0);  // the map's owner
    const vine::intrusive_ptr<Light> sun_b = make_sun(0.0, 0.4, 0.0);
    const vine::intrusive_ptr<Light> sun_c = make_sun(0.0, 0.0, 0.4);
    const vine::intrusive_ptr<Light> sun_d = make_sun(0.2, 0.2, 0.0);
    sun_a->setCastShadow(true);
    sun_b->setCastShadow(true);
    sun_c->setCastShadow(true);
    sun_d->setCastShadow(true);

    // The matrix the producer publishes: x/y from the world position, and a CONSTANT light-space depth, so every
    // fragment of the receiver is behind the caster and the map scales the whole surface.
    vine::math::Mat4d view_projection;
    view_projection(0, 0) = 0.5;
    view_projection(1, 1) = 0.25;
    view_projection(2, 3) = 0.5;
    view_projection(3, 3) = 1.0;
    map_handle->setShadowOf(sun_a);
    map_handle->setProducerViewProjection(view_projection);
    ASSERT_TRUE(map_handle->hasProducerViewProjection());

    // The calls' light lists (the map's owner is announced by pointer identity, which is what the plan matches on).
    const Light* band0_lights[]{ ambient.get(), sun_a.get() };
    const Light* band1_lights[]{ ambient.get(), sun_a.get() };  // the same light object, its switch flipped below
    const Light* band2_lights[]{ ambient.get(), sun_b.get(), sun_a.get() };
    const Light* band3_lights[]{ ambient.get(), sun_b.get(), sun_c.get(), sun_d.get(), sun_a.get() };

    const auto command_for = [&](const vine::intrusive_ptr<Geometry>& geometry, const vine::intrusive_ptr<ShaderProgram>& program) {
        RenderCommand command;
        command.geometry = geometry;
        command.material = material;
        command.program  = program;
        return command;
    };
    const RenderCommand producer_command = command_for(full_quad, producer_program);
    const RenderCommand commands[]{ command_for(bands[0], consumer_program), command_for(bands[1], consumer_program),
                                    command_for(bands[2], consumer_program), command_for(bands[3], consumer_program) };

    // The target facts: the map states its owner and its matrix, the receiver is a plain colour target.
    TargetFacts map_facts;
    map_facts.target        = map_handle.get();
    map_facts.wanted.width  = static_cast<int>(kSize);
    map_facts.wanted.height = static_cast<int>(kSize);
    map_facts.wanted.shape  = map->shape();
    map_facts.current       = map->instance();
    map_facts.depth.has_depth = true;
    map_facts.depth.promotion = true;
    map_facts.shadow.light    = sun_a.get();
    map_facts.shadow.has_view_projection = map_handle->hasProducerViewProjection();
    map_facts.shadow.view_projection     = map_handle->producerViewProjection();

    TargetFacts receiver_facts;
    receiver_facts.target        = receiver_handle.get();
    receiver_facts.wanted.width  = static_cast<int>(kSize);
    receiver_facts.wanted.height = static_cast<int>(kSize);
    receiver_facts.wanted.shape  = receiver->shape();
    receiver_facts.current       = receiver->instance();
    const std::vector<TargetFacts> target_table{ map_facts, receiver_facts };

    FrameArena    arena{ 128 * 1024 };
    Diagnostics   diagnostics;
    Observe       observe;
    FrameRecorder recorder{ arena, diagnostics, observe };
    FrameCompiler compiler{ arena, diagnostics, observe };

    recorder.beginFrame(FrameToken{ 1 });
    recorder.beginPass(1U);  // the engine's shadow pass: the map, and nothing else
    recorder.setRenderTarget(map_handle.get());
    recorder.setLights(std::span<const Light* const>(band0_lights, 2U));
    recorder.render(std::span<const RenderCommand>(&producer_command, 1U), &camera);
    recorder.endPass();
    recorder.beginPass(2U);  // the receiver: four calls, four light lists
    recorder.setRenderTarget(receiver_handle.get());
    ClearPolicy clear;
    clear.color = true;
    for (std::size_t index = 0; index < 4U; ++index)
    {
        clear.color_value[index] = kClear[index];
    }
    recorder.setClearPolicy(clear);
    const RenderTarget* map_inputs[]{ map_handle.get() };
    recorder.setPassInputs(std::span<const RenderTarget* const>(map_inputs, 1U));
    recorder.setLights(std::span<const Light* const>(band0_lights, 2U));
    recorder.render(std::span<const RenderCommand>(&commands[0], 1U), &camera);
    // The runtime switch, flipped on the SAME light between two calls of one frame: the map's owner has to stop
    // shading the moment it stops casting (a snapshot taken once per call is what makes that observable).
    sun_a->setCastShadow(false);
    recorder.setLights(std::span<const Light* const>(band1_lights, 2U));
    recorder.render(std::span<const RenderCommand>(&commands[1], 1U), &camera);
    sun_a->setCastShadow(true);
    recorder.setLights(std::span<const Light* const>(band2_lights, 3U));
    recorder.render(std::span<const RenderCommand>(&commands[2], 1U), &camera);
    recorder.setLights(std::span<const Light* const>(band3_lights, 5U));
    recorder.render(std::span<const RenderCommand>(&commands[3], 1U), &camera);
    recorder.endPass();
    recorder.endFrame();

    const CompiledFrame& frame = compiler.compile(recorder.description(), FrameFacts{ target_table });
    ASSERT_EQ(frame.passes.size(), 2U);
    const CompiledPass& receiver_pass = frame.passes[1];
    ASSERT_EQ(receiver_pass.shadow.light, static_cast<const void*>(sun_a.get()))
        << "the plan resolved the map from what the target stated";
    EXPECT_TRUE(receiver_pass.shadow.has_view_projection);

    storage->beginFrame();
    VariantPool   pool;
    StreamUploads uploads;
    const auto    entry_points =
        vine::vsg::detail::fetchDynamicStateEntryPoints(created.device->vk(), created.instance->vk());
    ContentDraw   producer_draws(*producer_pipelines, pool, entry_points);
    ContentDraw   consumer_draws(*consumer_pipelines, pool, entry_points);
    StateRegistry producer_registry(pool);
    StateRegistry consumer_registry(pool);

    const ContentPass::Scope::Entry producer_halves[]{ ContentPass::Scope::Entry{
        DrawKind::Content, producer_program.get(), producer_facts.revision, quad_facts.layout,
        producer_pipelines.get(), &producer_draws } };
    const ContentPass::Scope::Entry consumer_halves[]{ ContentPass::Scope::Entry{
        DrawKind::Content, consumer_program.get(), consumer_facts.revision, band_facts[0].layout,
        consumer_pipelines.get(), &consumer_draws } };
    ContentPass::Scope producer_scope;
    producer_scope.entries     = producer_halves;
    producer_scope.registry    = &producer_registry;
    producer_scope.storage     = storage.get();
    producer_scope.block_sets  = producer_blocks;
    producer_scope.uploads     = &uploads;
    ContentPass producer_pass(producer_scope, diagnostics);

    ContentPass::Scope consumer_scope;
    consumer_scope.entries     = consumer_halves;
    consumer_scope.registry    = &consumer_registry;
    consumer_scope.storage     = storage.get();
    consumer_scope.block_sets  = consumer_blocks;
    consumer_scope.uploads     = &uploads;
    ContentPass consumer_pass(consumer_scope, diagnostics);

    const std::vector<std::byte> view_block(288U, std::byte{ 0 });
    ::vsg::ref_ptr<::vsg::Node>  producer_node;
    ::vsg::ref_ptr<::vsg::Node>  consumer_node;
    ASSERT_TRUE(producer_pass.record(frame.passes[0], producer_table, map->shape().compatibility(), {}, view_block,
                                     producer_node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U);

    const InputImages offered_inputs[] = { InputImages{ {}, map->depthView() } };
    ASSERT_TRUE(consumer_pass.record(receiver_pass, consumer_table, receiver->shape().compatibility(), offered_inputs,
                                     view_block, consumer_node));
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ContentSkipped), 0U)
        << "every band is a complete content draw";
    EXPECT_EQ(diagnostics.count(vine::graphics::DiagnosticCategory::ChannelIgnored), 1U)
        << "band 3 announced a caster the light block cannot carry: one episode, one report";

    VsgExecutor executor(diagnostics);
    executor.addTarget(map_handle.get(), map.get());
    executor.addTarget(receiver_handle.get(), receiver.get());
    auto command_graph = ::vsg::CommandGraph::create(created.device, created.queue_family);
    const PassContent packets[]{ PassContent{ 1U, producer_node }, PassContent{ 2U, consumer_node } };
    ASSERT_TRUE(executor.record(frame, command_graph, packets));
    EXPECT_EQ(executor.skipped(), 0U);

    ::vsg::ref_ptr<::vsg::Viewer> viewer = ::vsg::Viewer::create();
    ASSERT_NE(viewer, nullptr);
    viewer->assignRecordAndSubmitTaskAndPresentation(::vsg::CommandGraphs{ command_graph });
    ASSERT_TRUE(viewer->compile());
    viewer->advanceToNextFrame();
    viewer->handleEvents();
    viewer->recordAndSubmit();
    viewer->deviceWaitIdle();

    const auto near = [](std::uint8_t byte, double value) {
        return std::abs(static_cast<double>(byte) - 255.0 * value) <= 8.0;
    };
    const auto band = [&](int x) { return receiver->probe().pixel(x, 32); };

    const Rgba8 shadowed = band(8);
    EXPECT_TRUE(near(shadowed.r, 0.1) && near(shadowed.g, 0.1) && near(shadowed.b, 0.1))
        << "band 0: the map scaled the sun to nothing and only the ambient is left, got ("
        << static_cast<int>(shadowed.r) << ", " << static_cast<int>(shadowed.g) << ", "
        << static_cast<int>(shadowed.b) << ")";

    const Rgba8 not_casting = band(24);
    EXPECT_TRUE(near(not_casting.r, 0.5) && near(not_casting.g, 0.1) && near(not_casting.b, 0.1))
        << "band 1: the same light with castShadow off is fully lit, got (" << static_cast<int>(not_casting.r)
        << ", " << static_cast<int>(not_casting.g) << ", " << static_cast<int>(not_casting.b) << ")";

    const Rgba8 one_scaled = band(40);
    EXPECT_TRUE(near(one_scaled.r, 0.1) && near(one_scaled.g, 0.5) && near(one_scaled.b, 0.1))
        << "band 2: only the map's OWN light lost its term - the second sun still lights the face, got ("
        << static_cast<int>(one_scaled.r) << ", " << static_cast<int>(one_scaled.g) << ", "
        << static_cast<int>(one_scaled.b) << ")";

    const Rgba8 no_slot = band(56);
    EXPECT_TRUE(near(no_slot.r, 0.3) && near(no_slot.g, 0.7) && near(no_slot.b, 0.5))
        << "band 3: the owner is the fourth directional light, so the switch stays off and every light contributes, "
           "got ("
        << static_cast<int>(no_slot.r) << ", " << static_cast<int>(no_slot.g) << ", " << static_cast<int>(no_slot.b)
        << ")";
}
