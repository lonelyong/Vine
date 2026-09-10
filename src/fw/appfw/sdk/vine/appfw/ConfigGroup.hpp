#pragma once

#include "appfw_global.hpp"

#include <memory>
#include <vector>

#include <vine/String.hpp>

#include "ConfigItem.hpp"

V_APPFW_NS_BEGIN

class ConfigRegistry;

/**
 * @brief Configuration group: a QGroupBox inside a tab of the configuration window.
 *
 * Created and owned by ConfigCategory::addGroup(); may carry label, description
 * and order metadata and host items (ConfigItem). Qt-free, non-copyable
 * (owned by the category).
 *
 * @note Lifetime: the returned pointers stay valid until their node is removed
 * (removeItem(), or removeGroup()/removeCategory()/clear() above it).
 * @note Threading: not synchronized; register before worker threads read the tree.
 */
class V_APPFW_API ConfigGroup {
  public:
    ConfigGroup(const ConfigGroup&)            = delete;
    ConfigGroup& operator=(const ConfigGroup&) = delete;
    ~ConfigGroup();

    /**
     * @brief Adds an item.
     *
     * @param item Item descriptor.
     * @return true if added, false if the key already exists in the registry.
     */
    bool addItem(const ConfigItem& item);
    /**
     * @brief Removes the item with the given key.
     *
     * @param key Item key.
     * @return true if removed, false if not found.
     */
    bool removeItem(const String& key);
    /**
     * @brief All items (insertion order).
     */
    std::vector<const ConfigItem*> items() const;
    /**
     * @brief Looks up an item by key.
     *
     * @param key Item key.
     * @return The item, or nullptr if not found.
     */
    const ConfigItem* item(const String& key) const;

    /**
     * @brief Unique name (given at addGroup time).
     */
    const String& name() const;
    /**
     * @brief Group title; falls back to name() when unset.
     */
    const String& label() const;
    /**
     * @brief Group tooltip.
     */
    const String& description() const;
    /**
     * @brief Sort weight (smaller comes first; default 0 = insertion order).
     */
    int order() const;

    /**
     * @brief Sets the group title.
     */
    ConfigGroup& label(const String& v);
    /**
     * @brief Sets the group tooltip.
     */
    ConfigGroup& description(const String& v);
    /**
     * @brief Sets the sort weight.
     */
    ConfigGroup& order(int v);

  private:
    ConfigGroup(String name, ConfigRegistry* owner);
    friend class ConfigCategory;

    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
