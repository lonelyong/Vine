/**
 * @brief The clear-value list of a pass' graph: attachment order, and which union member gets what.
 *
 * VkRenderPassBeginInfo::pClearValues is indexed by ATTACHMENT, and VkClearValue is a UNION — so
 * writing a colour into the depth attachment's entry (or the reverse) is not a type error anywhere, it
 * validates clean, and it clears the wrong thing to a bit pattern nobody asked for. That is exactly
 * what D48 was, and the rule that prevents it used to be spelled out by hand in three places (the
 * new-pass builder and the two "the pass' request changed" updates). It is one function now, so the
 * three cannot drift, and this file pins the rule the way the rest of the pass layer is pinned: on
 * values, with no device involved.
 *
 * What the rule is: attachment 0 carries THIS pass' colour, extra colour attachments (MRT / G-buffer)
 * stay transparent black, the depth attachment is LAST and carries the target's own depth clear value,
 * and a colour write only ever touches attachment 0 of a pass that HAS colour.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include <vsg/state/ImageView.h>

#include <vine/vsg/VsgPassMaterialiser.hpp>

using vine::vsg::detail::makePassClearValues;
using vine::vsg::detail::setColorClearValue;
using vine::vsg::VsgRenderTargetEntry;

namespace
{

/// @brief A target entry with @p color_count colour attachments and, optionally, a depth attachment.
///
/// The images are never built (a device-free test): only the entries' SHAPE and the depth clear value
/// are read, which is what the clear-value list is derived from.
VsgRenderTargetEntry makeTarget(std::size_t color_count, bool has_depth, float depth_clear_value = 0.75f)
{
    VsgRenderTargetEntry t;
    t.color_views.resize(color_count);
    for (auto& view : t.color_views) {
        view = ::vsg::ImageView::create();
    }
    if (has_depth) {
        t.depth_clear_value = depth_clear_value;
    }
    return t;
}

/// @brief The colour entry of @p values, as four floats (for readable failures).
std::vector<float> colorOf(const VkClearValue& value)
{
    return { value.color.float32[0], value.color.float32[1], value.color.float32[2], value.color.float32[3] };
}

} // namespace

TEST(PassClearValuesTest, TheListFollowsTheAttachmentOrderWithTheColourFirstAndDepthLast)
{
    const auto target = makeTarget(/*color_count*/ 3, /*has_depth*/ true, 0.75f);
    const auto values = makePassClearValues(target, /*has_depth*/ true, ::vsg::vec4(0.1f, 0.2f, 0.3f, 0.4f));

    ASSERT_EQ(values.size(), 4u);
    EXPECT_EQ(colorOf(values[0]), (std::vector<float>{ 0.1f, 0.2f, 0.3f, 0.4f }));
    // The extra MRT attachments stay transparent black: their region is black until a fragment writes
    // it, and they must NOT be cleared to the pass' own colour (that is the pass' attachment 0 only).
    EXPECT_EQ(colorOf(values[1]), (std::vector<float>{ 0.0f, 0.0f, 0.0f, 0.0f }));
    EXPECT_EQ(colorOf(values[2]), (std::vector<float>{ 0.0f, 0.0f, 0.0f, 0.0f }));
    // Depth is LAST, and its value is in the DEPTH member of the union.
    EXPECT_FLOAT_EQ(values[3].depthStencil.depth, 0.75f);
    // The other way round would be the D48 defect: attachment 0 read as a depth entry.
    EXPECT_NE(values[0].depthStencil.depth, 0.75f);
}

TEST(PassClearValuesTest, ADepthOnlyTargetGetsExactlyTheDepthEntry)
{
    const auto target = makeTarget(/*color_count*/ 0, /*has_depth*/ true, 0.5f);
    const auto values = makePassClearValues(target, /*has_depth*/ true, ::vsg::vec4(1.0f, 0.0f, 0.0f, 1.0f));

    ASSERT_EQ(values.size(), 1u);
    EXPECT_FLOAT_EQ(values.front().depthStencil.depth, 0.5f);
}

TEST(PassClearValuesTest, AColourOnlyTargetGetsNoDepthEntry)
{
    const auto target = makeTarget(/*color_count*/ 2, /*has_depth*/ false);
    const auto values = makePassClearValues(target, /*has_depth*/ false, ::vsg::vec4(0.25f, 0.5f, 0.75f, 1.0f));

    ASSERT_EQ(values.size(), 2u);
    EXPECT_EQ(colorOf(values[0]), (std::vector<float>{ 0.25f, 0.5f, 0.75f, 1.0f }));
    EXPECT_EQ(colorOf(values[1]), (std::vector<float>{ 0.0f, 0.0f, 0.0f, 0.0f }));
}

TEST(PassClearValuesTest, AColourWriteReplacesAttachmentZeroAndNothingElse)
{
    auto target = makeTarget(/*color_count*/ 3, /*has_depth*/ true, 0.75f);
    auto values = makePassClearValues(target, /*has_depth*/ true, ::vsg::vec4(0.1f, 0.2f, 0.3f, 0.4f));

    ASSERT_TRUE(setColorClearValue(values, /*has_color*/ true, ::vsg::vec4(0.9f, 0.8f, 0.7f, 0.6f)));
    EXPECT_EQ(colorOf(values[0]), (std::vector<float>{ 0.9f, 0.8f, 0.7f, 0.6f }));
    EXPECT_EQ(colorOf(values[1]), (std::vector<float>{ 0.0f, 0.0f, 0.0f, 0.0f }));
    EXPECT_FLOAT_EQ(values[3].depthStencil.depth, 0.75f);
}

TEST(PassClearValuesTest, AColourWriteRefusesADepthOnlyPassAndLeavesItsDepthAlone)
{
    // The pass may have STARTED clearing colour after it was built, so "there is a colour entry" is a
    // question about the pass' attachments, not about the vector being non-empty: a depth-only pass'
    // single entry is its depth value, and writing a colour there is the union defect.
    const auto target = makeTarget(/*color_count*/ 0, /*has_depth*/ true, 0.5f);
    auto       values = makePassClearValues(target, /*has_depth*/ true, ::vsg::vec4(0.0f, 0.0f, 0.0f, 1.0f));

    EXPECT_FALSE(setColorClearValue(values, /*has_color*/ false, ::vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
    ASSERT_EQ(values.size(), 1u);
    EXPECT_FLOAT_EQ(values.front().depthStencil.depth, 0.5f) << "the depth entry must not be touched";

    // An empty list with no colour is the same refusal, not a write out of bounds.
    std::vector<VkClearValue> empty;
    EXPECT_FALSE(setColorClearValue(empty, /*has_color*/ true, ::vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f)));
    EXPECT_TRUE(empty.empty());
}
