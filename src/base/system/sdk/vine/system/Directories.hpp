#pragma once
#include "system_global.hpp"

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Answers where the platform keeps the per-user directories.
 *
 * @details
 * - These are the ROOTS only. This module knows where a user's data lives, not where one application's does: the
 *   `<root>/<organization>/<application>` layout is the framework's (appfw::Application::dataDirectory()).
 * - Where the platform defines a variable for the answer (XDG_*, LOCALAPPDATA, TMPDIR), the variable wins over the
 *   convention, and a relative value is ignored as the XDG specification requires.
 * - No path carries a trailing separator, so `root + "/" + name` is always well formed. A root that is itself a
 *   separator ("/") is returned as is.
 * - Nothing here creates a directory; a caller that needs one creates it.
 *
 * @note
 * - An empty string means "the platform has no answer": a container can run without a home directory and with the
 *   environment stripped.
 * - The answers are read on every call and are cheap; none of them is cached, because the environment can be replaced
 *   by the host after startup.
 */
class VN_SYSTEM_API Directories {

  public:
    /**
     * @brief Returns the system's temporary root.
     *
     * @details
     * - `$TMPDIR` when it is absolute, otherwise `/tmp` on Linux, the per-user `$TMPDIR` on macOS, and the directory
     *   `GetTempPathW` reports on Windows (per-user unless the machine is configured otherwise).
     *
     * @return The temporary directory, or an empty string when the platform has none.
     */
    static String temporary();

    /**
     * @brief Returns the current user's home directory.
     *
     * @details
     * - `$HOME` on POSIX, `%USERPROFILE%` (falling back to `%HOMEDRIVE%%HOMEPATH%`) on Windows.
     *
     * @return The home directory, or an empty string when the process has none.
     */
    static String home();

    /**
     * @brief Returns the root for per-user application data.
     *
     * @details
     * - Linux: `$XDG_DATA_HOME`, otherwise `~/.local/share`.
     * - Windows: `%LOCALAPPDATA%`, otherwise `<home>/AppData/Local`.
     * - macOS: `~/Library/Application Support`.
     *
     * @return The data root, or an empty string when it cannot be determined.
     */
    static String data();

    /**
     * @brief Returns the root for per-user configuration.
     *
     * @details
     * - Linux: `$XDG_CONFIG_HOME`, otherwise `~/.config`.
     * - Windows: `%APPDATA%` (the roaming profile), otherwise `<home>/AppData/Roaming`.
     * - macOS: `~/Library/Preferences`.
     *
     * @return The configuration root, or an empty string when it cannot be determined.
     */
    static String config();

    /**
     * @brief Returns the root for per-user cache data.
     *
     * @details
     * - Linux: `$XDG_CACHE_HOME`, otherwise `~/.cache`.
     * - Windows: `%LOCALAPPDATA%`: the platform has no separate cache root, so this answer equals data().
     * - macOS: `~/Library/Caches`.
     *
     * @return The cache root, or an empty string when it cannot be determined.
     */
    static String cache();

    /**
     * @brief Returns the directory that holds the running executable.
     *
     * @details
     * - Derived from `Process::currentExecutablePath()`, so the two cannot disagree.
     *
     * @return The directory, or an empty string when the executable path cannot be determined.
     */
    static String executableDirectory();
};

VN_SYSTEM_NS_END
