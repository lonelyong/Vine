#include "TranslatorRegistry.hpp"

#include <memory>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QString>
#include <QStringList>
#include <QTranslator>

#include <vine/logging/Log.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// Context of the framework's own UI text; also the file-name prefix of its catalog.
constexpr char kFrameworkCatalogId[] = "appfw";

QString toQString(const String& text)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

QString toQString(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

/// One installed catalog and who asked for it (an empty owner means the application itself).
struct InstalledCatalog {
    String                      owner;
    std::unique_ptr<QTranslator> translator;
};

/// Installed catalogs, in install order. A function-local static: there is no initialization order to get wrong, and
/// the translators outlive the application object, which QTranslator allows (it unregisters itself).
std::vector<InstalledCatalog>& catalogs()
{
    static std::vector<InstalledCatalog> s_catalogs;
    return s_catalogs;
}

/// The tag this run was installed with; empty before installApplicationCatalogs().
String& languageTag()
{
    static String s_tag;
    return s_tag;
}

/// `<id>_<locale>.qm`: one naming rule for the framework, the host and every plugin.
QString catalogFileName(const QString& id, const QString& locale)
{
    return id + QLatin1Char('_') + locale + QStringLiteral(".qm");
}

/**
 * @brief Loads the first candidate that exists and installs it on the application.
 *
 * @details
 * - Candidates are full file names, so a resource path and a filesystem path are the same kind of thing here.
 * - A catalog that cannot be loaded is not an error: the source strings (Chinese) stay visible.
 *
 * @param owner      Who asked for the catalog (empty: the application).
 * @param candidates File names to try, in order.
 * @return true when a catalog was installed.
 */
bool installFirst(const String& owner, const QStringList& candidates)
{
    auto translator = std::make_unique<QTranslator>();
    for (const QString& file : candidates) {
        if (translator->load(file)) {
            QCoreApplication::installTranslator(translator.get());
            catalogs().push_back(InstalledCatalog{ owner, std::move(translator) });
            return true;
        }
    }
    return false;
}

} // namespace

void installApplicationCatalogs(const String& language_tag)
{
    if (language_tag.empty()) {
        return;
    }

    languageTag() = language_tag;
    const QString locale = toQString(language_tag);

    // Qt's own text first. English is Qt's source language: there is no qtbase_en.qm, and the empty meta catalog that
    // ships as qt_en.qm would only add a translator that matches nothing - so nothing is installed for it.
    // `qtbase_<locale>` first, `qt_<locale>` second: the module catalog is the one that carries Core/Gui/Widgets text
    // (what our dialogs show) and whose presence is unambiguous, while `qt_<locale>` is Qt's meta catalog - it changes
    // nothing when the module catalogs are missing, which is exactly what a windeployqt deployment looks like.
    // Two directories as well: the Qt installation's translations directory, and the one beside the executable (that is
    // where a deployed build keeps them).
    QStringList qt_candidates;
    const QStringList qt_directories{ QLibraryInfo::path(QLibraryInfo::TranslationsPath),
                                      QCoreApplication::applicationDirPath() + QLatin1String("/translations") };
    if (locale != QLatin1String("en")) {
        for (const QString& directory : qt_directories) {
            if (directory.isEmpty()) {
                continue;
            }
            qt_candidates << directory + QLatin1Char('/') + catalogFileName(QStringLiteral("qtbase"), locale);
            qt_candidates << directory + QLatin1Char('/') + catalogFileName(QStringLiteral("qt"), locale);
        }
    }
    static_cast<void>(installFirst(String(), qt_candidates));

    // Ours: a file beside the framework module (a translation fix without a release) wins over the copy embedded in
    // the module (the default, so an install tree carries no loose .qm).
    QStringList candidates;
    candidates << QCoreApplication::applicationDirPath() + QLatin1Char('/') +
                      catalogFileName(QString::fromLatin1(kFrameworkCatalogId), locale);
    candidates << QStringLiteral(":/i18n/") + catalogFileName(QString::fromLatin1(kFrameworkCatalogId), locale);
    const bool framework = installFirst(String(), candidates);

    VN_LOGI("UI language '{}': {} catalog(s) installed, framework catalog {}, Qt translations '{}'",
            language_tag.as_std_str(), catalogs().size(), framework ? "loaded" : "absent",
            QLibraryInfo::path(QLibraryInfo::TranslationsPath).toStdString());
}

bool installPluginCatalog(const String& plugin_id, const std::filesystem::path& module_path)
{
    const QString locale = toQString(languageTag());
    if (plugin_id.empty() || locale.isEmpty()) {
        return false;
    }

    const QString file = catalogFileName(toQString(plugin_id), locale);

    QStringList candidates;
    const std::filesystem::path directory = module_path.parent_path();
    if (!directory.empty()) {
        // Beside the module itself: no "translations" folder to remember, one rule for every module.
        candidates << toQString(directory) + QLatin1Char('/') + file;
    }
    candidates << QStringLiteral(":/i18n/") + file;

    return installFirst(plugin_id, candidates);
}

void removePluginCatalog(const String& plugin_id)
{
    auto& list = catalogs();
    for (auto it = list.begin(); it != list.end();) {
        if (it->owner == plugin_id) {
            QCoreApplication::removeTranslator(it->translator.get());
            it = list.erase(it);
        }
        else {
            ++it;
        }
    }
}

VN_APPFW_NS_END
