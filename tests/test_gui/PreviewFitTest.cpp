/**
 * @brief The letterbox arithmetic of the deferred demo's preview strip (`app_shell/PreviewFit.hpp`).
 *
 * WHY IT IS WORTH A TEST. The slot is a fixed box and the G-buffer follows the window, so the copy is
 * isotropic only while the two aspects agree; getting this arithmetic wrong is a picture nobody can grep for
 * (it was wrong once: the previews were squeezed by exactly the aspect difference, and every count-based
 * assertion stayed green). The function is pure, so the cases below are the whole contract.
 */

#include <gtest/gtest.h>

#include <vine/graphics/Viewport.hpp>

#include "PreviewFit.hpp"

using vn::app_shell::fitPreviewRect;

TEST(PreviewFitTest, AWiderSourceIsLetterboxedVertically)
{
    // 32x16 (2:1) in a 10x10 slot at (3, 4): the width binds, so the picture is 10x5 and centred.
    const vn::graphics::Viewport rect = fitPreviewRect(32, 16, 3, 4, 10, 10);
    EXPECT_EQ(rect.x, 3);
    EXPECT_EQ(rect.y, 6) << "centred: 4 + (10 - 5) / 2";
    EXPECT_EQ(rect.width, 10);
    EXPECT_EQ(rect.height, 5);
}

TEST(PreviewFitTest, ATallerSourceIsPillarboxed)
{
    // 16x32 in the same slot: the height binds instead, and the picture is 5x10 centred.
    const vn::graphics::Viewport rect = fitPreviewRect(16, 32, 3, 4, 10, 10);
    EXPECT_EQ(rect.x, 5) << "centred: 3 + (10 - 5) / 2";
    EXPECT_EQ(rect.y, 4);
    EXPECT_EQ(rect.width, 5);
    EXPECT_EQ(rect.height, 10);
}

TEST(PreviewFitTest, ASourceOfTheSlotsOwnAspectFillsItExactly)
{
    const vn::graphics::Viewport rect = fitPreviewRect(320, 200, 8, 8, 160, 100);
    EXPECT_EQ(rect.x, 8);
    EXPECT_EQ(rect.y, 8);
    EXPECT_EQ(rect.width, 160);
    EXPECT_EQ(rect.height, 100);
}

TEST(PreviewFitTest, ADegenerateSourceAnswersTheSlotItself)
{
    // "Not laid out yet" is not an aspect to fit: the slot is the answer, so the preview shows the whole
    // (empty) attachment rather than nothing at all.
    for (const vn::graphics::Viewport& rect : { fitPreviewRect(0, 200, 8, 8, 160, 100),
                                                  fitPreviewRect(320, 0, 8, 8, 160, 100) }) {
        EXPECT_EQ(rect.x, 8);
        EXPECT_EQ(rect.y, 8);
        EXPECT_EQ(rect.width, 160);
        EXPECT_EQ(rect.height, 100);
    }
}

TEST(PreviewFitTest, ADegenerateSlotAnswersTheSlotAsGiven)
{
    // A slot with no extent is the caller's own box, zeros included: this function never invents a size, it
    // only fits a picture inside what the caller said.
    const vn::graphics::Viewport no_width = fitPreviewRect(320, 200, 8, 8, 0, 100);
    EXPECT_EQ(no_width.x, 8);
    EXPECT_EQ(no_width.y, 8);
    EXPECT_EQ(no_width.width, 0);
    EXPECT_EQ(no_width.height, 100);

    const vn::graphics::Viewport no_height = fitPreviewRect(320, 200, 8, 8, 160, 0);
    EXPECT_EQ(no_height.x, 8);
    EXPECT_EQ(no_height.y, 8);
    EXPECT_EQ(no_height.width, 160);
    EXPECT_EQ(no_height.height, 0);
}

TEST(PreviewFitTest, AVeryThinSourceStillGetsAPixel)
{
    // A 100x1 strip in a 4x4 slot rounds to 0 pixels high: the clamp is what keeps the copy a copy, and the
    // picture is centred on that one row rather than at the slot's edge.
    const vn::graphics::Viewport rect = fitPreviewRect(100, 1, 0, 0, 4, 4);
    EXPECT_EQ(rect.width, 4);
    EXPECT_EQ(rect.height, 1) << "at least one pixel a side";
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 1) << "centred: (4 - 1) / 2";
}
