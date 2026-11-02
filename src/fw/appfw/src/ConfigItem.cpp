#include <vine/appfw/ConfigItem.hpp>

#include <any>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include <QRegularExpression>
#include <QString>
#include <QTextBoundaryFinder>

VN_APPFW_NS_BEGIN

namespace
{

QString toQString(const String& value)
{
    return QString::fromStdU16String(value.toUtf16());
}

int graphemeCount(const QString& text)
{
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int count = 0;
    while (finder.toNextBoundary() >= 0)
        ++count;
    return count;
}

bool matchesString(const String& value, const std::optional<QRegularExpression>& expression, const std::optional<int>& min,
                   const std::optional<int>& max, bool required)
{
    const QString text = toQString(value);
    if (text.isEmpty())
        return !required;
    const int     size = graphemeCount(text);
    if ((min && size < *min) || (max && size > *max))
        return false;
    return !expression || expression->match(text).hasMatch();
}

} // namespace

struct ConfigItem::Impl {
    String         key;
    String         label;
    String         description;
    ConfigItemType type = ConfigItemType::String;

    std::any default_value;

    std::any min_value;
    std::any max_value;
    std::any step_value;
    String    regular_expression;
    std::optional<QRegularExpression> compiled_expression;
    std::optional<int> min_length;
    std::optional<int> max_length;
    bool required = false;

    std::vector<ConfigChoice> choices;
    bool                      read_only        = false;
    bool                      restart_required = false;
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

bool ConfigItem::isValidInt(int value) const
{
    return d->type == ConfigItemType::Int && (!hasRange() || (value >= minInt() && value <= maxInt()));
}

double ConfigItem::minDouble() const
{
    return std::any_cast<double>(d->min_value);
}

double ConfigItem::maxDouble() const
{
    return std::any_cast<double>(d->max_value);
}

bool ConfigItem::isValidDouble(double value) const
{
    return d->type == ConfigItemType::Double && std::isfinite(value)
           && (!hasRange() || (value >= minDouble() && value <= maxDouble()));
}

double ConfigItem::step() const
{
    if (!d->step_value.has_value())
        return 1.0; // default step when none is set
    return std::any_cast<double>(d->step_value);
}

bool ConfigItem::hasLengthRange() const
{
    return d->min_length.has_value() && d->max_length.has_value();
}

int ConfigItem::minLength() const
{
    if (!d->min_length)
        throw std::bad_any_cast();
    return *d->min_length;
}

int ConfigItem::maxLength() const
{
    if (!d->max_length)
        throw std::bad_any_cast();
    return *d->max_length;
}

const String& ConfigItem::regularExpression() const
{
    return d->regular_expression;
}

bool ConfigItem::isValidString(const String& value) const
{
    return d->type == ConfigItemType::String
           && matchesString(value, d->compiled_expression, d->min_length, d->max_length, d->required);
}

bool ConfigItem::isRequired() const
{
    return d->required;
}

const std::vector<ConfigChoice>& ConfigItem::choices() const
{
    return d->choices;
}

bool ConfigItem::readOnly() const
{
    return d->read_only;
}

bool ConfigItem::restartRequired() const
{
    return d->restart_required;
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
    if (d->type == ConfigItemType::String
        && !matchesString(v, d->compiled_expression, d->min_length, d->max_length, d->required))
        throw std::invalid_argument("defaultValue(String) does not satisfy the configured string constraints");
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
    if (d->type == ConfigItemType::Int && !isValidInt(v))
        throw std::invalid_argument("defaultValue(int) is outside the configured range");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::defaultValue(double v)
{
    if (d->type != ConfigItemType::Double && d->type != ConfigItemType::Choice)
        throw std::invalid_argument("defaultValue(double) on a non-Double/Choice item");
    if (!std::isfinite(v))
        throw std::invalid_argument("defaultValue(double) must be finite");
    if (d->type == ConfigItemType::Double && !isValidDouble(v))
        throw std::invalid_argument("defaultValue(double) is outside the configured range");
    d->default_value = v;
    return *this;
}

ConfigItem& ConfigItem::regularExpression(const String& expression)
{
    if (d->type != ConfigItemType::String)
        throw std::invalid_argument("regularExpression() on a non-String item");

    std::optional<QRegularExpression> compiled;
    if (!expression.empty()) {
        compiled.emplace(QRegularExpression::anchoredPattern(toQString(expression)));
        if (!compiled->isValid())
            throw std::invalid_argument("regularExpression() requires a valid Qt regular expression");
    }

    if (const auto* value = std::any_cast<String>(&d->default_value);
        value != nullptr && !matchesString(*value, compiled, d->min_length, d->max_length, d->required))
        throw std::invalid_argument("regularExpression() excludes the configured default");

    d->regular_expression = expression;
    d->compiled_expression = std::move(compiled);
    return *this;
}

ConfigItem& ConfigItem::lengthRange(int min, int max)
{
    if (d->type != ConfigItemType::String)
        throw std::invalid_argument("lengthRange() on a non-String item");
    if (min < 0 || max < 0 || min > max)
        throw std::invalid_argument("lengthRange() requires non-negative bounds with min <= max");

    if (const auto* value = std::any_cast<String>(&d->default_value);
        value != nullptr && !matchesString(*value, d->compiled_expression, min, max, d->required))
        throw std::invalid_argument("lengthRange() excludes the configured default");

    d->min_length = min;
    d->max_length = max;
    return *this;
}

ConfigItem& ConfigItem::required(bool on)
{
    if (d->type == ConfigItemType::String) {
        if (const auto* value = std::any_cast<String>(&d->default_value);
            value != nullptr && !matchesString(*value, d->compiled_expression, d->min_length, d->max_length, on))
            throw std::invalid_argument("required() excludes the configured default");
    }

    d->required = on;
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
    if (!std::isfinite(min) || !std::isfinite(max) || min > max)
        throw std::invalid_argument("range() requires finite bounds with min <= max");
    if (d->type == ConfigItemType::Int
        && (std::trunc(min) != min || std::trunc(max) != max || min < std::numeric_limits<int>::min()
            || max > std::numeric_limits<int>::max()))
        throw std::invalid_argument("range() on an Int item requires int bounds");
    if (d->default_value.has_value()) {
        if (const auto* value = std::any_cast<int>(&d->default_value)) {
            if (static_cast<double>(*value) < min || static_cast<double>(*value) > max)
                throw std::invalid_argument("range() excludes the configured default");
        }
        else if (const auto* value = std::any_cast<double>(&d->default_value)) {
            if (*value < min || *value > max)
                throw std::invalid_argument("range() excludes the configured default");
        }
    }
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
    if (!std::isfinite(s) || s <= 0.0)
        throw std::invalid_argument("step() requires a positive finite value");
    if (d->type == ConfigItemType::Int
        && (std::trunc(s) != s || s > std::numeric_limits<int>::max()))
        throw std::invalid_argument("step() on an Int item requires a positive int value");
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

ConfigItem& ConfigItem::restartRequired(bool on)
{
    d->restart_required = on;
    return *this;
}

VN_APPFW_NS_END
