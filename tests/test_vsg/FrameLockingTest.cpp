/**
 * @brief A frame answers what IT spent on the device's content lock (see FrameLocking).
 *
 * The backend's three-phase rule (head / recording / commit, `graphics-threading.md` C3) says a frame takes the
 * content lock a CONSTANT number of times and records without it. That is a number the backend can report, and this
 * case is what makes "a steady frame costs the same whatever it draws, and never waits" checkable from outside -
 * the same way `FrameUploadsTest` pins what a frame uploaded.
 */

#include <gtest/gtest.h>

#include <cstdint>

#include <vine/graphics/DepthPolicy.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/backend_vsg/internal/VsgBackend.hpp>

#include "TestFrame.hpp"
#include "TestTriangleContent.hpp"

TEST(FrameLockingTest, AFrameReportsWhatItSpentOnTheContentLock)
{
    vn::graphics::DeviceDesc desc;
    desc.presentation = vn::graphics::DevicePresentation::None; // a device that presents nothing needs no display

    vn::intrusive_ptr<vn::backend_vsg::VsgBackend> backend(new vn::backend_vsg::VsgBackend(desc));
    if (!backend->initialize() || !backend->capabilities().render_targets) {
        GTEST_SKIP() << "no device session that serves off-screen pixels on this machine";
    }

    // BEFORE ANY FRAME: no answer at all - "not measured" and "measured zero" are different facts (see FrameReport).
    EXPECT_FALSE(backend->frameReport().locking.has_value()) << "no frame has run yet";

    vn::intrusive_ptr<vn::graphics::RenderTarget> target(new vn::graphics::RenderTarget());
    target->attachColor(vn::graphics::RenderTarget::ColorFormat::RGBA8);
    target->setSize(32, 32);
    vn::intrusive_ptr<vn::graphics::RenderPass> pass(new vn::graphics::RenderPass());
    vn::intrusive_ptr<vn::graphics::Camera>     camera = vn::test::makeCamera();
    const vn::graphics::ClearPolicy             clear{ vn::Colorf(0.0F, 0.25F, 0.0F, 1.0F), true };

    const auto drive = [&](const vn::test::TriangleFixture* content) {
        TestFrame frame;
        frame.pass(pass.get()).target(target.get()).clear(clear).depth(vn::graphics::DepthPolicy::TestAndWrite);
        if (content != nullptr) {
            frame.content(content->commands, camera.get());
        }
        backend->executeFrame(frame.value(), nullptr);
    };

    const vn::test::TriangleFixture triangle = vn::test::makeTriangle();
    drive(&triangle);
    const vn::graphics::FrameReport first = backend->frameReport();
    ASSERT_TRUE(first.locking.has_value()) << "a frame that ran measured its content lock";
    EXPECT_GT(first.locking->acquisitions, 0U) << "the head and the commit both take the device's content lock";
    EXPECT_EQ(first.locking->waits, 0U) << "a serial frame never waits for a lock nobody else holds";

    // A STEADY FRAME COSTS THE SAME: the number is per frame and independent of what the frame drew, which is what
    // the three-phase rule is for.
    drive(&triangle);
    const vn::graphics::FrameReport steady = backend->frameReport();
    ASSERT_TRUE(steady.locking.has_value());
    EXPECT_EQ(steady.locking->acquisitions, first.locking->acquisitions) << "a steady frame cost a different number of acquisitions";
    EXPECT_EQ(steady.locking->waits, 0U);

    backend->shutdown();
}
