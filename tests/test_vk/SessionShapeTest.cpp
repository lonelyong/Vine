/**
 * @brief The session seam as the SDK states it: a device hands out a frame driver and takes it back
 *        (see RenderDevice::createSession / releaseSession).
 *
 * WHY THIS FILE EXISTS. Step S6a introduces the seam without changing today's answer: this backend has one driver, so
 * `createSession()` answers the device's OWN executor and `releaseSession()` hands back nothing. What must be pinned
 * is that the default is exactly that - a backend that invented a second driver here would make two engines race over
 * caches that are not locked yet - and that releasing the device's own driver is harmless.
 */

#include <gtest/gtest.h>

#include <vine/graphics/RenderBackendRegistry.hpp>

#include <vine/backend_vk/internal/Device.hpp>

namespace
{

using vn::graphics::DeviceDesc;
using vn::graphics::DevicePresentation;
using vn::graphics::RenderBackendRegistry;
using vn::graphics::RenderFrameExecutor;

} // namespace

TEST(VkSessionShapeTest, TheDeviceHandsOutItsOwnDriverAndTakingItBackIsHarmless)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }

    // THE DEFAULT IS "ONE DEVICE, ONE DRIVER" (see RenderDevice::createSession): until the cache and queue locks land
    // (S6b), a second driver would let two engines race over state that is not guarded yet.
    RenderFrameExecutor* const session = backend->createSession();
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session, &backend->executor()) << "the vk device invented a second driver before its caches are guarded";

    // Releasing what the device owns is a no-op - and must not leave the driver unusable.
    backend->releaseSession(*session);
    backend->releaseSession(*session);
    EXPECT_TRUE(backend->initialized());
    EXPECT_EQ(&backend->executor(), session) << "a released own driver was replaced or destroyed";

    backend->shutdown();
}
