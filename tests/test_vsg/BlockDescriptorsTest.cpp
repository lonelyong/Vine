/**
 * @brief The descriptor side of the block storage, on a real device.
 *
 * The mechanism these cases pin: ONE set binds the storage buffer once per region (`range` = one block), and
 * a draw chooses its blocks with three DYNAMIC OFFSETS in binding order. That is what keeps an edit a write
 * and a new frame an offset instead of a rebind - the failure family this backend has actually shipped is
 * the opposite (a material edit that wrote a descriptor, a resize that rebuilt every program slot).
 *
 * Refusals are pinned too: a dynamic offset must be a multiple of the device's
 * `minUniformBufferOffsetAlignment`, and an offset that is not gets NO command and a counted refusal rather
 * than a validation error (`VUID-vkCmdBindDescriptorSets-pDynamicOffsets-01971`). Repointing is pinned as
 * well: a replaced storage keeps the LAYOUT and swaps the set's elements.
 *
 * X11 + a device that satisfies the backend's requirements, or it SKIPS.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

#include <vsg/app/Window.h>
#include <vsg/app/WindowTraits.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/Image.h>
#include <vsg/state/ImageView.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>

using vn::vsg::BlockDescriptors;
using vn::vsg::BlockStorage;
using vn::vsg::api::probePhysicalDevices;

namespace
{

/// @brief A window (and with it a device) the test owns, plus the storage and descriptors built on it.
class Fixture
{
  public:
    [[nodiscard]] static bool available()
    {
        return std::getenv("DISPLAY") != nullptr && probePhysicalDevices().usableCount() > 0;
    }

    bool build(const BlockStorage::Layout& layout)
    {
        auto traits         = ::vsg::WindowTraits::create();
        traits->width       = 64;
        traits->height      = 64;
        traits->windowTitle = "Vine block descriptors test";
        window              = ::vsg::Window::create(traits);
        if (window == nullptr) {
            return false;
        }
        device = window->getOrCreateDevice();
        if (device == nullptr) {
            return false;
        }
        storage = BlockStorage::create(device, layout);
        if (storage == nullptr) {
            return false;
        }
        descriptors = BlockDescriptors::create(device, *storage);
        return descriptors != nullptr;
    }

    ~Fixture()
    {
        // Descriptors reference the storage (its buffer); the device belongs to the window.
        descriptors.reset();
        storage.reset();
        device.reset();
        window.reset();
    }

    ::vsg::ref_ptr<::vsg::Window>  window;
    ::vsg::ref_ptr<::vsg::Device>  device;
    std::unique_ptr<BlockStorage>  storage;
    std::unique_ptr<BlockDescriptors> descriptors;
};

/// @brief Builds the pipeline layout the binds are recorded against (its set 0 is the block set).
::vsg::ref_ptr<::vsg::PipelineLayout> pipelineLayoutFor(const ::vsg::ref_ptr<::vsg::Device>& device,
                                                        const ::vsg::ref_ptr<::vsg::DescriptorSetLayout>& set_layout)
{
    // A pipeline layout is constructed from its set layouts and push ranges and compiled later by a
    // `vsg::Context`; what the bind command stores is the object, so no compilation is needed here.
    (void)device;
    return ::vsg::PipelineLayout::create(::vsg::DescriptorSetLayouts{ set_layout }, ::vsg::PushConstantRanges{});
}

}  // namespace

TEST(BlockDescriptorsTest, TheLayoutDeclaresOneDynamicBindingPerBlock)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    const auto layout = fixture.descriptors->layout();
    ASSERT_NE(layout, nullptr);
    ASSERT_EQ(layout->bindings.size(), 5U);

    const auto expect_binding = [&layout](std::size_t index, std::uint32_t binding) {
        const auto& declared = layout->bindings[index];
        EXPECT_EQ(declared.binding, binding);
        EXPECT_EQ(declared.descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC)
            << "the block is chosen by a dynamic offset, never by a per-draw set";
        EXPECT_EQ(declared.descriptorCount, 1U);
        EXPECT_EQ(declared.stageFlags & VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_VERTEX_BIT);
        EXPECT_EQ(declared.stageFlags & VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    };
    expect_binding(0, BlockDescriptors::kViewBinding);
    expect_binding(1, BlockDescriptors::kDrawBinding);
    expect_binding(2, BlockDescriptors::kMaterialBinding);
    expect_binding(3, BlockDescriptors::kLightsBinding);
    expect_binding(4, BlockDescriptors::kShadowBinding);
}

TEST(BlockDescriptorsTest, TheSetBindsOneBlockPerRegionAtOffsetZero)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    const auto set = fixture.descriptors->set();
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->descriptors.size(), 5U);

    const BlockStorage::Strides strides = fixture.storage->strides();
    const std::uint64_t         expected_ranges[5] = { strides.view, strides.draw, strides.material, strides.light,
                                                       strides.shadow };

    for (std::size_t index = 0; index < set->descriptors.size(); ++index) {
        const auto* descriptor = dynamic_cast<const ::vsg::DescriptorBuffer*>(set->descriptors[index].get());
        ASSERT_NE(descriptor, nullptr) << "every block is a uniform buffer";
        EXPECT_EQ(descriptor->descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
        EXPECT_EQ(descriptor->dstBinding, index) << "bindings are declared in region order";
        ASSERT_EQ(descriptor->bufferInfoList.size(), 1U);
        const auto& info = descriptor->bufferInfoList.front();
        EXPECT_EQ(info->buffer, fixture.storage->buffer()) << "the ONE buffer the frame writes";
        EXPECT_EQ(info->offset, 0U) << "the block is chosen by the dynamic offset, not by this";
        EXPECT_EQ(info->range, expected_ranges[index]) << "range is ONE block: the stride of its region";
    }
}

TEST(BlockDescriptorsTest, TheBindCarriesTheOffsetsInBindingOrderAndReusesOneSet)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));
    const auto pipeline_layout = pipelineLayoutFor(fixture.device, fixture.descriptors->layout());
    ASSERT_NE(pipeline_layout, nullptr);

    const BlockDescriptors::Offsets first{ 0, 80, 0, 0, 0 };
    const BlockDescriptors::Offsets second{ 256, 336, 64, 112, 160 };

    const auto first_bind  = fixture.descriptors->bind(pipeline_layout, first);
    const auto second_bind = fixture.descriptors->bind(pipeline_layout, second);

    ASSERT_NE(first_bind, nullptr);
    ASSERT_NE(second_bind, nullptr);
    EXPECT_EQ(first_bind->dynamicOffsets, (std::vector<std::uint32_t>{ 0, 80, 0, 0, 0 }))
        << "one offset per binding, in binding order (view, draw, material, lights, shadow)";
    EXPECT_EQ(second_bind->dynamicOffsets, (std::vector<std::uint32_t>{ 256, 336, 64, 112, 160 }));
    EXPECT_EQ(first_bind->firstSet, fixture.descriptors->setIndex());
    EXPECT_EQ(first_bind->descriptorSet, second_bind->descriptorSet)
        << "every draw binds the SAME set: the offsets carry the difference";
    EXPECT_EQ(fixture.descriptors->refusals(), 0U);
}

TEST(BlockDescriptorsTest, TheDeclaredShapeDecidesTheBindingsAndTheOffsets)
{
    // A shape that is NOT this backend's own arrangement: the view block at binding 0 and the material at
    // binding 3 (the engine's programs put the material at 0 and the per-drawable block in set 1). The set
    // this object builds is whatever its caller declared, and the offsets follow THAT shape's order - the
    // canonical five would be wrong here, and a set bound with the wrong offsets reads another draw's bytes.
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    const BlockDescriptors::Binding declared[]{ { 0U, vn::vsg::AbiBlockRole::View },
                                                { 3U, vn::vsg::AbiBlockRole::Material } };
    std::unique_ptr<BlockDescriptors> descriptors =
        BlockDescriptors::create(fixture.device, *fixture.storage, declared, 1U);
    ASSERT_NE(descriptors, nullptr);

    const std::span<const BlockDescriptors::Binding> shape = descriptors->shape();
    ASSERT_EQ(shape.size(), 2U);
    EXPECT_EQ(shape[0].binding, 0U);
    EXPECT_EQ(shape[1].binding, 3U);
    EXPECT_EQ(descriptors->setIndex(), 1U) << "the set the program declares its blocks in is the caller's";

    const auto layout = descriptors->layout();
    ASSERT_NE(layout, nullptr);
    ASSERT_EQ(layout->bindings.size(), 2U);
    EXPECT_EQ(layout->bindings[0].binding, 0U);
    EXPECT_EQ(layout->bindings[1].binding, 3U);
    EXPECT_EQ(layout->bindings[1].descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);

    const auto bound =
        descriptors->bind(pipelineLayoutFor(fixture.device, layout), BlockDescriptors::Offsets{ 0, 0, 64, 0, 0 });
    ASSERT_NE(bound, nullptr);
    EXPECT_EQ(bound->dynamicOffsets, (std::vector<std::uint32_t>{ 0, 64 }))
        << "one offset per DECLARED binding, in the declared order";
    EXPECT_EQ(descriptors->refusals(), 0U);
}

TEST(BlockDescriptorsTest, ADeclaredSetCarriesTheMaterialBlockAndItsMap)
{
    // The engine's own set 0 is BOTH: the material's block at binding 0 and the diffuse map at binding 1. One
    // declared set, one layout, one bind - which is what lets a program written against that arrangement be
    // served without the backend inventing a second set for the image.
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    // An image and a view for the map: nothing samples it here (the set is never submitted), so it only has to
    // be a real image object - the Context compiles it into a VkImage whenever a frame first needs it.
    auto image = ::vsg::Image::create();
    image->format = VK_FORMAT_R8G8B8A8_UNORM;
    image->usage  = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto view = ::vsg::ImageView::create(image, VK_IMAGE_VIEW_TYPE_2D);
    ASSERT_NE(view, nullptr);
    const auto sampler = ::vsg::Sampler::create();
    ASSERT_NE(sampler, nullptr);

    const BlockDescriptors::Binding declared[]{ { 0U, vn::vsg::AbiBlockRole::Material } };
    const BlockDescriptors::SampledBinding maps[]{ { 1U, view, sampler } };
    std::unique_ptr<BlockDescriptors> descriptors =
        BlockDescriptors::create(fixture.device, *fixture.storage, declared, 0U, maps);
    ASSERT_NE(descriptors, nullptr);

    const auto layout = descriptors->layout();
    ASSERT_NE(layout, nullptr);
    ASSERT_EQ(layout->bindings.size(), 2U) << "the block AND the map, in one declared set";
    EXPECT_EQ(layout->bindings[0].binding, 0U);
    EXPECT_EQ(layout->bindings[0].descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    EXPECT_EQ(layout->bindings[1].binding, 1U);
    EXPECT_EQ(layout->bindings[1].descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    ASSERT_EQ(descriptors->samplers().size(), 1U) << "the images the set carries are readable back";
    EXPECT_EQ(descriptors->samplers()[0].binding, 1U);

    // The SET carries the same two things the layout declares, each at its own binding: a descriptor at the
    // wrong binding is a set the shader reads nothing from (the validation layer says so; the pixel may
    // still look plausible), so the set itself is checked - one buffer at 0 and one image at 1.
    const auto set = descriptors->set();
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(set->descriptors.size(), 2U);
    EXPECT_EQ(set->descriptors[0]->dstBinding, 0U);
    EXPECT_EQ(set->descriptors[0]->descriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    EXPECT_EQ(set->descriptors[1]->dstBinding, 1U);
    EXPECT_EQ(set->descriptors[1]->descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    // The bind still hands over ONE dynamic offset per BLOCK binding: the image is not a dynamic offset.
    const auto bound = descriptors->bind(pipelineLayoutFor(fixture.device, layout), BlockDescriptors::Offsets{});
    ASSERT_NE(bound, nullptr);
    EXPECT_EQ(bound->dynamicOffsets, (std::vector<std::uint32_t>{ 0U }))
        << "one offset per declared block, and none for the map";
}

TEST(BlockDescriptorsTest, AMisalignedOffsetIsRefusedInsteadOfBound)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));
    const auto pipeline_layout = pipelineLayoutFor(fixture.device, fixture.descriptors->layout());
    const std::uint64_t alignment = fixture.descriptors->alignment();

    BlockDescriptors::Offsets offsets{ 0, 0, 0 };
    offsets.draw = 1;  // deliberately off the alignment the device demands
    if (alignment <= 1U) {
        GTEST_SKIP() << "this device accepts any offset alignment";
    }

    EXPECT_EQ(fixture.descriptors->bind(pipeline_layout, offsets), nullptr)
        << "an offset the API forbids gets no command: it would be a validation error, not a draw";
    EXPECT_EQ(fixture.descriptors->refusals(), 1U);

    const auto proper = fixture.descriptors->bind(pipeline_layout, BlockDescriptors::Offsets{ 0, alignment, 0 });
    EXPECT_NE(proper, nullptr) << "a usable offset still binds";
    EXPECT_EQ(fixture.descriptors->refusals(), 1U) << "no refusal was added";
}

TEST(BlockDescriptorsTest, RepointingKeepsTheLayoutAndSwapsTheSetElements)
{
    if (!Fixture::available()) {
        GTEST_SKIP() << "no window system or no device satisfies the requirements";
    }
    Fixture fixture;
    ASSERT_TRUE(fixture.build({}));

    const auto layout_before = fixture.descriptors->layout();
    const auto set_before    = fixture.descriptors->set();

    // A session rebuilt its storage: same layout, new bytes.
    auto replacement = BlockStorage::create(fixture.device, BlockStorage::Layout{});
    ASSERT_NE(replacement, nullptr);

    ASSERT_TRUE(fixture.descriptors->repoint(*replacement));
    EXPECT_EQ(fixture.descriptors->layout(), layout_before) << "the layout survives: only the elements moved";
    EXPECT_NE(fixture.descriptors->set(), set_before) << "the set itself was replaced";

    const auto descriptor = dynamic_cast<const ::vsg::DescriptorBuffer*>(
        fixture.descriptors->set()->descriptors[BlockDescriptors::kMaterialBinding].get());
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->bufferInfoList.front()->buffer, replacement->buffer())
        << "the new set binds the NEW storage's buffer";
    EXPECT_EQ(descriptor->bufferInfoList.front()->range, replacement->strides().material);

    const auto pipeline_layout = pipelineLayoutFor(fixture.device, layout_before);
    const auto bound           = fixture.descriptors->bind(pipeline_layout, BlockDescriptors::Offsets{});
    ASSERT_NE(bound, nullptr);
    EXPECT_EQ(bound->descriptorSet, fixture.descriptors->set()) << "the bind uses the current set";
}
