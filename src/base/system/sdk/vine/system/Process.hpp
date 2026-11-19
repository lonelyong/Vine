#pragma once
#include "system_global.hpp"

#include <vector>

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Provides information about processes and basic process operations.
 *
 * @details
 * - Queries use the Toolhelp API on Windows, the /proc filesystem on Linux and libproc on macOS; no process is spawned
 *   here, and no failure reason is reported: a query that cannot answer returns its empty value.
 * - kill() is the only operation that changes the system.
 *
 * @note
 * - A platform the module does not implement answers every query with its empty value.
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
     * @note
     * - No error is reported: an empty result means "the platform could not determine it", which includes a platform the
     *   module does not implement.
     *
     * @return The executable path, or an empty string when it cannot be determined.
     */
    static String currentExecutablePath();

    /**
     * @brief Checks whether a process with the given identifier exists.
     *
     * @details
     * - A process that exists but cannot be opened (a protected or privileged one) is reported as existing on every
     *   platform: the question is existence, not accessibility.
     * - Identifiers <= 0 are rejected without asking the platform.
     *
     * @param pid Process identifier; must be positive.
     * @return true if a process with pid exists.
     */
    static bool exists(int pid);

    /**
     * @brief Finds processes whose executable name equals the given name.
     *
     * @details
     * - Windows compares the executable name case-insensitively; Linux resolves /proc/<pid>/exe and compares the file name
     *   exactly.
     * - The comparison is against a file name, not a path: a directory part in @p name never matches.
     *
     * @note
     * - POSIX resolves each process' executable (Linux /proc/<pid>/exe, macOS proc_pidpath) and skips a process the
     *   caller may not inspect; Windows reads the name from its process snapshot.
     * - A process whose executable was deleted reports a name ending in " (deleted)" on Linux and is not matched.
     * - An empty @p name matches nothing on every platform.
     *
     * @param name Executable file name, e.g. "test_system.exe".
     * @return The identifiers of all matching processes.
     */
    static std::vector<int> findByName(const String& name);

    /**
     * @brief Terminates the process with the given identifier.
     *
     * @details
     * - POSIX sends SIGKILL and Windows calls TerminateProcess: the target's cleanup handlers do not run either way.
     * - Identifiers <= 0 are rejected without asking the platform.
     *
     * @param pid Process identifier; must be positive.
     * @return true when the platform reports the process was terminated; false when the call was refused (no right to
     *         terminate it, or no such process).
     */
    static bool kill(int pid);
};

VN_SYSTEM_NS_END
