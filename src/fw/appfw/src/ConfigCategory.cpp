#include <vine/appfw/ConfigCategory.hpp>

#include <algorithm>
#include <memory>
#include <utility>

#include <vine/appfw/ConfigGroup.hpp>

VN_APPFW_NS_BEGIN

struct ConfigCategory::Impl {
    ConfigRegistry* owner = nullptr;
    String          name;
    String          label;
    String          description;
    int             order = 0;

    std::vector<std::unique_ptr<ConfigGroup>> groups;
};

ConfigCategory::ConfigCategory(String name, ConfigRegistry* owner)
  : d(new Impl)
{
    d->name  = std::move(name);
    d->owner = owner;
}

ConfigCategory::~ConfigCategory() = default;

ConfigGroup* ConfigCategory::addGroup(String name)
{
    if (group(name) != nullptr)
        return nullptr; // Duplicate name, reject
    d->groups.push_back(std::unique_ptr<ConfigGroup>(new ConfigGroup(std::move(name), d->owner)));
    return d->groups.back().get();
}

ConfigGroup* ConfigCategory::getOrAddGroup(String name)
{
    if (ConfigGroup* existing = group(name))
        return existing;
    d->groups.push_back(std::unique_ptr<ConfigGroup>(new ConfigGroup(std::move(name), d->owner)));
    return d->groups.back().get();
}

bool ConfigCategory::removeGroup(const String& name)
{
    for (auto it = d->groups.begin(); it != d->groups.end(); ++it) {
        if ((*it)->name() == name) {
            d->groups.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<ConfigGroup*> ConfigCategory::groups() const
{
    std::vector<ConfigGroup*> out;
    out.reserve(d->groups.size());
    for (const auto& g : d->groups) out.push_back(g.get());
    std::stable_sort(out.begin(), out.end(), [](const ConfigGroup* a, const ConfigGroup* b) { return a->order() < b->order(); });
    return out;
}

ConfigGroup* ConfigCategory::group(const String& name) const
{
    for (const auto& g : d->groups) {
        if (g->name() == name)
            return g.get();
    }
    return nullptr;
}

const String& ConfigCategory::name() const
{
    return d->name;
}

const String& ConfigCategory::label() const
{
    return d->label;
}

const String& ConfigCategory::description() const
{
    return d->description;
}

int ConfigCategory::order() const
{
    return d->order;
}

ConfigCategory& ConfigCategory::label(const String& v)
{
    d->label = v;
    return *this;
}

ConfigCategory& ConfigCategory::description(const String& v)
{
    d->description = v;
    return *this;
}

ConfigCategory& ConfigCategory::order(int v)
{
    d->order = v;
    return *this;
}

VN_APPFW_NS_END
