#include <vine/appfw/ConfigItem.hpp>

#include <any>
#include <stdexcept>
#include <utility>
#include <variant>

VN_APPFW_NS_BEGIN

struct ConfigItem::Impl {
    String         key;
    String         label;
    String         description;
    ConfigItemType type = ConfigItemType::String;

    std::any default_value;

    std::any min_value;
    std::any max_value;
    std::any step_value;

    std::vector<ConfigChoice> choices;
    bool                      read_only = false;
};

ConfigItem::ConfigItem(String key, String label, ConfigItemType type)
  : d(new Impl)
{
    d->key   = std::move(key);
    d->label = std::move(label);
    d->type  = type;
}

ConfigItem::ConfigItem(const ConfigItem& other)
  : d(new Impl(*other.d))
{}

ConfigItem& ConfigItem::operator=(const ConfigItem& other)
{
    if (this != &other) {
        *d = *other.d;
    }
    return *this;
}

ConfigItem::ConfigItem(ConfigItem&& other) noexcept
  : d(std::move(other.d))
{}

ConfigItem& ConfigItem::operator=(ConfigItem&& other) noexcept
{
    if (this != &other) {
        d = std::move(other.d);
    }
    return *this;
}

ConfigItem::~ConfigItem() = default;

const String& ConfigItem::key() const
{
    return d->key;
}

const String& ConfigItem::label() const
{
    return d->label;
}

const String& ConfigItem::description() const
{
    return d->description;
}

ConfigItemType ConfigItem::type() const
{
    return d->type;
}

bool ConfigItem::hasDefault() const
{
    return d->default_value.has_value();
}

const String& ConfigItem::defaultString() const
{
    return std::any_cast<const String&>(d->default_value);
}

bool ConfigItem::defaultBool() const
{
    return std::any_cast<bool>(d->default_value);
}

int ConfigItem::defaultInt() const
{
    return std::any_cast<int>(d->default_value);
}

double ConfigItem::defaultDouble() const
{
    return std::any_cast<double>(d->default_value);
}

ConfigItemType ConfigItem::defaultType() const
{
    if (std::any_cast<int>(&d->default_value) != nullptr)
        return ConfigItemType::Int;
    if (std::any_cast<double>(&d->default_value) != nullptr)
        return ConfigItemType::Double;
    return ConfigItemType::String;
}

bool ConfigItem::hasRange() const
{
    return d->min_value.has_value() && d->max_value.has_value();
}

int ConfigItem::minInt() const
{
    return static_cast<int>(std::any_cast<double>(d->min_value));
}

int ConfigItem::maxInt() const
{
    return static_cast<int>(std::any_cast<double>(d->max_value));
}

double ConfigItem::minDouble() const
{
    return std::any_cast<double>(d->min_value);
}

double ConfigItem::maxDouble() const
{
    return std::any_cast<double>(d->max_value);
}

double ConfigItem::step() const
{
    if (!d->step_value.has_value())
        return 1.0; // default step when none is set
    return std::any_cast<double>(d->step_value);
}

const std::vector<ConfigChoice>& ConfigItem::choices() const
{
    return d->choices;
}

bool ConfigItem::readOnly() const
{
    return d->read_only;
}

ConfigItem& ConfigItem::description(const String& v)
{
    d->description = v;
    return *this;
}

ConfigItem& ConfigItem::defaultValue(const String& v)
{
    if (d->type != ConfigItemType::String && d->type != ConfigItemType::Choice)
        throw std::invalid_argument("defaultValue(String) on a non-String/Choice item");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::defaultValue(const char8_t* v)
{
    return defaultValue(String(v));
}

ConfigItem& ConfigItem::defaultValue(bool v)
{
    if (d->type != ConfigItemType::Bool)
        throw std::invalid_argument("defaultValue(bool) on a non-Bool item");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::defaultValue(int v)
{
    if (d->type != ConfigItemType::Int && d->type != ConfigItemType::Choice)
        throw std::invalid_argument("defaultValue(int) on a non-Int/Choice item");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::defaultValue(double v)
{
    if (d->type != ConfigItemType::Double && d->type != ConfigItemType::Choice)
        throw std::invalid_argument("defaultValue(double) on a non-Double/Choice item");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::range(int min, int max)
{
    return range(static_cast<double>(min), static_cast<double>(max));
}

ConfigItem& ConfigItem::range(double min, double max)
{
    if (d->type != ConfigItemType::Int && d->type != ConfigItemType::Double)
        throw std::invalid_argument("range() on a non-numeric item");
    d->min_value = min;
    d->max_value = max;
    d->step_value.reset(); // re-ranging resets the step to the default 1.0
    return *this;
}

ConfigItem& ConfigItem::step(int s)
{
    return step(static_cast<double>(s));
}

ConfigItem& ConfigItem::step(double s)
{
    if (d->type != ConfigItemType::Int && d->type != ConfigItemType::Double)
        throw std::invalid_argument("step() on a non-numeric item");
    if (!d->min_value.has_value())
        throw std::invalid_argument("step() requires a range() first");
    d->step_value = s;
    return *this;
}

ConfigItem& ConfigItem::choices(std::initializer_list<const char8_t*> cs)
{
    std::vector<ConfigChoice> out;
    out.reserve(cs.size());
    for (const auto* s : cs) out.push_back(ConfigChoice{ String(s), String(s) }); // value == description
    d->choices = std::move(out);
    return *this;
}

ConfigItem& ConfigItem::choices(std::initializer_list<std::pair<std::variant<int, double, String>, String>> cs)
{
    std::vector<ConfigChoice> out;
    out.reserve(cs.size());
    for (const auto& c : cs) {
        ConfigChoice choice;
        choice.value       = std::visit([](const auto& v) { return std::any(v); }, c.first);
        choice.description = c.second;
        out.push_back(std::move(choice));
    }
    d->choices = std::move(out);
    return *this;
}

ConfigItem& ConfigItem::readOnly(bool on)
{
    d->read_only = on;
    return *this;
}

VN_APPFW_NS_END
