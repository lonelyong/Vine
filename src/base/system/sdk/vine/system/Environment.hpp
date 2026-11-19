#pragma once
#include "system_global.hpp"

#include <utility>
#include <vector>

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Reads the process environment.
 *
 * @details
 * - Read-only: this module never sets or clears a variable. The environment is process-global state shared with the C
 *   runtime and every dependency, so a library that mutates it changes the behaviour of components it cannot see.
 * - Names and values are UTF-8 on every platform; the Windows half converts from UTF-16, so a value outside the ANSI
 *   code page survives the round trip.
 * - The environment is fixed at process start for the caller's purposes: nothing here re-reads the cache the C runtime
 *   keeps, so a variable set by another library after startup is visible only if the platform's own lookup sees it.
 *
 * @note
 * - Windows reports the per-drive current-directory entries (`=C:=C:\dir`) through the same API as variables. They are
 *   hidden here: they are not name/value pairs a caller can use.
 */
class VN_SYSTEM_API Environment {

  public:
    /**
     * @brief Whether the variable @p name is set.
     *
     * @details
     * - A variable that is set to an empty string counts as set, which is the distinction `value()` cannot make.
     * - An empty @p name is never set.
     *
     * @param name Variable name; case-insensitive on Windows, case-sensitive on POSIX.
     * @return true when the variable exists.
     */
    static bool has(const String& name);

    /**
     * @brief Returns the value of the variable @p name.
     *
     * @note
     * - An empty result means "not set or set to empty": use has() to tell the two apart.
     *
     * @param name Variable name; case-insensitive on Windows, case-sensitive on POSIX.
     * @return The value, or an empty string when the variable is not set.
     */
    static String value(const String& name);

    /**
     * @brief Returns every variable the process inherited.
     *
     * @details
     * - One entry per variable, split at the first '=' so a value may contain the character.
     * - The order is the platform's; no sorting is applied, because a caller that wants an order can sort a map itself.
     *
     * @return The name/value pairs.
     */
    static std::vector<std::pair<String, String>> variables();
};

VN_SYSTEM_NS_END
