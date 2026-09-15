/**
 * @brief The one place a content slot's viewport is decided: `detail::updateSlotViewport`.
 *
 * The rectangle comes from two sources (the pass' announced sub-viewport, or the target's size when the
 * pass presents full-target content) and is asserted from two callers (the per-frame render path and the
 * renderer's resize()). It used to build a NEW vsg::ViewportState every call, which allocated on every
 * steady-state frame per slot — and the object it replaced was not even needed: vsg re-emits
 * vkCmdSetViewport from the state on every recording, so re-asserting the same rectangle needs no new
 * object. What is pinned here is that the state is written in place and is still the object the camera
 * records.
 *
 * No device is needed: a ViewportState is a plain state command.
 */

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <optional>

#include <vine/graphics/Viewport.hpp>

#include <vine/vsg/CameraBridge.hpp>
#include <vine/vsg/VsgContentSlot.hpp>
#include <vine/vsg/VsgRenderTargetEntry.hpp>

#include <vsg/app/Camera.h>
#include <vsg/state/ViewportState.h>

using vine::graphics::Viewport;
using vine::vsg::CameraBridge;
using vine::vsg::ContentSlot;
using vine::vsg::detail::updateSlotViewport;

namespace
{

/// A slot whose camera exists (its viewport state is what these tests drive).
///
/// Constructed in place: a ContentSlot owns a SceneBridge, so it is neither copyable nor movable.
std::unique_ptr<ContentSlot> makeSlot()
{
    static CameraBridge bridge;
    auto              slot    = std::make_unique<ContentSlot>();
    slot->vsg_camera          = bridge.create(nullptr);
    return slot;
}

/// The rectangle the slot's state holds right now.
std::array<int, 4> viewportRect(const ContentSlot& slot)
{
    const auto& vk_viewport = slot.viewport_state->getViewport();
    return { static_cast<int>(vk_viewport.x), static_cast<int>(vk_viewport.y),
             static_cast<int>(vk_viewport.width), static_cast<int>(vk_viewport.height) };
}

} // namespace

TEST(ContentSlotViewportTest, TheStateIsCreatedOnceAndReused)
{
    const auto slot = makeSlot();
    ASSERT_EQ(slot->viewport_state, nullptr);

    updateSlotViewport(*slot, /*presenting*/ false, Viewport{ 4, 8, 320, 240 }, 1280, 720);
    ASSERT_NE(slot->viewport_state, nullptr);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 4, 8, 320, 240 }));
    EXPECT_EQ(slot->vsg_camera->viewportState, slot->viewport_state)
        << "the camera must record the state the slot keeps";

    const auto* state = slot->viewport_state.get();

    // The same rectangle again: same object, and nothing was re-created for it.
    updateSlotViewport(*slot, /*presenting*/ false, Viewport{ 4, 8, 320, 240 }, 1280, 720);
    EXPECT_EQ(slot->viewport_state.get(), state);

    // A new rectangle: still the same object, with the new values (a fresh object would leave the
    // recorded state pointing at the old one, and the resize path asserts rectangles it never rebuilt).
    updateSlotViewport(*slot, /*presenting*/ false, Viewport{ 0, 0, 640, 480 }, 1280, 720);
    EXPECT_EQ(slot->viewport_state.get(), state);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 0, 0, 640, 480 }));
}

TEST(ContentSlotViewportTest, PresentingContentFillsTheTargetWhateverThePassAnnounced)
{
    const auto slot = makeSlot();

    // A HUD pass announces a sub-viewport; the same slot presenting the frame must fill the target
    // instead (the presenting role wins), and the scissor follows the viewport.
    updateSlotViewport(*slot, /*presenting*/ true, Viewport{ 10, 10, 64, 64 }, 1280, 720);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 0, 0, 1280, 720 }));
    EXPECT_EQ(slot->viewport_state->getScissor().offset.x, 0);
    EXPECT_EQ(slot->viewport_state->getScissor().offset.y, 0);
    EXPECT_EQ(slot->viewport_state->getScissor().extent.width, 1280u);
    EXPECT_EQ(slot->viewport_state->getScissor().extent.height, 720u);

    updateSlotViewport(*slot, /*presenting*/ false, Viewport{ 10, 10, 64, 64 }, 1280, 720);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 10, 10, 64, 64 }));
    EXPECT_EQ(slot->viewport_state->getScissor().offset.x, 10);
    EXPECT_EQ(slot->viewport_state->getScissor().extent.width, 64u);
}

TEST(ContentSlotViewportTest, ASurfaceWithNoSizeIsNotAsserted)
{
    // A swapchain can be 0x0 (a minimised window): that is not a rectangle to record, and recording it
    // would make the slot draw nothing instead of keeping the last real one.
    const auto slot      = makeSlot();
    slot->viewport_state = ::vsg::ViewportState::create(1, 2, 800u, 600u);
    slot->viewport_x     = 1;
    slot->viewport_y     = 2;
    slot->viewport_w     = 800;
    slot->viewport_h     = 600;

    updateSlotViewport(*slot, /*presenting*/ true, std::nullopt, 0, 0);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 1, 2, 800, 600 })) << "the last real rectangle stays";

    // A SUB-VIEWPORT of zero size is a different case and keeps the rule it always had: it is not a
    // usable rectangle either, so the slot falls back to the whole surface (a pass that announces a
    // degenerate viewport draws over the target rather than nowhere).
    updateSlotViewport(*slot, /*presenting*/ false, Viewport{ 0, 0, 0, 0 }, 1280, 720);
    EXPECT_EQ(viewportRect(*slot), (std::array<int, 4>{ 0, 0, 1280, 720 }));
}
