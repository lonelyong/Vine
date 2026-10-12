#pragma once

#include <cstdint>

#include <vsg/core/ref_ptr.h>
#include <vsg/core/Exception.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/Instance.h>

#include <vine/String.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief A device without a window: the seam an off-screen caller (a test, a headless phase) needs.
 *
 * WHY IT EXISTS. Everything this backend does - uploading geometry, building pipelines, recording draws,
 * reading a target back - needs a DEVICE, and only a few of those things need a WINDOW (a swapchain does).
 * The session reaches its device through a window because that is what WSI is; a phase that renders into an
 * off-screen target should not have to create a window (and so a display, and so an X server) to get one.
 *
 * WHAT IT SELECTS, AND WITH WHICH POLICY. The same floor the session applies: `core::DeviceRequirements`,
 * checked per physical device, and the required feature chain requested on the one that passes - see
 * `applyRequiredFeatures`. A caller therefore gets a device that satisfies the policy or a reason it could
 * not, never a device that happens to be first.
 */
VN_VSG_NS_BEGIN

/** @brief What a caller may ask about the device it wants. */
struct DeviceOptions
{
    bool validation{false};  ///< Enable the validation layer (a phase that asserts behaviour wants it).
};

/** @brief What creating a device produced. */
struct DeviceResult
{
    bool                          ok{false};         ///< Whether a device satisfying the policy was created.
    vn::String                  error;            ///< Why not, in the caller's words (empty when ok).
    bool                          validation{false}; ///< Whether the validation layer is ACTUALLY enabled.
    ::vsg::ref_ptr<::vsg::Instance> instance;       ///< The instance the device belongs to.
    ::vsg::ref_ptr<::vsg::Device>   device;         ///< The logical device.
    int                           queue_family{-1}; ///< The graphics queue family the device was built with.
};

/** @brief Creates a device that satisfies the device-floor policy, without creating a window.
 *
 * @param options Whether to enable validation.
 * @return The device (and its instance and queue family), or `ok == false` with a reason.
 *         Never throws: vsg's own failure is caught and answered as a reason (see deviceFailureMessage).
 */
[[nodiscard]] DeviceResult createDevice(const DeviceOptions& options = {});

/** @brief The sentence a device that could not be created is reported with.
 *
 * vsg's device layer refuses a creation by THROWING a `vsg::Exception` (it is NOT a `std::exception`, so a
 * catch that names only that one lets it escape), and the failure a host actually hits is the device CAP: a
 * build supports `vsg::Device::maxNumDevices()` simultaneous devices, and a host that keeps one per render
 * area (one view = one control = one session, see appfw's document model) reaches it by opening one more.
 *
 * The reason states vsg's own words - it is the party that knows what the driver refused - and then the
 * standing fact about this build's cap and what a host can do about it. Both the window-less seam
 * (`createDevice`) and the session (which creates its device through a window) report through this ONE
 * sentence, so what a host reads does not depend on which door the failure came through.
 *
 * @param error The exception vsg threw.
 * @return The reason, in this SDK's words (never empty).
 */
[[nodiscard]] vn::String deviceFailureMessage(const ::vsg::Exception& error);

VN_VSG_NS_END
