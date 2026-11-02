#include <vine/appfw/ConfigManager.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <vine/logging/Log.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/ConfigSnapshot.hpp>

VN_APPFW_NS_BEGIN

namespace
{

using ConfigValue = std::variant<std::monostate, String, bool, int, double, std::vector<String>, std::vector<bool>, std::vector<int>, std::vector<double>>;

QString toQString(const String& s)
{
    auto u16 = s.toUtf16();
    return QString::fromStdU16String(u16);
}

String fromQString(const QString& qs)
{
    return String::fromUtf16((const char16_t*)qs.utf16(), static_cast<size_t>(qs.size()));
}

QByteArray toQByteArray(const String& s)
{
    return QByteArray(reinterpret_cast<const char*>(s.data()), static_cast<int>(s.size()));
}

// UTF-8 view of a key for logging, without allocating.
std::string_view toUtf8View(const String& s) noexcept
{
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

// Converts a JSON integer to int, clamping and reporting an out-of-range value.
//
// A hand-edited file may carry a number that does not fit the stored type;
// storing a silently different one would be worse than saying so.
int jsonToInt(double value, const String& key)
{
    constexpr double lo = std::numeric_limits<int>::min();
    constexpr double hi = std::numeric_limits<int>::max();
    if (value >= lo && value <= hi)
        return static_cast<int>(value);
    const int clamped = value > hi ? static_cast<int>(hi) : static_cast<int>(lo);
    VN_LOGW("Config entry '{}' holds the out-of-range integer {}; using {}", toUtf8View(key), value, clamped);
    return clamped;
}

std::optional<double> jsonFiniteNumber(const QJsonValue& value)
{
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (!std::isfinite(number))
        return std::nullopt;
    return number;
}

std::optional<double> jsonInteger(const QJsonValue& value)
{
    const std::optional<double> number = jsonFiniteNumber(value);
    if (!number || std::trunc(*number) != *number)
        return std::nullopt;
    return number;
}

template <typename T, typename ParseElement>
std::optional<std::vector<T>> jsonArray(const QJsonValue& value, ParseElement parse_element)
{
    if (!value.isArray())
        return std::nullopt;

    const QJsonArray array = value.toArray();
    std::vector<T> result;
    result.reserve(static_cast<size_t>(array.size()));
    for (const QJsonValue& element : array) {
        std::optional<T> parsed = parse_element(element);
        if (!parsed)
            return std::nullopt;
        result.push_back(std::move(*parsed));
    }
    return result;
}

bool hasConfigKey(const String& key)
{
    if (!key.empty())
        return true;
    VN_LOGW("Config key is empty; mutation ignored");
    return false;
}

void warnInvalidJsonEntry(const String& key, const QString& type)
{
    VN_LOGW("Config entry '{}' has an invalid value for type '{}'; ignored", toUtf8View(key), toUtf8View(fromQString(type)));
}

// Stores a value and reports whether it differs from the one stored before.
//
// A write that does not change the value must not notify subscribers: a handler
// that normalizes a value back to the one already stored would otherwise loop
// through its own change event.
template <typename T>
bool assignValue(std::map<String, ConfigValue>& values, const String& key, T&& value)
{
    using V       = std::remove_cvref_t<T>;
    const auto it = values.find(key);
    if (it == values.end()) {
        values.emplace(key, std::forward<T>(value));
        return true;
    }
    const auto* stored = std::get_if<V>(&it->second);
    if (stored != nullptr && *stored == value)
        return false;
    it->second = std::forward<T>(value);
    return true;
}

// Reads a value, falling back to def when the key is absent or holds another type.
template <typename T>
T readValue(const std::map<String, ConfigValue>& values, const String& key, const T& def)
{
    const auto it = values.find(key);
    if (it == values.end())
        return def;
    const auto* stored = std::get_if<T>(&it->second);
    return stored != nullptr ? *stored : def;
}

template <typename T>
std::optional<T> readOptionalValue(const std::map<String, ConfigValue>& values, const String& key)
{
    const auto it = values.find(key);
    if (it == values.end())
        return std::nullopt;
    const auto* stored = std::get_if<T>(&it->second);
    return stored != nullptr ? std::optional<T>(*stored) : std::nullopt;
}

const ConfigItem* registeredConfigItem(const ConfigManager& manager, const String& key)
{
    Application* app = Application::current();
    if (app == nullptr || app->configManager() != &manager)
        return nullptr;
    ConfigRegistry* registry = app->configRegistry();
    return registry != nullptr ? registry->item(key) : nullptr;
}

template <typename T>
std::optional<T> configItemDefault(const ConfigManager& manager, const String& key)
{
    const ConfigItem* item = registeredConfigItem(manager, key);
    if (item == nullptr || !item->hasDefault())
        return std::nullopt;

    if constexpr (std::is_same_v<T, String>) {
        if ((item->type() == ConfigItemType::String || item->type() == ConfigItemType::Choice)
            && item->defaultType() == ConfigItemType::String)
            return item->defaultString();
    }
    else if constexpr (std::is_same_v<T, bool>) {
        if (item->type() == ConfigItemType::Bool)
            return item->defaultBool();
    }
    else if constexpr (std::is_same_v<T, int>) {
        if ((item->type() == ConfigItemType::Int || item->type() == ConfigItemType::Choice)
            && item->defaultType() == ConfigItemType::Int)
            return item->defaultInt();
    }
    else if constexpr (std::is_same_v<T, double>) {
        if ((item->type() == ConfigItemType::Double || item->type() == ConfigItemType::Choice)
            && item->defaultType() == ConfigItemType::Double)
            return item->defaultDouble();
    }
    return std::nullopt;
}

// Notifies subscribers that a key changed.
//
// An empty key means "the whole configuration changed": clear() and loadJson()
// report a replacement that is not attributable to a single key that way.
void notifyChanged(ConfigManager& manager, const String& key)
{
    ConfigChangedEventArgs args(key);
    manager.changed.trigger(manager, args);
}

// Inserts a leaf entry into the nested object layer by layer along the path
// (existing containers are kept; an object overrides on a level conflict).
QJsonObject insertNested(QJsonObject obj, const QStringList& path, const QJsonObject& entry)
{
    const QString head = path.first();
    if (path.size() == 1) {
        obj.insert(head, entry);
        return obj;
    }
    QJsonObject child = obj.value(head).toObject();
    child             = insertNested(child, path.mid(1), entry);
    obj.insert(head, child);
    return obj;
}

// Recursively flattens nested objects: leaf nodes (typed entries with exactly
// a type/value key pair) are written to dotted keys.
//
// Entries that do not follow that shape are reported and skipped: a typo in a
// hand-edited file must not drop a setting without a trace.
void flattenJson(const QJsonObject& obj, const QString& prefix, std::map<String, ConfigValue>& out)
{
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        const QString    name = it.key();
        const QString    full = prefix.isEmpty() ? name : prefix + QStringLiteral(".") + name;
        const String     key  = fromQString(full);
        const QJsonValue val  = it.value();
        if (key.empty()) {
            VN_LOGW("Config entry has an empty key; ignored");
            continue;
        }
        if (!val.isObject()) {
            VN_LOGW("Config entry '{}' is a plain value, not of the type/value form; ignored", toUtf8View(key));
            continue;
        }
        const QJsonObject o = val.toObject();
        const bool has_type_marker = o.contains(QStringLiteral("type")) || o.contains(QStringLiteral("value"));
        if (has_type_marker) {
            if (o.size() != 2 || !o.value(QStringLiteral("type")).isString() || !o.contains(QStringLiteral("value"))) {
                VN_LOGW("Config entry '{}' is not a valid type/value entry; ignored", toUtf8View(key));
                continue;
            }
            // leaf entry: parse the type
            const QString    type  = o.value(QStringLiteral("type")).toString();
            const QJsonValue value = o.value(QStringLiteral("value"));
            if (type == QStringLiteral("string")) {
                if (!value.isString()) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = fromQString(value.toString());
            }
            else if (type == QStringLiteral("bool")) {
                if (!value.isBool()) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = value.toBool();
            }
            else if (type == QStringLiteral("int")) {
                const std::optional<double> number = jsonInteger(value);
                if (!number) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = jsonToInt(*number, key);
            }
            else if (type == QStringLiteral("double")) {
                const std::optional<double> number = jsonFiniteNumber(value);
                if (!number) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = *number;
            }
            else if (type == QStringLiteral("string[]")) {
                auto array = jsonArray<String>(value, [](const QJsonValue& element) -> std::optional<String> {
                    if (!element.isString())
                        return std::nullopt;
                    return fromQString(element.toString());
                });
                if (!array) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = std::move(*array);
            }
            else if (type == QStringLiteral("bool[]")) {
                auto array = jsonArray<bool>(value, [](const QJsonValue& element) -> std::optional<bool> {
                    if (!element.isBool())
                        return std::nullopt;
                    return element.toBool();
                });
                if (!array) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = std::move(*array);
            }
            else if (type == QStringLiteral("int[]")) {
                const auto numbers = jsonArray<double>(value, [](const QJsonValue& element) {
                    return jsonInteger(element);
                });
                if (!numbers) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                std::vector<int> arr;
                arr.reserve(numbers->size());
                for (double number : *numbers)
                    arr.push_back(jsonToInt(number, key));
                out[key] = std::move(arr);
            }
            else if (type == QStringLiteral("double[]")) {
                auto array = jsonArray<double>(value, [](const QJsonValue& element) {
                    return jsonFiniteNumber(element);
                });
                if (!array) {
                    warnInvalidJsonEntry(key, type);
                    continue;
                }
                out[key] = std::move(*array);
            }
            else {
                VN_LOGW("Config entry '{}' has the unknown type '{}'; ignored", toUtf8View(key), toUtf8View(fromQString(type)));
            }
        }
        else {
            // nested container: recurse
            flattenJson(o, full, out);
        }
    }
}

} // namespace

VN_OBJECT_META_IMPL(ConfigChangedEventArgs, EventArgs)

ConfigChangedEventArgs::ConfigChangedEventArgs(const String& key)
  : key_(key)
{}

const String& ConfigChangedEventArgs::key() const
{
    return key_;
}

struct ConfigManager::Impl {
    std::map<String, ConfigValue> values;
    std::shared_mutex             mutex;
};

struct ConfigSnapshot::Impl {
    std::map<String, ConfigValue> values;
};

ConfigSnapshot::ConfigSnapshot()
  : d(new Impl)
{}

ConfigSnapshot::ConfigSnapshot(const ConfigSnapshot& other)
  : d(new Impl(*other.d))
{}

ConfigSnapshot& ConfigSnapshot::operator=(const ConfigSnapshot& other)
{
    if (this != &other) {
        *d = *other.d;
    }
    return *this;
}

ConfigSnapshot::ConfigSnapshot(ConfigSnapshot&& other) noexcept
  : d(std::move(other.d))
{}

ConfigSnapshot& ConfigSnapshot::operator=(ConfigSnapshot&& other) noexcept
{
    if (this != &other) {
        d = std::move(other.d);
    }
    return *this;
}

ConfigSnapshot::~ConfigSnapshot() = default;

std::vector<String> ConfigSnapshot::divergedKeys(const ConfigSnapshot& other, std::span<const String> keys) const
{
    std::vector<String> out;
    for (const String& key : keys) {
        const auto mine   = d->values.find(key);
        const auto theirs = other.d->values.find(key);
        const bool both_absent = mine == d->values.end() && theirs == other.d->values.end();
        const bool same = !both_absent && mine != d->values.end() && theirs != other.d->values.end() && mine->second == theirs->second;
        if (!both_absent && !same) {
            out.push_back(key);
        }
    }
    return out;
}

ConfigManager::ConfigManager()
  : d(new Impl)
{}

ConfigManager::~ConfigManager() = default;

bool ConfigManager::contains(const String& key) const
{
    std::shared_lock lock(d->mutex);
    return d->values.find(key) != d->values.end();
}

void ConfigManager::remove(const String& key)
{
    if (!hasConfigKey(key))
        return;
    bool erased = false;
    {
        std::lock_guard lock(d->mutex);
        erased = d->values.erase(key) > 0;
    }
    if (erased)
        notifyChanged(*this, key);
}

void ConfigManager::clear()
{
    bool had_values = false;
    {
        std::lock_guard lock(d->mutex);
        had_values = !d->values.empty();
        d->values.clear();
    }
    if (had_values)
        notifyChanged(*this, String());
}

void ConfigManager::setString(const String& key, const String& value)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::optional<String> ConfigManager::getString(const String& key) const
{
    {
        std::shared_lock lock(d->mutex);
        std::optional<String> value = readOptionalValue<String>(d->values, key);
        if (value)
            return value;
    }
    return configItemDefault<String>(*this, key);
}

String ConfigManager::getString(const String& key, const String& fallback) const
{
    return getString(key).value_or(fallback);
}

void ConfigManager::setBool(const String& key, bool value)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::optional<bool> ConfigManager::getBool(const String& key) const
{
    {
        std::shared_lock lock(d->mutex);
        std::optional<bool> value = readOptionalValue<bool>(d->values, key);
        if (value)
            return value;
    }
    return configItemDefault<bool>(*this, key);
}

bool ConfigManager::getBool(const String& key, bool fallback) const
{
    return getBool(key).value_or(fallback);
}

void ConfigManager::setInt(const String& key, int value)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::optional<int> ConfigManager::getInt(const String& key) const
{
    {
        std::shared_lock lock(d->mutex);
        std::optional<int> value = readOptionalValue<int>(d->values, key);
        if (value)
            return value;
    }
    return configItemDefault<int>(*this, key);
}

int ConfigManager::getInt(const String& key, int fallback) const
{
    return getInt(key).value_or(fallback);
}

void ConfigManager::setDouble(const String& key, double value)
{
    if (!hasConfigKey(key))
        return;
    if (!std::isfinite(value)) {
        VN_LOGW("Config value for '{}' is not finite; mutation ignored", toUtf8View(key));
        return;
    }
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::optional<double> ConfigManager::getDouble(const String& key) const
{
    {
        std::shared_lock lock(d->mutex);
        std::optional<double> value = readOptionalValue<double>(d->values, key);
        if (value)
            return value;
    }
    return configItemDefault<double>(*this, key);
}

double ConfigManager::getDouble(const String& key, double fallback) const
{
    return getDouble(key).value_or(fallback);
}

void ConfigManager::setStringArray(const String& key, const std::vector<String>& values)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, values);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::vector<String> ConfigManager::getStringArray(const String& key) const
{
    std::shared_lock lock(d->mutex);
    return readValue<std::vector<String>>(d->values, key, {});
}

void ConfigManager::setBoolArray(const String& key, const std::vector<bool>& values)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, values);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::vector<bool> ConfigManager::getBoolArray(const String& key) const
{
    std::shared_lock lock(d->mutex);
    return readValue<std::vector<bool>>(d->values, key, {});
}

void ConfigManager::setIntArray(const String& key, const std::vector<int>& values)
{
    if (!hasConfigKey(key))
        return;
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, values);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::vector<int> ConfigManager::getIntArray(const String& key) const
{
    std::shared_lock lock(d->mutex);
    return readValue<std::vector<int>>(d->values, key, {});
}

void ConfigManager::setDoubleArray(const String& key, const std::vector<double>& values)
{
    if (!hasConfigKey(key))
        return;
    if (std::any_of(values.begin(), values.end(), [](double value) { return !std::isfinite(value); })) {
        VN_LOGW("Config array for '{}' contains a non-finite value; mutation ignored", toUtf8View(key));
        return;
    }
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, values);
    }
    if (assigned)
        notifyChanged(*this, key);
}

std::vector<double> ConfigManager::getDoubleArray(const String& key) const
{
    std::shared_lock lock(d->mutex);
    return readValue<std::vector<double>>(d->values, key, {});
}

String ConfigManager::toJson() const
{
    std::shared_lock lock(d->mutex);
    QJsonObject      obj;
    for (const auto& [key, value] : d->values) {
        QJsonObject entry;
        std::visit(
            [&entry](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, String>) {
                    entry[QStringLiteral("type")]  = QStringLiteral("string");
                    entry[QStringLiteral("value")] = toQString(v);
                }
                else if constexpr (std::is_same_v<T, bool>) {
                    entry[QStringLiteral("type")]  = QStringLiteral("bool");
                    entry[QStringLiteral("value")] = v;
                }
                else if constexpr (std::is_same_v<T, int>) {
                    entry[QStringLiteral("type")]  = QStringLiteral("int");
                    entry[QStringLiteral("value")] = v; // QJsonValue(int): integers stay exact
                }
                else if constexpr (std::is_same_v<T, double>) {
                    entry[QStringLiteral("type")]  = QStringLiteral("double");
                    entry[QStringLiteral("value")] = v;
                }
                else if constexpr (std::is_same_v<T, std::vector<String>>) {
                    entry[QStringLiteral("type")] = QStringLiteral("string[]");
                    QJsonArray arr;
                    for (const auto& s : v) arr.append(toQString(s));
                    entry[QStringLiteral("value")] = arr;
                }
                else if constexpr (std::is_same_v<T, std::vector<bool>>) {
                    entry[QStringLiteral("type")] = QStringLiteral("bool[]");
                    QJsonArray arr;
                    for (bool b : v) arr.append(b);
                    entry[QStringLiteral("value")] = arr;
                }
                else if constexpr (std::is_same_v<T, std::vector<int>>) {
                    entry[QStringLiteral("type")] = QStringLiteral("int[]");
                    QJsonArray arr;
                    for (int i : v) arr.append(i);
                    entry[QStringLiteral("value")] = arr;
                }
                else if constexpr (std::is_same_v<T, std::vector<double>>) {
                    entry[QStringLiteral("type")] = QStringLiteral("double[]");
                    QJsonArray arr;
                    for (double x : v) arr.append(x);
                    entry[QStringLiteral("value")] = arr;
                }
                // monostate: emit nothing
            },
            value);
        if (entry.isEmpty())
            continue;
        // expand dotted key into nested levels
        obj = insertNested(obj, toQString(key).split(QStringLiteral(".")), entry);
    }
    QJsonDocument doc(obj);
    return fromQString(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

bool ConfigManager::loadJson(const String& json)
{
    QJsonParseError err;
    QJsonDocument   doc = QJsonDocument::fromJson(toQByteArray(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    // Expand nested objects into dotted keys on a local map, then swap it in
    // under the lock so parsing never holds the mutex.
    std::map<String, ConfigValue> parsed;
    flattenJson(doc.object(), QString(), parsed);
    bool replaced = false;
    {
        std::lock_guard lock(d->mutex);
        replaced  = d->values != parsed;
        d->values = std::move(parsed);
    }
    if (replaced) {
        // A whole-configuration replacement, reported like clear() with an empty key.
        notifyChanged(*this, String());
    }
    return true;
}

bool ConfigManager::save(const String& path) const
{
    // QSaveFile writes a temporary next to the target and renames it over the
    // target on commit(), so a failing save (full disk, missing directory, crash)
    // never leaves a truncated file behind - and unlike a plain QFile::write(),
    // every step reports its result, so the return value can be trusted.
    QSaveFile file(toQString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray data = toQByteArray(toJson());
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool ConfigManager::load(const String& path)
{
    QFile file(toQString(path));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data    = file.readAll();
    const bool       read_ok = file.error() == QFileDevice::NoError;
    file.close();
    if (!read_ok)
        return false;
    return loadJson(fromQString(QString::fromUtf8(data)));
}

ConfigSnapshot ConfigManager::snapshot() const
{
    std::shared_lock lock(d->mutex);
    ConfigSnapshot   copy;
    copy.d->values = d->values; // ConfigSnapshot is a value-layer type: the map is exactly what it keeps
    return copy;
}

VN_APPFW_NS_END
