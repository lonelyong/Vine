/**
 * @brief M1's mapping table, tested with no device: every neutral usage becomes exactly one Vulkan state.
 *
 * The table is the only place a layout, its stages and its access mask are decided together (see BarrierPlan). A case
 * here that passes on a machine with no Vulkan loader is the point: the table is pure, so drift in it is caught by the
 * build rather than by a pixel that "looks fine".
 */

#include <gtest/gtest.h>

#include <array>
#include <set>

#include <vine/backend_vk/internal/BarrierPlan.hpp>
#include <vine/graphics/DrawState.hpp>

using vn::backend_vk::barrierFor;
using vn::backend_vk::ImageState;
using vn::backend_vk::loadOpOf;
using vn::backend_vk::requiresTransition;
using vn::backend_vk::stateOf;
using vn::backend_vk::storeOpOf;
using vn::backend_vk::ImageLayout;
using vn::graphics::LoadOp;
using vn::graphics::StoreOp;

namespace
{

/// Every usage the SDK states, so a new one cannot be added without this table failing.
constexpr std::array<ImageLayout, 5> kUsages{
    ImageLayout::Undefined,
    ImageLayout::ColorAttachment,
    ImageLayout::DepthAttachment,
    ImageLayout::ShaderReadOnly,
    ImageLayout::Present,
};

} // namespace

TEST(BarrierPlanTest, EveryUsageHasItsOwnLayoutAndStage)
{
    std::set<VkImageLayout> layouts;
    for (const ImageLayout usage : kUsages) {
        const ImageState state = stateOf(usage);
        EXPECT_NE(state.stages, 0U) << "usage " << static_cast<int>(usage) << " must wait for some stage";
        layouts.insert(state.layout);
    }
    EXPECT_EQ(layouts.size(), kUsages.size()) << "two usages sharing a layout would make one of them a lie";
}

TEST(BarrierPlanTest, AUsageTransitionsNoLayoutAgainstItself)
{
    // WHAT THE ANSWER MEANS: `requiresTransition` describes the LAYOUT half only. Two identical usages need no layout
    // change - and they may still need the barrier, which the next case pins.
    for (const ImageLayout usage : kUsages) {
        EXPECT_FALSE(requiresTransition(usage, usage)) << "two identical usages state the same layout";
    }
    EXPECT_TRUE(requiresTransition(ImageLayout::ColorAttachment, ImageLayout::ShaderReadOnly));
    EXPECT_TRUE(requiresTransition(ImageLayout::Present, ImageLayout::ColorAttachment));
}

TEST(BarrierPlanTest, ASameStateBarrierStillCarriesTheDependency)
{
    // THE RULE DYNAMIC RENDERING RESTS ON (see DynamicRendering::beginRendering): a pass into an attachment an earlier
    // pass wrote states the SAME layout twice, and that barrier is the execution dependency between the two passes - the
    // half `vkCmdBeginRendering` does not insert. A BORROWING target's depth is the extreme case: the image belongs to
    // the lender, the layout does not move at all, and the barrier is still what orders the lender's write before the
    // borrower's LOAD and depth test. So a same-state barrier must carry a real source AND a real destination scope.
    const VkImage image = reinterpret_cast<VkImage>(static_cast<std::uint64_t>(0x1234));

    const VkImageMemoryBarrier2 barrier =
        barrierFor(image, ImageLayout::DepthAttachment, ImageLayout::DepthAttachment, VK_IMAGE_ASPECT_DEPTH_BIT);

    EXPECT_EQ(barrier.oldLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_EQ(barrier.newLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    EXPECT_NE(barrier.srcStageMask, 0U) << "the writer must still be waited for";
    EXPECT_NE(barrier.srcAccessMask, 0U) << "the write must still be made available";
    EXPECT_NE(barrier.dstStageMask & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT), 0U)
        << "the pass' depth test is what reads the attachment";
    EXPECT_NE(barrier.dstAccessMask & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, 0U)
        << "a LOAD (and the depth test) reads, so the destination scope must include the read";
}

TEST(BarrierPlanTest, ATransitionCarriesBothSidesOfTheTable)
{
    // A synthetic handle: the function is pure, so its value is checkable without an image.
    const VkImage image = reinterpret_cast<VkImage>(static_cast<std::uint64_t>(0x1234));

    const VkImageMemoryBarrier2 barrier =
        barrierFor(image, ImageLayout::ColorAttachment, ImageLayout::ShaderReadOnly, VK_IMAGE_ASPECT_COLOR_BIT);

    const ImageState before = stateOf(ImageLayout::ColorAttachment);
    const ImageState after  = stateOf(ImageLayout::ShaderReadOnly);

    EXPECT_EQ(barrier.oldLayout, before.layout);
    EXPECT_EQ(barrier.newLayout, after.layout);
    EXPECT_EQ(barrier.srcStageMask, before.stages) << "the writer's stages travel on the barrier (synchronisation 2)";
    EXPECT_EQ(barrier.dstStageMask, after.stages) << "the reader's stages travel on the barrier too";
    EXPECT_EQ(barrier.srcAccessMask, before.access);
    EXPECT_EQ(barrier.dstAccessMask, after.access) << "the reader must be made to wait for the writer's access";
    EXPECT_EQ(barrier.image, image);
    EXPECT_EQ(barrier.subresourceRange.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
    EXPECT_EQ(barrier.subresourceRange.levelCount, 1U);
    EXPECT_EQ(barrier.subresourceRange.layerCount, 1U);
    EXPECT_EQ(barrier.srcQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED) << "one queue family records everything here";
    EXPECT_EQ(barrier.dstQueueFamilyIndex, VK_QUEUE_FAMILY_IGNORED);
}

TEST(BarrierPlanTest, UndefinedDiscardsAndPresentOwnsNothing)
{
    // The two ends of the table are the ones a mistake hides in: "discard the contents" and "hand it to the platform".
    const ImageState undefined = stateOf(ImageLayout::Undefined);
    EXPECT_EQ(undefined.layout, VK_IMAGE_LAYOUT_UNDEFINED);
    EXPECT_EQ(undefined.stages, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
    EXPECT_EQ(undefined.access, 0U) << "nothing is made visible for a layout whose contents do not matter";

    const ImageState present = stateOf(ImageLayout::Present);
    EXPECT_EQ(present.layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    EXPECT_EQ(present.stages, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
}

TEST(BarrierPlanTest, APassClockBoundaryWaitsForEveryCommand)
{
    // THE STAGE IS THE MEANING (see kPassClockBoundaryStage). A timestamp answers "when did everything before it
    // finish", so a pass' boundary must wait for every command the pass recorded; `TOP_OF_PIPE` records the ARRIVAL
    // instead, which is queue latency rather than the pass. A software rasteriser executes commands in order and makes
    // either value look plausible, so the stage itself is pinned here rather than observed from a timing.
    EXPECT_EQ(vn::backend_vk::kPassClockBoundaryStage, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    // AND THE FRAME'S START IS THE ONE BOUNDARY THAT MUST NOT WAIT: nothing before it belongs to this frame.
    EXPECT_EQ(vn::backend_vk::kFrameClockStartStage, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
}

TEST(BarrierPlanTest, LoadAndStoreOperationsMapOneToOne)
{
    EXPECT_EQ(loadOpOf(LoadOp::Load), VK_ATTACHMENT_LOAD_OP_LOAD);
    EXPECT_EQ(loadOpOf(LoadOp::Clear), VK_ATTACHMENT_LOAD_OP_CLEAR);
    EXPECT_EQ(loadOpOf(LoadOp::DontCare), VK_ATTACHMENT_LOAD_OP_DONT_CARE);

    EXPECT_EQ(storeOpOf(StoreOp::Store), VK_ATTACHMENT_STORE_OP_STORE);
    EXPECT_EQ(storeOpOf(StoreOp::DontCare), VK_ATTACHMENT_STORE_OP_DONT_CARE);
}
