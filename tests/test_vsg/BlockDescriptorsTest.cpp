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

#include <vsg/app/Window.h>
#include <vsg/app/WindowTraits.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/vk/Device.h>

#include <vine/vsg/api/BlockDescriptors.hpp>
#include <vine/vsg/api/BlockStorage.hpp>
#include <vine/vsg/api/DeviceProbe.hpp>

using vine::vsg::BlockDescriptors;
using vine::vsg::BlockStorage;
using vine::vsg::api::probePhysicalDevices;

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
    ASSERT_EQ(layout->bindings.size(), 4U);

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
    ASSERT_EQ(set->descriptors.size(), 4U);

    const BlockStorage::Strides strides = fixture.storage->strides();
    const std::uint64_t         expected_ranges[4] = { strides.view, strides.draw, strides.material, strides.light };

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

    const BlockDescriptors::Offsets first{ 0, 80, 0, 0 };
    const BlockDescriptors::Offsets second{ 256, 336, 64, 112 };

    const auto first_bind  = fixture.descriptors->bind(pipeline_layout, first);
    const auto second_bind = fixture.descriptors->bind(pipeline_layout, second);

    ASSERT_NE(first_bind, nullptr);
    ASSERT_NE(second_bind, nullptr);
    EXPECT_EQ(first_bind->dynamicOffsets, (std::vector<std::uint32_t>{ 0, 80, 0, 0 }))
        << "one offset per binding, in binding order (view, draw, material, lights)";
    EXPECT_EQ(second_bind->dynamicOffsets, (std::vector<std::uint32_t>{ 256, 336, 64, 112 }));
    EXPECT_EQ(first_bind->firstSet, fixture.descriptors->setIndex());
    EXPECT_EQ(first_bind->descriptorSet, second_bind->descriptorSet)
        << "every draw binds the SAME set: the offsets carry the difference";
    EXPECT_EQ(fixture.descriptors->refusals(), 0U);
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
