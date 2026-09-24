#pragma once
#include "system_global.hpp"

#include <vector>

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Provides information about processes and basic process operations.
 *
 * Queries use the Toolhelp API on Windows and the /proc filesystem on POSIX;
 * no process is spawned here. kill() is the only operation and terminates a
 * process forcefully.
 */
class VN_SYSTEM_API Process {

  public:
    /**
     * @brief Returns the current process identifier.
     *
     * @return The current process ID.
     */
    static int currentProcessId();

    /**
     * @brief Returns the path of the current executable.
     *
     * @return The executable path, or an empty string when it cannot be determined.
     */
    static String currentExecutablePath();

    /**
     * @brief Checks whether a process with the given identifier exists.
     *
     * @param pid Process identifier; must be positive.
     * @return true if a process with pid exists.
     */
    static bool exists(int pid);

    /**
     * @brief Finds processes whose executable name equals the given name.
     *
     * On Windows the executable file name is compared case-insensitively; on
     * POSIX the resolved /proc/<pid>/exe file name is compared exactly.
     *
     * @param name Executable file name, e.g. "test_system.exe".
     * @return The identifiers of all matching processes.
     */
    static std::vector<int> findByName(const String& name);

    /**
     * @brief Terminates the process with the given identifier.
     *
     * Forcefully terminates the process; cleanup is not guaranteed.
     *
     * @param pid Process identifier; must be positive.
     * @return true if the process was terminated.
     */
    static bool kill(int pid);
};

VN_SYSTEM_NS_END
