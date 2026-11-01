#pragma once

#include <filesystem>

#include <vine/String.hpp>
#include <vine/appfw/appfw_global.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Installs Qt's own catalogs and the framework's for the given tag.
 *
 * @details
 * - Qt's catalog comes first: the texts of `QMessageBox`, `QFileDialog` and the line-edit context menu are Qt's, and
 *   only Qt's translator translates them.
 * - Our own catalog is looked up beside the framework module, then in the module's embedded resources; neither being
 *   there is not an error (the source strings are Chinese and stay visible).
 * - Called once, from `Application::initialize()`, before any window exists.
 *
 * @param language_tag Resolved tag (`"en"` or `"zh_CN"`).
 */
void installApplicationCatalogs(const String& language_tag);

/**
 * @brief Installs the catalog of one plugin module, before any of its code runs.
 *
 * @details
 * - The plugin id is the `tr()` context and the file-name prefix, so no directory has to be scanned: the loader
 *   already knows which plugins it is loading.
 * - Lookup order: `<module directory>/<id>_<locale>.qm`, then `:/i18n/<id>_<locale>.qm` (embedded in the module).
 * - A plugin without a catalog is the normal case and returns false.
 *
 * @param plugin_id   Plugin id (`PluginInfo::name`).
 * @param module_path Library the plugin was loaded from; empty skips the file candidate.
 * @return true when a catalog was installed.
 */
bool installPluginCatalog(const String& plugin_id, const std::filesystem::path& module_path);

/**
 * @brief Removes the catalogs installed for one plugin.
 *
 * @details
 * - Called when the plugin is unloaded, so its translations stop affecting other layers once it is gone.
 * - A plugin that had no catalog is ignored.
 *
 * @param plugin_id Plugin id.
 */
void removePluginCatalog(const String& plugin_id);

VN_APPFW_NS_END
