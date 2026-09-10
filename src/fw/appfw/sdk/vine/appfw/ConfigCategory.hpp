#pragma once

#include "appfw_global.hpp"

#include <memory>
#include <vector>

#include <vine/String.hpp>

#include "ConfigGroup.hpp"

V_APPFW_NS_BEGIN

class ConfigRegistry;

/**
 * @brief Configuration category: corresponds to a tab in the configuration window.
 *
 * Created and owned by ConfigRegistry::addCategory(); may carry label, description
 * and order metadata and host groups (ConfigGroup). Qt-free, non-copyable
 * (owned by the registry).
 *
 * @note Lifetime: the returned pointers stay valid until their node is removed
 * (removeGroup(), or removeCategory()/clear() on the registry).
 * @note Threading: not synchronized; register before worker threads read the tree.
 */
class V_APPFW_API ConfigCategory {
  public:
    ConfigCategory(const ConfigCategory&)            = delete;
    ConfigCategory& operator=(const ConfigCategory&) = delete;
    ~ConfigCategory();

    /**
     * @brief Adds a group.
     *
     * @param name Unique group name.
     * @return The new group, or nullptr if a group with the same name exists.
     */
    ConfigGroup* addGroup(String name);

    /**
     * @brief Returns the group with the given name, creating it if absent.
     *
     * Unlike addGroup() (which rejects duplicates), this never fails: repeated
     * calls with the same name share one group node.
     *
     * @param name Group name.
     * @return The existing or newly created group.
     */
    ConfigGroup* getOrAddGroup(String name);
    /**
     * @brief Removes the group with the given name.
     *
     * @param name Group name.
     * @return true if removed, false if not found.
     */
    bool removeGroup(const String& name);
    /**
     * @brief All groups (display order, stable-sorted by order).
     */
    std::vector<ConfigGroup*> groups() const;
    /**
     * @brief Looks up a group by name.
     *
     * @param name Group name.
     * @return The group, or nullptr if not found.
     */
    ConfigGroup* group(const String& name) const;

    /**
     * @brief Unique name (given at addGroup time).
     */
    const String& name() const;
    /**
     * @brief Tab title; falls back to name() when unset ("General" if name is empty).
     */
    const String& label() const;
    /**
     * @brief Tab tooltip.
     */
    const String& description() const;
    /**
     * @brief Sort weight (smaller comes first; default 0 = insertion order).
     */
    int order() const;

    /**
     * @brief Sets the tab title.
     */
    ConfigCategory& label(const String& v);
    /**
     * @brief Sets the tab tooltip.
     */
    ConfigCategory& description(const String& v);
    /**
     * @brief Sets the sort weight.
     */
    ConfigCategory& order(int v);

  private:
    ConfigCategory(String name, ConfigRegistry* owner);
    friend class ConfigRegistry;

    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
