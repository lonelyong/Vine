#include <vine/appfw/Translation.hpp>

#if defined(_WIN32)
#    include <Windows.h>
#endif // _WIN32

#include <string_view>
#include <utility>
#include <variant>

#include <QByteArray>
#include <QCoreApplication>
#include <QLocale>
#include <QString>

VN_APPFW_NS_BEGIN

namespace
{

/// Language part of a tag, lower-cased: "zh_CN" -> "zh", "en-US" -> "en".
String languagePart(const String& tag)
{
    String part;
    for (size_t i = 0; i < tag.size(); ++i) {
        const char8_t ch = tag[i];
        if (ch == u8'_' || ch == u8'-') {
            break;
        }
        part.push_back(ch);
    }
    return part.toLower();
}

/// The tag the system language maps to. Chinese gets the catalog; every other language takes the English source text
/// (a system we do not translate must not be shown a third language's catalog).
String systemLanguageTag()
{
#if defined(_WIN32)
    // Ask Windows, not the environment: LANG is inherited from whatever shell launched the process (MSYS, Git Bash, a
    // VS Code terminal) and is not the language Windows itself uses. Following it makes "follow the system" disagree
    // with every native dialog on the same desktop. A user who wants the English text can still choose it explicitly.
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? String(u8"zh_CN") : String(u8"en");
#else
    // On Unix the locale environment *is* the system setting, so Qt's answer is the one to follow.
    return QLocale::system().language() == QLocale::Chinese ? String(u8"zh_CN") : String(u8"en");
#endif
}

/// Converts a QString (already in UTF-8) to a framework String.
String fromUtf8(const QByteArray& bytes)
{
    return String::fromUtf8(std::string_view(bytes.constData(), static_cast<size_t>(bytes.size())));
}

} // namespace

String translate(const char* context, const char* source)
{
    if (source == nullptr) {
        return String();
    }

    const QString text = QCoreApplication::translate(context != nullptr ? context : "", source, nullptr, -1);
    return fromUtf8(text.toUtf8());
}

String translate(const char8_t* context, const char8_t* source)
{
    return translate(reinterpret_cast<const char*>(context), reinterpret_cast<const char*>(source));
}

const String& languageConfigKey()
{
    static const String s_key(u8"general.language");
    return s_key;
}

ConfigItem languageConfigItem()
{
    using Value = std::variant<int, double, String>;

    // The context is a literal here on purpose: lupdate extracts a translate() call only when it can read the context,
    // so a constant (kFrameworkContext) would leave these texts out of the catalog while the call still compiled.
    ConfigItem item(languageConfigKey(), translate("appfw", "UI language"), ConfigItemType::Choice);
    item.description(translate("appfw", "Choose the UI language; it is loaded once at startup"))
        .choices({
          { Value(String(kLanguageSystem)), translate("appfw", "Follow the system") },
          { Value(String(u8"zh_CN")),       translate("appfw", "Chinese")           },
          { Value(String(u8"en")),          translate("appfw", "English")           }
    })
        .defaultValue(kLanguageSystem)
        .restartRequired(true);
    return item;
}

String resolveLanguageTag(const String& setting)
{
    // Empty and "system" both mean "follow the system"; an unknown tag does the same, because the supported set is
    // only Chinese and English and guessing a third language is not an option the framework has.
    if (setting.empty() || setting == String(kLanguageSystem)) {
        return systemLanguageTag();
    }

    const String language = languagePart(setting);
    if (language == u8"en") {
        return String(u8"en");
    }
    if (language == u8"zh") {
        return String(u8"zh_CN");
    }
    return systemLanguageTag();
}

VN_APPFW_NS_END
