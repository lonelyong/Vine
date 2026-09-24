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
    General,    ///< 通用
    Appearance, ///< 外观
    Logging,    ///< 日志
    Files,      ///< 文件
    Editor,     ///< 编辑器
    Rendering,  ///< 渲染
    Plugins,    ///< 插件
};

/**
 * @brief Standard configuration groups.
 *
 * Well-known groups inside standard categories. Like categories, each maps to
 * a canonical name (the second segment of an item key) and a display label.
 */
enum class StandardGroup
{
    Startup,  ///< 启动
    Behavior, ///< 行为
    Theme,    ///< 主题
    Language, ///< 语言
    Console,  ///< 控制台
    File,     ///< 文件
    Autosave, ///< 自动保存
    Paths,    ///< 路径
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
 * @param id Standard category.
 * @return The display label.
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
 * @param id Standard group.
 * @return The display label.
 */
VN_APPFW_API String standardGroupLabel(StandardGroup id);

VN_APPFW_NS_END
