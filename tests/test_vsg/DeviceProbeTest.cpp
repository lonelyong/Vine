/**
 * @brief The device probe: the API-side half of the device policy, exercised on whatever machine runs it.
 *
 * The probe opens a throwaway instance and asks what the machine offers - and that is ALL it needs to do
 * its job, which is why this test runs headless: no window, no surface, no device. What it must not do
 * is carry its own opinion about which device is usable: the verdict is the core policy's, and the
 * assertions below are exactly that claim (a device the probe calls usable must satisfy the requirements,
 * and a device it refuses must fail one of the two halves).
 *
 * The run prints one `[device_probe]` line per device, because a probe that quietly finds nothing and a
 * probe that is broken look identical in a pass/fail summary. A machine with no loader, and a machine
 * with a loader and no device, both SKIP rather than fail: neither is a defect in this code.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include <vine/vsg/api/DeviceProbe.hpp>
#include <vine/vsg/core/DeviceRequirements.hpp>

using vine::vsg::api::probePhysicalDevices;
using vine::vsg::core::kDeviceFeatureCount;
using vine::vsg::core::kDeviceExtensionCount;
using vine::vsg::core::missingExtensionCount;
using vine::vsg::core::missingFeatureCount;
using vine::vsg::core::satisfiesRequirements;
using vine::vsg::core::supportsRequiredVersion;

namespace
{

/// @brief Prints a `vine::String` (which holds UTF-8 bytes) as the bytes it holds.
std::string as_bytes(const vine::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace

TEST(DeviceProbeTest, EveryOfferedDeviceIsDescribedInTheTermsThePolicyChecks)
{
    const auto result = probePhysicalDevices();
    if (!result.ok)
    {
        GTEST_SKIP() << as_bytes(result.error);
    }
    if (result.devices.empty())
    {
        GTEST_SKIP() << "the loader was reachable but offered no physical device";
    }

    std::size_t usable = 0;
    for (const auto& device : result.devices)
    {
        const auto& facts = device.facts;
        std::printf("[device_probe] name=%s api=%u.%u.%u features=%zu/%zu extensions=%zu/%zu usable=%d\n",
                    as_bytes(device.name).c_str(), (facts.api_version >> 22U) & 0x7FU,
                    (facts.api_version >> 12U) & 0x3FFU, facts.api_version & 0xFFFU,
                    kDeviceFeatureCount - missingFeatureCount(facts), kDeviceFeatureCount,
                    kDeviceExtensionCount - missingExtensionCount(facts), kDeviceExtensionCount,
                    device.usable ? 1 : 0);
        std::fflush(stdout);

        EXPECT_FALSE(device.name.empty()) << "a device nobody can name cannot be reported to a user";

        // One verdict, not two: the probe copies the policy's answer instead of forming its own.
        EXPECT_EQ(device.usable, satisfiesRequirements(facts));

        if (device.usable)
        {
            ++usable;
            EXPECT_EQ(missingFeatureCount(facts), 0U);
            EXPECT_EQ(missingExtensionCount(facts), 0U) << "the extension half of the floor is checked too";
            EXPECT_TRUE(supportsRequiredVersion(facts.api_version));
        }
        else
        {
            EXPECT_TRUE(!supportsRequiredVersion(facts.api_version) || missingFeatureCount(facts) != 0U ||
                        missingExtensionCount(facts) != 0U)
                << "a refused device must fail the version floor, a feature, or an extension";
        }
    }

    EXPECT_EQ(result.usableCount(), usable);
    std::printf("[device_probe] devices=%zu usable=%zu loader=%u.%u\n", result.devices.size(), usable,
                (result.loader_version >> 22U) & 0x7FU, (result.loader_version >> 12U) & 0x3FFU);
    std::fflush(stdout);
}
