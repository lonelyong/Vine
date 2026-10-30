#pragma once

#include <vine/crash/CrashHandler.hpp>

VN_CRASH_NS_BEGIN
namespace detail {

/**
 * @brief Platform half of install(): resolves @p config and arms the operating system's hook.
 *
 * @details
 * - Runs while the process is healthy, so it may allocate and may touch the filesystem: it turns @p config into the plain
 *   buffers the crash path copies out of static storage.
 * - Defined once per platform; the unsupported platforms return false, which is how @ref install reports "no handler here".
 *
 * @param config Install-time configuration; only read here, never during a crash.
 * @return true when a handler was armed.
 */
bool installPlatform(const Config& config);

/**
 * @brief Platform half of uninstall(): restores the previous hook and clears the snapshot.
 *
 * @details
 * - Safe to call when nothing was installed, so @ref install can start with it.
 */
void uninstallPlatform();

} // namespace detail
VN_CRASH_NS_END
