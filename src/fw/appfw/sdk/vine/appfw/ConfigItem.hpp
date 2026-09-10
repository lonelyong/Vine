#pragma once

#include "appfw_global.hpp"

#include <any>
#include <initializer_list>
#include <utility>
#include <variant>
#include <memory>
#include <vector>

#include <vine/String.hpp>

V_APPFW_NS_BEGIN

/**
 * @brief A choice option: a typed value paired with its display description.
 *
 * The value is held in std::any and stores an int, double or String depending
 * on which choices() overload set it. Qt-free.
 */
struct ConfigChoice {
    std::any value; // int / double / String
    String   description;
};

/**
 * @brief Configurable item type; the panel picks the editor widget from it.
 */
enum class ConfigItemType
{
    String, ///< Single-line text -> QLineEdit
    Bool,   ///< Toggle -> QCheckBox
    Int,    ///< Integer -> QSpinBox
    Double, ///< Floating point -> QDoubleSpinBox
    Choice, ///< Enum choice (stored as String) -> QComboBox
};

/**
 * @brief Configuration item descriptor: describes how an item is shown and edited.
 *
 * Pure data, Qt-free. key is the dot-separated path in ConfigManager; label and
 * description are shown by the panel; type selects the editor; defaultValue,
 * range, step and choices constrain editing. The display placement (category and
 * group) is decided by the ConfigGroup tree, not stored here. Plugins register
 * items through PluginLoadContext::configRegistry() or directly with ConfigRegistry.
 *
 * @note Fluent builders return the object itself, e.g.
 * item.range(8, 72).defaultValue(14)
 */
class V_APPFW_API ConfigItem {
  public:
    /**
     * @brief Constructs an item.
     *
     * @param key   Dot-separated ConfigManager path.
     * @param label Display name.
     * @param type  Item type.
     */
    ConfigItem(String key, String label, ConfigItemType type);

    ConfigItem(const ConfigItem& other);
    ConfigItem& operator=(const ConfigItem& other);
    ConfigItem(ConfigItem&& other) noexcept;
    ConfigItem& operator=(ConfigItem&& other) noexcept;
    ~ConfigItem();

  public:
    /**
     * @brief Dot-separated key (ConfigManager path).
     */
    const String& key() const;
    /**
     * @brief Display name.
     */
    const String& label() const;
    /**
     * @brief Description (panel tooltip).
     */
    const String& description() const;
    /**
     * @brief Item type.
     */
    ConfigItemType type() const;

    /**
     * @brief Whether a default value is configured.
     */
    bool hasDefault() const;
    /**
     * @brief Default value; valid when hasDefault() is true.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    const String& defaultString() const;
    /**
     * @brief Default bool value.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    bool defaultBool() const;
    /**
     * @brief Default int value.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    int defaultInt() const;
    /**
     * @brief Default double value.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    double defaultDouble() const;
    /**
     * @brief The stored type of the default value (Int/Double/String; String when unset).
     */
    ConfigItemType defaultType() const;

    /**
     * @brief Whether a numeric range is configured.
     */
    bool hasRange() const;
    /**
     * @brief Minimum int value.
     *
     * @throws std::bad_any_cast if no range is set.
     */
    int minInt() const;
    /**
     * @brief Maximum int value.
     *
     * @throws std::bad_any_cast if no range is set.
     */
    int maxInt() const;
    /**
     * @brief Minimum double value.
     *
     * @throws std::bad_any_cast if no range is set.
     */
    double minDouble() const;
    /**
     * @brief Maximum double value.
     *
     * @throws std::bad_any_cast if no range is set.
     */
    double maxDouble() const;
    /**
     * @brief Step (Int/Double).
     *
     * @note Returns 1.0 when no step has been set, whether or not a range is
     * configured; an editor started from that step moves by 1.0 across the whole
     * range, so set a step for ranges wider than a few hundred units.
     */
    double step() const;

    /**
     * @brief Choice options (value-description pairs).
     */
    const std::vector<ConfigChoice>& choices() const;
    /**
     * @brief Whether read-only (display only, not editable).
     */
    bool readOnly() const;

  public:
    /**
     * @brief Sets the description.
     */
    ConfigItem& description(const String& d);
    /**
     * @brief Sets the default string value (valid for String and Choice).
     *
     * @throws std::invalid_argument if the item type does not match.
     */
    ConfigItem& defaultValue(const String& v);
    /**
     * @brief Sets the default string value from a u8 literal.
     */
    ConfigItem& defaultValue(const char8_t* v);
    /**
     * @brief Sets the default bool value.
     *
     * @throws std::invalid_argument if the item type does not match.
     */
    ConfigItem& defaultValue(bool v);
    /**
     * @brief Sets the default int value (Int, or Choice with int-valued options).
     *
     * @throws std::invalid_argument if the item type does not match.
     */
    ConfigItem& defaultValue(int v);
    /**
     * @brief Sets the default double value (Double, or Choice with double-valued options).
     *
     * @throws std::invalid_argument if the item type does not match.
     */
    ConfigItem& defaultValue(double v);
    /**
     * @brief Sets an int range.
     *
     * @note Resets the step to its default 1.0, so call step() last.
     *
     * @throws std::invalid_argument if the item type is not Int/Double.
     */
    ConfigItem& range(int min, int max);
    /**
     * @brief Sets a double range.
     *
     * @note Resets the step to its default 1.0, so call step() last.
     *
     * @throws std::invalid_argument if the item type is not Int/Double.
     */
    ConfigItem& range(double min, double max);
    /**
     * @brief Sets an int step.
     *
     * @throws std::invalid_argument if the item type is not Int/Double or no range has been set.
     */
    ConfigItem& step(int s);
    /**
     * @brief Sets a double step.
     *
     * @throws std::invalid_argument if the item type is not Int/Double or no range has been set.
     */
    ConfigItem& step(double s);
    /**
     * @brief Sets the choice options (value equals description for each entry).
     */
    ConfigItem& choices(std::initializer_list<const char8_t*> cs);
    /**
     * @brief Sets choice options as value-description pairs; the value type
     * (int/double/String) is inferred from each literal.
     */
    ConfigItem& choices(std::initializer_list<std::pair<std::variant<int, double, String>, String>> cs);
    /**
     * @brief Sets the read-only flag.
     */
    ConfigItem& readOnly(bool on);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
