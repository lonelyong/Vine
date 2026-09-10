#include <vine/appfw/ConfigManager.hpp>

#include <limits>
#include <map>
#include <mutex>
#include <shared_mutex>
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

V_APPFW_NS_BEGIN

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
int jsonToInt(qint64 value, const String& key)
{
    constexpr qint64 lo = std::numeric_limits<int>::min();
    constexpr qint64 hi = std::numeric_limits<int>::max();
    if (value >= lo && value <= hi)
        return static_cast<int>(value);
    const int clamped = value > hi ? static_cast<int>(hi) : static_cast<int>(lo);
    V_LOGW("Config entry '{}' holds the out-of-range integer {}; using {}", toUtf8View(key), value, clamped);
    return clamped;
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
        if (!val.isObject()) {
            V_LOGW("Config entry '{}' is a plain value, not of the type/value form; ignored", toUtf8View(key));
            continue;
        }
        const QJsonObject o = val.toObject();
        if (o.contains(QStringLiteral("type")) && o.size() == 2) {
            // leaf entry: parse the type
            const QString    type  = o.value(QStringLiteral("type")).toString();
            const QJsonValue value = o.value(QStringLiteral("value"));
            if (type == QStringLiteral("string")) {
                out[key] = fromQString(value.toString());
            }
            else if (type == QStringLiteral("bool")) {
                out[key] = value.toBool();
            }
            else if (type == QStringLiteral("int")) {
                out[key] = jsonToInt(value.toInteger(static_cast<qint64>(value.toDouble())), key);
            }
            else if (type == QStringLiteral("double")) {
                out[key] = value.toDouble();
            }
            else if (type == QStringLiteral("string[]")) {
                std::vector<String> arr;
                const QJsonArray    a = value.toArray();
                arr.reserve(static_cast<size_t>(a.size()));
                for (const QJsonValue& v : a) arr.push_back(fromQString(v.toString()));
                out[key] = std::move(arr);
            }
            else if (type == QStringLiteral("bool[]")) {
                std::vector<bool> arr;
                const QJsonArray  a = value.toArray();
                for (const QJsonValue& v : a) arr.push_back(v.toBool());
                out[key] = std::move(arr);
            }
            else if (type == QStringLiteral("int[]")) {
                std::vector<int> arr;
                const QJsonArray a = value.toArray();
                arr.reserve(static_cast<size_t>(a.size()));
                for (const QJsonValue& v : a) arr.push_back(jsonToInt(v.toInteger(static_cast<qint64>(v.toDouble())), key));
                out[key] = std::move(arr);
            }
            else if (type == QStringLiteral("double[]")) {
                std::vector<double> arr;
                const QJsonArray    a = value.toArray();
                arr.reserve(static_cast<size_t>(a.size()));
                for (const QJsonValue& v : a) arr.push_back(v.toDouble());
                out[key] = std::move(arr);
            }
            else {
                V_LOGW("Config entry '{}' has the unknown type '{}'; ignored", toUtf8View(key), toUtf8View(fromQString(type)));
            }
        }
        else {
            // nested container: recurse
            flattenJson(o, full, out);
        }
    }
}

} // namespace

V_OBJECT_META_IMPL(ConfigChangedEventArgs, EventArgs)

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
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

String ConfigManager::getString(const String& key, const String& def) const
{
    std::shared_lock lock(d->mutex);
    return readValue<String>(d->values, key, def);
}

void ConfigManager::setBool(const String& key, bool value)
{
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

bool ConfigManager::getBool(const String& key, bool def) const
{
    std::shared_lock lock(d->mutex);
    return readValue<bool>(d->values, key, def);
}

void ConfigManager::setInt(const String& key, int value)
{
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

int ConfigManager::getInt(const String& key, int def) const
{
    std::shared_lock lock(d->mutex);
    return readValue<int>(d->values, key, def);
}

void ConfigManager::setDouble(const String& key, double value)
{
    bool assigned = false;
    {
        std::lock_guard lock(d->mutex);
        assigned = assignValue(d->values, key, value);
    }
    if (assigned)
        notifyChanged(*this, key);
}

double ConfigManager::getDouble(const String& key, double def) const
{
    std::shared_lock lock(d->mutex);
    return readValue<double>(d->values, key, def);
}

void ConfigManager::setStringArray(const String& key, const std::vector<String>& values)
{
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
    QJsonObject obj;
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

V_APPFW_NS_END
