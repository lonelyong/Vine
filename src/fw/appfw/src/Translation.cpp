#include <vine/appfw/Translation.hpp>

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

/// Context of the framework's own UI text; it is also the file-name prefix of the framework catalog.
constexpr char kFrameworkContext[] = "appfw";

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

/// The tag the system locale maps to. Chinese is one catalog, so every other language takes the English one.
String systemLanguageTag()
{
    return QLocale::system().language() == QLocale::English ? String(u8"en") : String(u8"zh_CN");
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

const String& languageConfigKey()
{
    static const String s_key(u8"general.language");
    return s_key;
}

ConfigItem languageConfigItem()
{
    using Value = std::variant<int, double, String>;

    ConfigItem item(languageConfigKey(), translate(kFrameworkContext, "界面语言"), ConfigItemType::Choice);
    item.description(translate(kFrameworkContext, "选择界面语言；改动在下次启动时生效"))
        .choices({ { Value(String(kLanguageSystem)), translate(kFrameworkContext, "跟随系统") },
                   { Value(String(u8"zh_CN")), translate(kFrameworkContext, "中文") },
                   { Value(String(u8"en")), translate(kFrameworkContext, "English") } })
        .defaultValue(kLanguageSystem);
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
