#include <vine/appfw/ConfigGroup.hpp>

#include <memory>
#include <utility>

#include <vine/appfw/ConfigRegistry.hpp>

VN_APPFW_NS_BEGIN

struct ConfigGroup::Impl {
    ConfigRegistry* owner = nullptr;
    String          name;
    String          label;
    String          description;
    int             order = 0;

    std::vector<std::unique_ptr<ConfigItem>> items;
};

ConfigGroup::ConfigGroup(String name, ConfigRegistry* owner)
  : d(new Impl)
{
    d->name  = std::move(name);
    d->owner = owner;
}

ConfigGroup::~ConfigGroup() = default;

bool ConfigGroup::addItem(const ConfigItem& item)
{
    if (d->owner != nullptr && d->owner->item(item.key()) != nullptr)
        return false; // Key already exists in the registry, reject
    d->items.push_back(std::make_unique<ConfigItem>(item));
    return true;
}

bool ConfigGroup::removeItem(const String& key)
{
    for (auto it = d->items.begin(); it != d->items.end(); ++it) {
        if ((*it)->key() == key) {
            d->items.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<const ConfigItem*> ConfigGroup::items() const
{
    std::vector<const ConfigItem*> out;
    out.reserve(d->items.size());
    for (const auto& i : d->items) out.push_back(i.get());
    return out;
}

const ConfigItem* ConfigGroup::item(const String& key) const
{
    for (const auto& i : d->items) {
        if (i->key() == key)
            return i.get();
    }
    return nullptr;
}

const String& ConfigGroup::name() const
{
    return d->name;
}

const String& ConfigGroup::label() const
{
    return d->label;
}

const String& ConfigGroup::description() const
{
    return d->description;
}

int ConfigGroup::order() const
{
    return d->order;
}

ConfigGroup& ConfigGroup::label(const String& v)
{
    d->label = v;
    return *this;
}

ConfigGroup& ConfigGroup::description(const String& v)
{
    d->description = v;
    return *this;
}

ConfigGroup& ConfigGroup::order(int v)
{
    d->order = v;
    return *this;
}

VN_APPFW_NS_END
