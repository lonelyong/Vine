#include "TranslationCatalog.hpp"

#include <string>
#include <utility>

#include <QByteArray>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QString>
#include <QStringList>
#include <QTranslator>

VN_APPFW_NS_BEGIN

namespace
{

QString toQString(const String& text)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

QString toQString(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

/// Through the UTF-8 bytes, so a non-ASCII install path is not mangled by the platform's narrow code page.
std::filesystem::path toPath(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())));
}

/// `<context>_<locale>.qm`: one naming rule for Qt, the framework and every plugin.
QString catalogFileName(const QString& context, const QString& locale)
{
    return context + QLatin1Char('_') + locale + QStringLiteral(".qm");
}

/// The places one module's catalog can be: beside the module first, then inside the module's own resources.
QStringList moduleCandidates(const QString& context, const QString& locale, const QString& directory)
{
    const QString file = catalogFileName(context, locale);
    QStringList   candidates;
    if (!directory.isEmpty()) {
        candidates << directory + QLatin1Char('/') + file;
    }
    candidates << QStringLiteral(":/i18n/") + file;
    return candidates;
}

/**
 * @brief Loads the first candidate that exists and installs it on the application.
 *
 * @details
 * - Candidates are full file names, so a resource path and a filesystem path are the same kind of thing here.
 * - A catalog that cannot be loaded is not an error: the source strings (Chinese) stay visible.
 *
 * @param candidates File names to try, in order.
 * @return The installed translator, or an empty pointer when none of the candidates loads.
 */
std::unique_ptr<QTranslator> installFirst(const QStringList& candidates)
{
    auto translator = std::make_unique<QTranslator>();
    for (const QString& file : candidates) {
        if (translator->load(file)) {
            QCoreApplication::installTranslator(translator.get());
            return translator;
        }
    }
    return nullptr;
}

} // namespace

TranslationCatalog::TranslationCatalog() = default;

TranslationCatalog::TranslationCatalog(std::unique_ptr<QTranslator> translator)
  : translator_(std::move(translator))
{}

TranslationCatalog::TranslationCatalog(TranslationCatalog&& other) noexcept = default;

/// Destroying the translator is what removes it from the application (QTranslator unregisters itself), so a move
/// assignment first drops the catalog this value used to hold and only then takes the other one.
TranslationCatalog& TranslationCatalog::operator=(TranslationCatalog&& other) noexcept = default;

TranslationCatalog::~TranslationCatalog() = default;

TranslationCatalog TranslationCatalog::forQt(const String& language_tag)
{
    const QString locale = toQString(language_tag);
    if (locale.isEmpty() || locale == QLatin1String("en")) {
        return TranslationCatalog();
    }

    // Two directories as well: the Qt installation's translations directory, and the one beside the executable (that is
    // where a deployed build keeps them).
    QStringList       candidates;
    const QStringList directories{ QLibraryInfo::path(QLibraryInfo::TranslationsPath),
                                   QCoreApplication::applicationDirPath() + QLatin1String("/translations") };
    for (const QString& directory : directories) {
        if (directory.isEmpty()) {
            continue;
        }
        // `qtbase_<locale>` first: that is the catalog carrying Core/Gui/Widgets text (what our dialogs show) and whose
        // presence is unambiguous. `qt_<locale>` is Qt's meta catalog - it changes nothing when the module catalogs are
        // missing, which is exactly what a windeployqt deployment looks like (P8).
        candidates << directory + QLatin1Char('/') + catalogFileName(QStringLiteral("qtbase"), locale);
        candidates << directory + QLatin1Char('/') + catalogFileName(QStringLiteral("qt"), locale);
    }
    return TranslationCatalog(installFirst(candidates));
}

TranslationCatalog TranslationCatalog::forFramework(const String& language_tag)
{
    // The framework's catalog follows the same rule as any module's: beside the binary, then inside it. The binary here
    // is the executable, because that is the path a deployed tree can rely on for the framework module as well.
    return forModule(String::fromUtf8(kFrameworkContext), language_tag, toPath(QCoreApplication::applicationFilePath()));
}

TranslationCatalog TranslationCatalog::forModule(const String& context, const String& language_tag, const std::filesystem::path& module_file)
{
    if (context.empty() || language_tag.empty()) {
        return TranslationCatalog();
    }

    // Beside the module itself: no "translations" folder to remember, one rule for every module.
    const std::filesystem::path directory = module_file.parent_path();
    return TranslationCatalog(installFirst(moduleCandidates(toQString(context), toQString(language_tag), toQString(directory))));
}

bool TranslationCatalog::isInstalled() const noexcept
{
    return translator_ != nullptr;
}

VN_APPFW_NS_END
