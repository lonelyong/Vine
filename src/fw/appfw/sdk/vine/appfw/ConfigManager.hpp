#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <memory>
#include <optional>
#include <vector>

#include <vine/Events.hpp>
#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

class ConfigSnapshot;

/**
 * @brief Config change event arguments: carry the key (dotted path) that changed.
 *
 * @note An empty key means "the whole configuration changed": clear() and a
 * loadJson()/load() that actually replaced something report a change that is
 * not attributable to a single key that way.
 */
class VN_APPFW_API ConfigChangedEventArgs : public EventArgs {
    VN_OBJECT_META_DECL

  public:
    explicit ConfigChangedEventArgs(const String& key);
    /// The key that changed.
    const String& key() const;

  private:
    String key_;
};

/**
 * @brief Config manager: stores String/bool/int/double scalars and arrays by
 * key, with JSON serialization support.
 *
 * Application holds the single instance (Application::configManager()).
 *
 * @note Typed accessors keep the get/set prefix (getString/getInt/setString/...):
 * the bare type names (int/bool/double) are keywords and cannot be method names,
 * and the getter/setter pair shares the key parameter.
 * @note Scalar getters return the stored value, or the matching ConfigItem
 * default registered by this manager's Application. The no-fallback overload
 * returns std::nullopt when neither exists; the explicit-fallback overload
 * returns its argument in that case. A standalone manager has no item defaults.
 * @note contains() and serialization describe stored values only; reading an
 * item default does not store it.
 * @note Keys support dotted hierarchies, e.g. u8"window.x" denotes x under
 * window; toJson()/loadJson() generate/read nested JSON objects by hierarchy.
 * @note Values are stored as String/bool/int/double and their arrays; toJson()
 * writes the typed-marker format and loadJson() reads it back without loss for
 * every value the API can express.
 * @note All accessors are thread-safe: an internal shared mutex allows
 * concurrent readers while writers hold it exclusively; the changed event is
 * fired synchronously on the mutating caller's thread after releasing the lock,
 * so handlers may safely call back into the manager.
 * @note An empty key is reserved for whole-configuration changed events; mutators
 * reject it and log a warning.
 * @note Non-finite double values are rejected by double setters because JSON
 * has no representation for them.
 * Subscribing and unsubscribing are thread-safe too (Signal has its own lock-free
 * snapshot), so handlers may be wired up while worker threads are already running.
 */
class VN_APPFW_API ConfigManager {
  public:
    ConfigManager();
    virtual ~ConfigManager();

  public:
    /// Config change event: fired only by a set*/remove/clear/loadJson that
    /// actually changed a value, carrying the key (empty: whole configuration).
    Event<ConfigManager, ConfigChangedEventArgs> changed;

  public:
    /// Whether the given key exists.
    bool contains(const String& key) const;
    /// Removes the given non-empty key; an empty key is rejected with a warning.
    void remove(const String& key);
    /// Clears all configuration.
    void clear();

  public:
    void setString(const String& key, const String& value);
    /**
     * @brief Reads the stored String or this application's matching item default.
     *
     * @param key Configuration key.
     * @return The stored value or registered item default; empty if neither is available.
     */
    std::optional<String> getString(const String& key) const;
    /**
     * @brief Reads the String value, using fallback only when no stored value or item default is available.
     *
     * @param key Configuration key.
     * @param fallback Value returned when the key has no matching value or item default.
     * @return The resolved String value.
     */
    String getString(const String& key, const String& fallback) const;

    void setBool(const String& key, bool value);
    /**
     * @brief Reads the stored bool or this application's matching item default.
     *
     * @param key Configuration key.
     * @return The stored value or registered item default; empty if neither is available.
     */
    std::optional<bool> getBool(const String& key) const;
    /**
     * @brief Reads the bool value, using fallback only when no stored value or item default is available.
     *
     * @param key Configuration key.
     * @param fallback Value returned when the key has no matching value or item default.
     * @return The resolved bool value.
     */
    bool                getBool(const String& key, bool fallback) const;

    void setInt(const String& key, int value);
    /**
     * @brief Reads the stored int or this application's matching item default.
     *
     * @param key Configuration key.
     * @return The stored value or registered item default; empty if neither is available.
     */
    std::optional<int> getInt(const String& key) const;
    /**
     * @brief Reads the int value, using fallback only when no stored value or item default is available.
     *
     * @param key Configuration key.
     * @param fallback Value returned when the key has no matching value or item default.
     * @return The resolved int value.
     */
    int                getInt(const String& key, int fallback) const;

    /// Stores a finite double; non-finite values are rejected with a warning.
    void setDouble(const String& key, double value);
    /**
     * @brief Reads the stored double or this application's matching item default.
     *
     * @param key Configuration key.
     * @return The stored value or registered item default; empty if neither is available.
     */
    std::optional<double> getDouble(const String& key) const;
    /**
     * @brief Reads the double value, using fallback only when no stored value or item default is available.
     *
     * @param key Configuration key.
     * @param fallback Value returned when the key has no matching value or item default.
     * @return The resolved double value.
     */
    double                getDouble(const String& key, double fallback) const;

  public:
    void                setStringArray(const String& key, const std::vector<String>& values);
    std::vector<String> getStringArray(const String& key) const;

    void              setBoolArray(const String& key, const std::vector<bool>& values);
    std::vector<bool> getBoolArray(const String& key) const;

    void             setIntArray(const String& key, const std::vector<int>& values);
    std::vector<int> getIntArray(const String& key) const;

    /// Stores an array of finite doubles; any non-finite element rejects the whole write with a warning.
    void                setDoubleArray(const String& key, const std::vector<double>& values);
    std::vector<double> getDoubleArray(const String& key) const;

  public:
    /// Exports to a JSON string (typed-marker format, lossless round-trip).
    String toJson() const;

    /**
     * @brief Copies the current values.
     *
     * @details
     * - The copy is independent of this manager: later writes do not change it. That is what lets a caller keep "the
     *   values this run started with" and compare against them later (see ConfigSnapshot).
     * - Cheap: the stored values are scalars, strings and arrays; no file or JSON is involved.
     *
     * @return The values as they are now.
     */
    ConfigSnapshot snapshot() const;
    /// Replaces the whole configuration from a JSON string; fires changed with
    /// an empty key when the values differ. Returns false when the text is not
    /// a JSON object; malformed entries and empty keys are ignored and logged.
    bool loadJson(const String& json);
    /// Saves to a file (UTF-8 JSON).
    ///
    /// The write goes to a temporary file next to path that is renamed over it,
    /// so a failure never leaves a truncated file behind; the parent directory
    /// must exist. Returns true only when the data reached the file.
    bool save(const String& path) const;
    /// Loads from a file (as loadJson()).
    bool load(const String& path);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
