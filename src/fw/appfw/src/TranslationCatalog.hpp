#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <filesystem>
#include <memory>

#include <vine/String.hpp>

class QTranslator;

VN_APPFW_NS_BEGIN

/// Context of the framework's own UI text; it is also the file-name prefix of the framework catalog.
inline constexpr char kFrameworkContext[] = "appfw";

/**
 * @brief One translation catalog installed on the application: the value owns its translator for as long as it lives.
 *
 * @details
 * - `QCoreApplication` keeps a pointer to a translator, not the translator: this type is the owner, so installing is
 *   construction and removing is destruction - there is no separate "uninstall" step to forget (R3).
 * - A default-constructed value has installed nothing; the three factories are the only ways to get a usable one, and
 *   each returns an empty value when no catalog exists, which is the normal state and not an error (C3).
 * - Move-only: two values never own the same `QTranslator`, so the removal happens exactly once.
 * - Looking text up stays `translate()`'s job; this type only decides which `.qm` is active.
 *
 * @note Ownership follows whoever installs: `Application` holds the application's two catalogs (Qt's own text and the
 *       framework's), and the plugin loader holds one per loaded plugin. There is no process-wide registry to go stale.
 */
class TranslationCatalog
{
  public:
    TranslationCatalog();
    TranslationCatalog(TranslationCatalog&& other) noexcept;
    TranslationCatalog& operator=(TranslationCatalog&& other) noexcept;
    ~TranslationCatalog();

    TranslationCatalog(const TranslationCatalog&)            = delete;
    TranslationCatalog& operator=(const TranslationCatalog&) = delete;

    /**
     * @brief Installs Qt's own catalogs for @p language_tag: the text of `QMessageBox`, `QFileDialog` and the line-edit
     *        context menu.
     *
     * @details
     * - Lookup order: `qtbase_<locale>.qm` then `qt_<locale>.qm`, in the Qt installation's translations directory and
     *   then beside the executable (`<exe>/translations`), which is where a deployed build keeps them.
     * - English is Qt's source language: there is no `qtbase_en.qm`, and the empty meta catalog `qt_en.qm` would only add
     *   a translator that matches nothing - so nothing is installed for it.
     *
     * @param language_tag Resolved tag (`"en"` or `"zh_CN"`).
     * @return The installed catalog, or an empty one when Qt ships none for the tag.
     */
    static TranslationCatalog forQt(const String& language_tag);

    /**
     * @brief Installs the framework's own catalog for @p language_tag.
     *
     * @details
     * - One rule for every module: this is forModule() applied to the executable, with the framework's context.
     * - The file wins so a translation can be fixed without a release; the embedded copy is the default, so an install
     *   tree carries no loose `.qm`. Neither being there is not an error: the source strings stay visible.
     *
     * @param language_tag Resolved tag (`"en"` or `"zh_CN"`).
     * @return The installed catalog, or an empty one when the tag has none.
     */
    static TranslationCatalog forFramework(const String& language_tag);

    /**
     * @brief Installs the catalog of one module - a plugin library, or the host executable - for @p language_tag.
     *
     * @details
     * - The module id is the `tr()` context and the file-name prefix, so no directory has to be scanned: the caller
     *   already knows which module it is installing for (C6).
     * - Lookup order: `<module directory>/<context>_<locale>.qm`, then the copy embedded in the module
     *   (`:/i18n/<context>_<locale>.qm`). A plugin catalog is installed before the plugin's code runs: the module is
     *   already in memory then, so its embedded resources are registered.
     *
     * @param context      Module id: the `tr()` context and the file-name prefix.
     * @param language_tag Resolved tag (`"en"` or `"zh_CN"`).
     * @param module_file  Library - or executable - the catalog belongs to; its directory is looked in first.
     * @return The installed catalog, or an empty one when the module has none.
     */
    static TranslationCatalog forModule(const String& context, const String& language_tag, const std::filesystem::path& module_file);

    /**
     * @brief Reports whether a catalog is installed.
     *
     * @return true when this value holds an installed catalog, false for a default-constructed or failed one.
     */
    bool isInstalled() const noexcept;

  private:
    /**
     * @brief Takes ownership of a translator that the caller has already installed.
     *
     * @param translator Installed translator; an empty pointer produces an empty catalog.
     */
    explicit TranslationCatalog(std::unique_ptr<QTranslator> translator);

    std::unique_ptr<QTranslator> translator_;
};

VN_APPFW_NS_END
