#pragma once
#include "system_global.hpp"

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Describes the operating system the process is running on.
 *
 * @details
 * - `name` is the platform family, `osVersion` is the product version a user recognises, and
 *   `kernelVersion` is the kernel the process actually runs on. They are two facts: a distribution
 *   changes its kernel without changing its product version, and a container runs another kernel
 *   than its product version suggests.
 *
 * @note
 * - Each field carries the platform's own spelling; the fields are not a normalised version triple.
 */
struct VN_SYSTEM_API OperatingSystemInfo {
    String name;          // "Windows", "Linux", "macOS"; "Unknown" on a platform the module does not implement.
    String osVersion;     // Windows "10.0.19045"; Linux the distribution's PRETTY_NAME (e.g. "Ubuntu 24.04.1 LTS"); macOS "14.6.1".
    String kernelVersion; // The uname release on Linux and macOS; the OS build on Windows, which has one version.
    String architecture;  // "x64", "x86", "arm64", the platform's own machine spelling, or "unknown".
};

/**
 * @brief Provides read-only information about the host operating system.
 */
class VN_SYSTEM_API OperatingSystem {

  public:
    /**
     * @brief Returns information about the host operating system.
     *
     * @details
     * - The info is built once on first call and then cached; the call is thread-safe (the cache is a function-local
     *   static, initialised under the language's guard variable).
     * - A fact the platform cannot answer is reported as "unknown", never as an empty string.
     *
     * @return The operating system information; borrowed, valid for the lifetime of the process.
     */
    static const OperatingSystemInfo& info();
};

VN_SYSTEM_NS_END
