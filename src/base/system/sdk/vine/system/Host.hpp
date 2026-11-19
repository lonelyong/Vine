#pragma once
#include "system_global.hpp"

#include <vine/String.hpp>

VN_SYSTEM_NS_BEGIN

/**
 * @brief Identifies the machine and the user the process runs as.
 *
 * @details
 * - Both answers are cheap and are read on every call - a host can be renamed while the process runs, and a test
 *   fixture can replace the identity it observes.
 * - A name the platform does not expose is an empty string, never a placeholder such as "localhost" or "unknown": a
 *   log line that prints nothing is honest, a log line that prints the wrong machine is not.
 *
 * @note
 * - These are display identities for logs, About panels and crash reports. Neither value is unique, neither is
 *   stable across a rename, and neither may be used as an authentication or authorisation input.
 */
class VN_SYSTEM_API Host {

  public:
    /**
     * @brief Returns the machine's host name.
     *
     * @details
     * - The DNS host name where the platform distinguishes it from the legacy short name (Windows), otherwise the
     *   name `gethostname()` reports.
     *
     * @return The host name, or an empty string when the platform cannot answer.
     */
    static String hostName();

    /**
     * @brief Returns the account name of the user that owns this process.
     *
     * @details
     * - The account name, not the display or full name: that is the spelling a path, a log or a support ticket uses.
     *
     * @return The user name, or an empty string when the platform cannot answer.
     */
    static String userName();
};

VN_SYSTEM_NS_END
