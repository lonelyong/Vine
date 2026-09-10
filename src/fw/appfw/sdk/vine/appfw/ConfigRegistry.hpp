#pragma once

#include "appfw_global.hpp"

#include <memory>
#include <vector>

#include <vine/String.hpp>

#include "ConfigCategory.hpp"
#include "ConfigStandard.hpp"

V_APPFW_NS_BEGIN

/**
 * @brief Configuration registry: plugins build a "category -> group -> item"
 * display tree here.
 *
 * Stores metadata only, not values (values live in ConfigManager); the tree
 * structure is the display order and it also provides whole-tree queries.
 * Qt-free.
 *
 * @note Lifetime: the query functions return pointers into the tree, which stay
 * valid until their node is removed (removeCategory/removeGroup/removeItem/clear).
 * @note Threading: the registry is not synchronized. Register during startup and
 * plugin loading, before worker threads read the tree.
 */
class V_APPFW_API ConfigRegistry {
  public:
    ConfigRegistry();
    ~ConfigRegistry();

    ConfigRegistry(const ConfigRegistry&)            = delete;
    ConfigRegistry& operator=(const ConfigRegistry&) = delete;

    /**
     * @brief Adds a category.
     *
     * @param name Unique category name.
     * @return The new category, or nullptr if a category with the same name exists.
     */
    ConfigCategory* addCategory(String name);

    /**
     * @brief Returns the category with the given name, creating it if absent.
     *
     * Unlike addCategory() (which rejects duplicates), this never fails:
     * repeated calls with the same name share one category node.
     *
     * @param name Category name.
     * @return The existing or newly created category.
     */
    ConfigCategory* getOrAddCategory(String name);

    /**
     * @brief Returns the standard category, creating it with its canonical
     * name, label and order on first access.
     *
     * @param id Standard category.
     * @return The standard category.
     */
    ConfigCategory* standardCategory(StandardCategory id);

    /**
     * @brief Returns the standard group inside a standard category, creating
     * it with its canonical name and label on first access.
     *
     * @param cat Standard category.
     * @param grp Standard group.
     * @return The standard group, or nullptr if the category does not exist.
     */
    ConfigGroup* standardGroup(StandardCategory cat, StandardGroup grp);

    /**
     * @brief Adds an item under a standard category/group and records its owner.
     *
     * @param cat Standard category.
     * @param grp Standard group.
     * @param item Item descriptor; its key must already be namespaced.
     * @param owner Plugin name owning the item; empty records no owner and
     * drops an owner recorded for the same key earlier.
     * @return true if added, false if the key already exists.
     */
    bool addItem(StandardCategory cat, StandardGroup grp, const ConfigItem& item, String owner = {});

    /**
     * @brief Returns all items owned by the given plugin.
     *
     * @param plugin_name Plugin name.
     * @return The plugin's registered items, ordered by key.
     */
    std::vector<const ConfigItem*> itemsForPlugin(const String& plugin_name) const;

    /**
     * @brief Removes all items owned by the given plugin.
     *
     * @param plugin_name Plugin name.
     * @return true if at least one item was removed.
     */
    bool removeItemsForPlugin(const String& plugin_name);

    /**
     * @brief Removes the category with the given name.
     *
     * @param name Category name.
     * @return true if removed, false if not found.
     */
    bool removeCategory(const String& name);
    /**
     * @brief Clears the whole tree.
     */
    void clear();

    /**
     * @brief All categories (display order, stable-sorted by order).
     */
    std::vector<ConfigCategory*> categories() const;
    /**
     * @brief Looks up a category by name.
     *
     * @param name Category name.
     * @return The category, or nullptr if not found.
     */
    ConfigCategory* category(const String& name) const;

    /**
     * @brief Total number of items (sum over all groups in the tree).
     */
    int itemCount() const;
    /**
     * @brief Looks up an item by key across the whole tree.
     *
     * @param key Item key.
     * @return The item, or nullptr if not found.
     */
    const ConfigItem* item(const String& key) const;
    /**
     * @brief Removes the item with the given key across the whole tree.
     *
     * @param key Item key.
     * @return true if removed, false if not found.
     */
    bool removeItem(const String& key);

  private:
    /**
     * @brief Drops owner records whose item is no longer in the tree.
     *
     * The tree can be edited through any of its nodes (ConfigCategory::removeGroup,
     * ConfigGroup::removeItem, ...), so the owner map is treated as a cache that is
     * also validated whenever it is read.
     */
    void pruneOwners();

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
