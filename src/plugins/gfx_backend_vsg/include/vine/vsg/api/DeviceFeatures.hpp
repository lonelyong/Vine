#pragma once

#include <vsg/core/ref_ptr.h>
#include <vsg/vk/DeviceFeatures.h>

#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The device features this backend requires, in the API's vocabulary.
 *
 * This function IS the policy of `core::DeviceRequirements` spelled in the API's terms. The two must not
 * drift: a feature the policy requires but that is never requested means every pipeline that needs it is a
 * validation error at creation - on a device that would have served it. The compile-time assertion in the
 * implementation is what keeps the two counts in step.
 *
 * It lives here, and not inside the session, because a DEVICE is created in more than one way: a session
 * creates one through its window (WSI), and an off-screen caller creates one with no window at all. Two
 * spellings of one policy is exactly how a device ends up with different capabilities depending on which
 * door it came in.
 */
V_VSG_NS_BEGIN

/** @brief Requests every feature the device-floor policy names on @p features.
 *
 * @param features The feature chain handed to device creation (never null).
 */
void applyRequiredFeatures(const ::vsg::ref_ptr<::vsg::DeviceFeatures>& features);

/** @brief The device extensions the device-floor policy names, in the API's vocabulary.
 *
 * The names and not the features: an extension has to be ENABLED on the device for the dynamic states that
 * live in it to exist at all, while the feature structs above only say whether the states may be set. A
 * backend that asks for the bits and not for the names gets a pipeline the driver is entitled to reject (and
 * the validator does) - which is how a "dynamic" state ends up never being set.
 *
 * @return The extension names, in the order they should be enabled (EDS3 after EDS2).
 */
[[nodiscard]] ::vsg::Names requiredDeviceExtensions();

V_VSG_NS_END
