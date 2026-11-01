#pragma once

#include "appfw_global.hpp"

#include <vine/String.hpp>

#include "ConfigItem.hpp"

VN_APPFW_NS_BEGIN

/**
 * @brief Value of the language setting that means "follow the system locale".
 */
inline constexpr const char8_t kLanguageSystem[] = u8"system";

/**
 * @brief Translates UI text that belongs to a context.
 *
 * @details
 * - The source string is the Chinese text written at the call site; the catalog holds translations of it.
 * - @p context names the owner of the text: `"appfw"` for the framework's own UI, the plugin id for a plugin. It is
 *   also the file-name prefix of the catalog (`<context>_<locale>.qm`), so it must not be invented per call.
 * - The signature is `QCoreApplication::translate()`'s on purpose: the translation tool extracts calls by that name, so
 *   a custom wrapper would silently stop feeding the catalogs (see P9 in the design doc).
 * - The lookup never reads a file, never throws and never logs: a missing catalog, a missing entry or a corrupt
 *   catalog all return @p source unchanged.
 *
 * @note This is the UI layer's text only; logs and exception messages stay English and never pass through here.
 * @note Inside a `QObject` subclass, `tr()` finds `QObject::tr` first. Give the class `Q_DECLARE_TR_FUNCTIONS(appfw)`
 *       instead of qualifying every call (see P7).
 *
 * @param context Owner of the text: `"appfw"` for the framework's own UI, the plugin id for a plugin. It is ASCII
 *                (an id, never user text), which is why it is a narrow literal like the source.
 * @param source  Chinese source text; a narrow UTF-8 literal (the sources are compiled with /utf-8), so the translation
 *                tool reads exactly the bytes it extracts.
 * @return The translation, or @p source when none is loaded.
 */
VN_APPFW_API String translate(const char* context, const char* source);

/**
 * @brief Returns the key of the configuration item that holds the UI language.
 *
 * @details
 * - The key is `general.language`: the item sits in the standard "通用 / 语言" node of the configuration window.
 * - The stored value is a language tag or kLanguageSystem, not a display label.
 *
 * @return The configuration key.
 */
VN_APPFW_API const String& languageConfigKey();

/**
 * @brief Returns the UI-language configuration item.
 *
 * @details
 * - A Choice item with three entries: follow the system locale, Chinese, English.
 * - The default is kLanguageSystem, so an untouched configuration keeps following the system.
 * - Labels are produced through tr(), so the item has to be registered after the language was applied, otherwise
 *   the first start would show them in the wrong language.
 *
 * @return The item descriptor (metadata only; the value lives in ConfigManager).
 */
VN_APPFW_API ConfigItem languageConfigItem();

/**
 * @brief Resolves a language setting to one of the supported tags.
 *
 * @details
 * - Supported tags are `"en"` and `"zh_CN"`; that is the whole set, so every other input resolves to one of them.
 * - kLanguageSystem (and an empty setting) resolve to the system locale's choice.
 * - A tag is matched on its language part, so `"en_US"` and `"en"` resolve alike, and `"zh"`, `"zh_CN"`, `"zh_TW"`
 *   all resolve to `"zh_CN"` (Chinese is one catalog).
 *
 * @param setting The raw setting: an item value, or kLanguageSystem for "follow the system locale".
 * @return `"en"` or `"zh_CN"`.
 */
VN_APPFW_API String resolveLanguageTag(const String& setting);

VN_APPFW_NS_END
