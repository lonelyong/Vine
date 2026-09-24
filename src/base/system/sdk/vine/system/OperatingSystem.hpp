#pragma once
#include "system_global.hpp"

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Describes the operating system the process is running on.
 */
struct VN_SYSTEM_API OperatingSystemInfo {
    String name;         // e.g. "Windows", "Linux", "macOS".
    String version;      // OS version, e.g. "10.0.19045".
    String architecture; // CPU architecture, e.g. "x64", "x86", "arm64".
};

/**
 * @brief Provides read-only information about the host operating system.
 */
class VN_SYSTEM_API OperatingSystem {

  public:
    /**
     * @brief Returns information about the host operating system.
     *
     * The info is built once on first call and then cached; the call is
     * thread-safe.
     *
     * @return The operating system information.
     */
    static const OperatingSystemInfo& info();
};

VN_SYSTEM_NS_END
