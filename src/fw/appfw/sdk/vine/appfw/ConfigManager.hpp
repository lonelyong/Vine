#pragma once

#include <vine/Events.hpp>
#include <vine/String.hpp>
#include <vine/appfw/appfw_global.hpp>

#include <memory>
#include <vector>

V_APPFW_NS_BEGIN

/**
 * @brief Config change event arguments: carry the key (dotted path) that changed.
 *
 * @note An empty key means "the whole configuration changed": clear() and a
 * loadJson()/load() that actually replaced something report a change that is
 * not attributable to a single key that way.
 */
class V_APPFW_API ConfigChangedEventArgs : public EventArgs {
    V_OBJECT_META_DECL

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
 * @note Getters fall back to their default when the key is absent *or* holds
 * another type, so a getter never throws.
 * @note Keys support dotted hierarchies, e.g. u8"window.x" denotes x under
 * window; toJson()/loadJson() generate/read nested JSON objects by hierarchy.
 * @note Values are stored as String/bool/int/double and their arrays; toJson()
 * writes the typed-marker format and loadJson() reads it back without loss for
 * every value the API can express.
 * @note All accessors are thread-safe: an internal shared mutex allows
 * concurrent readers while writers hold it exclusively; the changed event is
 * fired after releasing it, so handlers may safely call back into the manager.
 * Subscribing to and unsubscribing from changed are *not* thread-safe (the
 * signal has no lock), so wire up handlers before worker threads run.
 */
class V_APPFW_API ConfigManager {
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
    /// Removes the given key.
    void remove(const String& key);
    /// Clears all configuration.
    void clear();

  public:
    void   setString(const String& key, const String& value);
    String getString(const String& key, const String& def = String()) const;

    void setBool(const String& key, bool value);
    bool getBool(const String& key, bool def = false) const;

    void setInt(const String& key, int value);
    int  getInt(const String& key, int def = 0) const;

    void   setDouble(const String& key, double value);
    double getDouble(const String& key, double def = 0.0) const;

  public:
    void                setStringArray(const String& key, const std::vector<String>& values);
    std::vector<String> getStringArray(const String& key) const;

    void              setBoolArray(const String& key, const std::vector<bool>& values);
    std::vector<bool> getBoolArray(const String& key) const;

    void             setIntArray(const String& key, const std::vector<int>& values);
    std::vector<int> getIntArray(const String& key) const;

    void                setDoubleArray(const String& key, const std::vector<double>& values);
    std::vector<double> getDoubleArray(const String& key) const;

  public:
    /// Exports to a JSON string (typed-marker format, lossless round-trip).
    String toJson() const;
    /// Replaces the whole configuration from a JSON string; fires changed with
    /// an empty key when the values differ. Returns false when the text is not
    /// a JSON object; entries of another shape are ignored and logged.
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

V_APPFW_NS_END
