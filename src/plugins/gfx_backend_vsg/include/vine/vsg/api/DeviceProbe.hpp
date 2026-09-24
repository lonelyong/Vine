#pragma once

#include <cstdint>
#include <vector>

#include <vine/String.hpp>
#include <vine/vsg/core/DeviceRequirements.hpp>
#include <vine/vsg/vsg_global.hpp>

/**
 * @brief The one place that asks a Vulkan loader what this machine offers, expressed in the core's terms.
 *
 * This is a THIN ADAPTER and deliberately nothing more: it opens a throwaway instance, enumerates the
 * physical devices, reads the two answers the policy needs (the reported version and a handful of
 * feature bits) and closes the instance again. It decides nothing - `usable` is the core's verdict,
 * copied in so a caller has one thing to print - and it never creates a device, a window or a surface,
 * which is what lets a session's device requirements be checked on a machine that cannot present
 * anything at all (a headless run, a CI container).
 *
 * It is also the FIRST piece of the new backend that touches the graphics API, and it lives on the
 * API side of the boundary from §2.3 of the design: `core/` states the policy, this states the
 * machine, and the check between them is `core::satisfiesRequirements()`.
 *
 * FAILURE IS DESCRIBED, NOT THROWN: a machine with no loader (or an instance the loader refuses)
 * reports `ok == false` with a sentence, and a loader that offers no device reports `ok == true` with
 * an empty list. Those are different situations for a caller - "Vulkan is not installed" and "Vulkan is
 * installed and sees no device" - and collapsing them would make a headless CI box look like a broken
 * build.
 */
namespace vsg
{
class PhysicalDevice;
}

VN_VSG_NS_BEGIN

namespace api
{

/** @brief One physical device, described in the terms the core policy checks. */
struct ProbedDevice
{
    core::DeviceFacts facts;        ///< Reported version and the required-feature flags.
    vn::String      name;         ///< Device name as the driver reports it (diagnostics only).
    bool              usable{false};///< The core's verdict for this device.
};

/** @brief What an enumeration attempt found, or why it could not run. */
struct ProbeResult
{
    bool                      ok{false};         ///< Whether an instance could be opened at all.
    std::uint32_t             loader_version{0}; ///< Instance version the loader granted (diagnostic).
    std::vector<ProbedDevice> devices;           ///< Every physical device offered, in loader order.
    vn::String              error;             ///< Why the probe could not run, when !ok.

    /** @brief Gets how many of the offered devices satisfy the requirements. */
    [[nodiscard]] std::size_t usableCount() const noexcept;
};

/** @brief Opens a throwaway instance and enumerates the physical devices with the facts the policy needs.
 *
 * @return The devices (possibly none), or the reason the probe could not run at all.
 */
[[nodiscard]] ProbeResult probePhysicalDevices();

/** @brief Describes one physical device in the terms the policy checks.
 *
 * Exported because a caller that already has an instance - `createDevice` picks a device from one - must ask
 * the SAME question the probe asks. Two spellings of "what can this device do" is how a selection ends up
 * accepting a device the probe would have refused.
 *
 * @param device Physical device to describe.
 * @return Its facts, name and the core's verdict.
 */
[[nodiscard]] ProbedDevice describePhysicalDevice(::vsg::PhysicalDevice& device);

}  // namespace api

VN_VSG_NS_END
