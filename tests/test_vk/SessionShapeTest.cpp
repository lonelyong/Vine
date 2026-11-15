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

TEST(VkSessionShapeTest, TheDeviceHandsOutANewDriverAndKeepsItsOwn)
{
    auto backend = RenderBackendRegistry::instance().create(u8"vk", DeviceDesc{ DevicePresentation::None, nullptr });
    ASSERT_NE(backend, nullptr);
    if (!backend->initialize()) {
        GTEST_SKIP() << "this machine has no Vulkan device the backend can come up on";
    }
    auto* const device = dynamic_cast<vn::backend_vk::Device*>(backend.get());
    ASSERT_NE(device, nullptr);

    // S6b: `createSession()` hands out a REAL second driver - its own command machinery, its own frame scratch - so it
    // is a different object from the device's own executor. A device borrowed before initialize() answers an inert
    // one that comes up with the device (see VkMultiSessionTest).
    RenderFrameExecutor* const session = backend->createSession();
    ASSERT_NE(session, nullptr);
    EXPECT_NE(session, &backend->executor()) << "the second driver IS the device's own session";

    // Releasing what the device owns is a no-op; releasing a borrowed driver drops it.
    backend->releaseSession(backend->executor());
    EXPECT_EQ(&backend->executor(), &device->executor()) << "releasing the device's own driver replaced it";
    backend->releaseSession(*session);
    EXPECT_TRUE(backend->initialized());

    backend->shutdown();
}
