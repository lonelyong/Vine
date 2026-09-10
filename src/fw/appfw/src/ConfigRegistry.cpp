#include <vine/appfw/ConfigRegistry.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

V_APPFW_NS_BEGIN

struct ConfigRegistry::Impl {
    std::vector<std::unique_ptr<ConfigCategory>> categories;
    std::map<String, String>                     owners; // item key -> owning plugin name
};

ConfigRegistry::ConfigRegistry()
  : d(new Impl)
{}

ConfigRegistry::~ConfigRegistry() = default;

ConfigCategory* ConfigRegistry::addCategory(String name)
{
    if (category(name) != nullptr)
        return nullptr; // Duplicate name, reject
    d->categories.push_back(std::unique_ptr<ConfigCategory>(new ConfigCategory(std::move(name), this)));
    return d->categories.back().get();
}

ConfigCategory* ConfigRegistry::getOrAddCategory(String name)
{
    if (ConfigCategory* existing = category(name))
        return existing;
    d->categories.push_back(std::unique_ptr<ConfigCategory>(new ConfigCategory(std::move(name), this)));
    return d->categories.back().get();
}

ConfigCategory* ConfigRegistry::standardCategory(StandardCategory id)
{
    ConfigCategory* cat = getOrAddCategory(standardCategoryName(id));
    if (cat) {
        cat->label(standardCategoryLabel(id));
        cat->order(standardCategoryOrder(id));
    }
    return cat;
}

ConfigGroup* ConfigRegistry::standardGroup(StandardCategory cat_id, StandardGroup grp_id)
{
    ConfigCategory* cat = standardCategory(cat_id);
    if (!cat)
        return nullptr;
    ConfigGroup* grp = cat->getOrAddGroup(standardGroupName(grp_id));
    if (grp)
        grp->label(standardGroupLabel(grp_id));
    return grp;
}

bool ConfigRegistry::addItem(StandardCategory cat, StandardGroup grp, const ConfigItem& item, String owner)
{
    ConfigGroup* g = standardGroup(cat, grp);
    if (!g || !g->addItem(item))
        return false;
    // The owner map mirrors the tree, but only this call can record an owner:
    // registering a key without one must not leave the previous owner behind,
    // or an unrelated plugin would appear to own it.
    if (owner.empty())
        d->owners.erase(item.key());
    else
        d->owners[item.key()] = std::move(owner);
    return true;
}

void ConfigRegistry::pruneOwners()
{
    for (auto it = d->owners.begin(); it != d->owners.end();) {
        if (item(it->first) != nullptr)
            ++it;
        else
            it = d->owners.erase(it);
    }
}

std::vector<const ConfigItem*> ConfigRegistry::itemsForPlugin(const String& plugin_name) const
{
    std::vector<const ConfigItem*> out;
    for (const auto& [key, owner] : d->owners) {
        if (owner == plugin_name) {
            if (const ConfigItem* it = item(key))
                out.push_back(it);
        }
    }
    return out;
}

bool ConfigRegistry::removeItemsForPlugin(const String& plugin_name)
{
    std::vector<String> keys;
    for (const auto& [key, owner] : d->owners) {
        if (owner == plugin_name)
            keys.push_back(key);
    }

    bool removed = false;
    for (const auto& key : keys) {
        if (removeItem(key))
            removed = true;
    }
    pruneOwners();
    return removed;
}

bool ConfigRegistry::removeCategory(const String& name)
{
    for (auto it = d->categories.begin(); it != d->categories.end(); ++it) {
        if ((*it)->name() == name) {
            d->categories.erase(it);
            pruneOwners();
            return true;
        }
    }
    return false;
}

void ConfigRegistry::clear()
{
    d->categories.clear();
    d->owners.clear();
}

std::vector<ConfigCategory*> ConfigRegistry::categories() const
{
    std::vector<ConfigCategory*> out;
    out.reserve(d->categories.size());
    for (const auto& c : d->categories) out.push_back(c.get());
    std::stable_sort(out.begin(), out.end(), [](const ConfigCategory* a, const ConfigCategory* b) { return a->order() < b->order(); });
    return out;
}

ConfigCategory* ConfigRegistry::category(const String& name) const
{
    for (const auto& c : d->categories) {
        if (c->name() == name)
            return c.get();
    }
    return nullptr;
}

int ConfigRegistry::itemCount() const
{
    int n = 0;
    for (const auto& c : d->categories) {
        for (ConfigGroup* g : c->groups()) n += static_cast<int>(g->items().size());
    }
    return n;
}

const ConfigItem* ConfigRegistry::item(const String& key) const
{
    for (const auto& c : d->categories) {
        for (ConfigGroup* g : c->groups()) {
            if (const ConfigItem* it = g->item(key))
                return it;
        }
    }
    return nullptr;
}

bool ConfigRegistry::removeItem(const String& key)
{
    for (const auto& c : d->categories) {
        for (ConfigGroup* g : c->groups()) {
            if (g->item(key) != nullptr) {
                const bool removed = g->removeItem(key);
                if (removed)
                    d->owners.erase(key);
                return removed;
            }
        }
    }
    return false;
}

V_APPFW_NS_END
