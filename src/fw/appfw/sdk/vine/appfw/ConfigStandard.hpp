#pragma once

#include "appfw_global.hpp"

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Standard configuration categories.
 *
 * Framework-owned tabs of the configuration window. Each maps to a canonical
 * (machine-facing) name — which is also the first segment of the item keys
 * registered under it — and a localized display label.
 */
enum class StandardCategory
{
    General,    // 通用
    Appearance, // 外观
    Logging,    // 日志
    Files,      // 文件
    Editor,     // 编辑器
    Rendering,  // 渲染
    Plugins,    // 插件
};

/**
 * @brief Standard configuration groups.
 *
 * Well-known groups inside standard categories. Like categories, each maps to
 * a canonical name (the second segment of an item key) and a display label.
 */
enum class StandardGroup
{
    Startup,  // 启动
    Behavior, // 行为
    Theme,    // 主题
    Language, // 语言
    Console,  // 控制台
    File,     // 文件
    Autosave, // 自动保存
    Paths,    // 路径
};

/**
 * @brief Canonical (machine-facing) name of a standard category.
 *
 * @param id Standard category.
 * @return The canonical name.
 */
VN_APPFW_API String standardCategoryName(StandardCategory id);

/**
 * @brief Localized display label of a standard category.
 *
 * @details
 * - The label is UI text, not an identifier: it is looked up in the `appfw` catalog at call time and returned in the
 *   language installed when the function runs. Use standardCategoryName() wherever a stable key is needed.
 * - The Chinese source text is returned verbatim when no catalog is installed, so a non-GUI tool or a unit test that
 *   never boots an Application sees the Chinese label rather than an empty string.
 * - ConfigRegistry::standardCategory() writes this label into the node it creates, which is why a config tree must be
 *   built after the language catalogs are installed (Application::initialize() does that before it registers anything).
 *
 * @param id Standard category.
 * @return The display label in the installed UI language.
 */
VN_APPFW_API String standardCategoryLabel(StandardCategory id);

/**
 * @brief Sort weight of a standard category.
 *
 * @param id Standard category.
 * @return The sort weight (smaller comes first).
 */
VN_APPFW_API int standardCategoryOrder(StandardCategory id);

/**
 * @brief Canonical name of a standard group.
 *
 * @param id Standard group.
 * @return The canonical name.
 */
VN_APPFW_API String standardGroupName(StandardGroup id);

/**
 * @brief Localized display label of a standard group.
 *
 * @details
 * - Same rule as standardCategoryLabel(): UI text resolved in the `appfw` catalog at call time, Chinese source text
 *   when no catalog is installed. standardGroupName() is the identity that never changes with the language.
 *
 * @param id Standard group.
 * @return The display label in the installed UI language.
 */
VN_APPFW_API String standardGroupLabel(StandardGroup id);

VN_APPFW_NS_END
