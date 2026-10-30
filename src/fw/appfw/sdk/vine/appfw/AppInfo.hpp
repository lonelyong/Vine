#pragma once

#include "appfw_global.hpp"

#include <filesystem>
#include <vector>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief One "label: value" fact shown in the about content ("Qt" / "6.7.2").
 */
struct AppInfoRow {
    String label; ///< Left-hand label.
    String value; ///< Right-hand value.
};

/**
 * @brief An external link shown in the about content.
 */
struct AppInfoLink {
    String text; ///< Text shown for the link.
    String url;  ///< Absolute URL opened by the desktop browser.
};

/**
 * @brief What the application is: the one description every "about" surface reads.
 *
 * @details
 * - It belongs to the application, not to a widget: the host fills it when it builds the process (AppConfig::info) and
 *   the framework keeps it (Application::appInfo()). AboutPanel, AboutDialog, the window title and any command that
 *   reports identity read the same copy.
 * - It is plain data: no Qt types and no I/O, so it can be built, copied and compared in a headless process (icons are
 *   paths; a reader resolves them).
 * - Build facts (version, commit, Qt/compiler versions) are filled by the host from what its build system generated;
 *   the framework does not guess them.
 *
 * @note An empty name means "use AppConfig::name": the process identity and the about identity cannot drift apart.
 */
struct VN_APPFW_API AppInfo {
    String name;        ///< Application name; empty falls back to AppConfig::name.
    String version;     ///< Version string as the application wants it printed.
    String description; ///< One-line description.
    String copyright;   ///< Copyright line.
    String license;     ///< Licence identifier ("MIT", ...).
    String homepage;    ///< Project home page URL.
    String issues_url;  ///< Issue tracker URL.
    String credits;     ///< Acknowledgements / third-party components, as plain multi-line text.

    /// Logo or badge; empty shows no logo. A reader resolves it as a file path or a Qt resource path.
    std::filesystem::path logo;

    /// Facts shown as "label: value" rows (Qt version, compiler, build kind, commit, plugin count, ...).
    std::vector<AppInfoRow> build_facts;

    /// Extra links shown under the facts.
    std::vector<AppInfoLink> links;
};

VN_APPFW_NS_END
