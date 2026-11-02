#pragma once

#include "appfw_global.hpp"

#include <any>
#include <initializer_list>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

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
 * @brief Configurable item type; the panel picks its editor from it.
 */
enum class ConfigItemType
{
    String, // Single-line text -> QLineEdit
    Bool,   // Toggle -> QCheckBox
    Int,    // Integer -> QSpinBox
    Double, // Floating point -> QDoubleSpinBox
    Choice, // Enum choice (String/int/double value) -> QComboBox
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
class VN_APPFW_API ConfigItem {
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
     * @brief The stored type of the default value (Int/Double/String; String when unset).
     */
    ConfigItemType defaultType() const;

    /**
     * @brief Default String value; valid when hasDefault() is true.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    const String& defaultString() const;
    /**
     * @brief Whether a string length range is configured.
     *
     * @return true when both string length bounds are set.
     */
    bool hasLengthRange() const;
    /**
     * @brief Minimum user-perceived character count.
     *
     * @throws std::bad_any_cast if no string length range is set.
     * @return The inclusive minimum.
     */
    int minLength() const;
    /**
     * @brief Maximum user-perceived character count.
     *
     * @throws std::bad_any_cast if no string length range is set.
     * @return The inclusive maximum.
     */
    int maxLength() const;
    /**
     * @brief The regular expression constraint, or an empty string when unconstrained.
     *
     * @return The configured Qt regular expression.
     */
    const String& regularExpression() const;
    /**
     * @brief Whether a String value satisfies this item's required and text constraints.
     *
     * @details
     * - Empty values are valid only when the item is not required.
     * - The constraints describe editor behavior; ConfigManager does not apply them.
     *
     * @param value Candidate string.
     * @return true when the item is String and the value satisfies its constraints.
     */
    bool isValidString(const String& value) const;

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
     * @brief Whether a numeric range is configured.
     *
     * @return true when both numeric bounds are set.
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
     * @brief Whether an int value satisfies this Int item's configured range.
     *
     * @param value Candidate int.
     * @return true when the item is Int and the value is within range, or no range is configured.
     */
    bool isValidInt(int value) const;

    /**
     * @brief Default double value.
     *
     * @throws std::bad_any_cast if no default is set or the stored type differs.
     */
    double defaultDouble() const;
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
     * @brief Whether a double value satisfies this Double item's finite-value and range constraints.
     *
     * @param value Candidate double.
     * @return true when the item is Double, the value is finite, and it is within range or no range is configured.
     */
    bool isValidDouble(double value) const;
    /**
     * @brief Numeric editor step.
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
    /**
     * @brief Whether this item cannot be left unset in the configuration editor.
     *
     * @note This is editor metadata; `ConfigManager` remains schema-independent.
     *
     * @return true when the editor requires a value before the item is considered configured.
     */
    bool isRequired() const;
    /**
     * @brief Whether the value is read once at startup, so a change only takes effect after a restart.
     *
     * @details
     * - This is a claim about how the key is *read*, not a wish: the module registering the item must keep the promise
     *   that nothing reads the key after startup. A runtime reader would see the new value while the rest of the process
     *   still runs on the old one.
     * - It says nothing about where the value is stored: the edit is written immediately, like every other setting, and
     *   only its effect waits for the next run (`Application::pendingRestartKeys()` answers whether it is still pending).
     *
     * @return true when the item is read once at startup.
     */
    bool restartRequired() const;

  public:
    /**
     * @brief Sets the description.
     *
     * @param d Tooltip text.
     */
    ConfigItem& description(const String& d);
    /**
     * @brief Sets whether this item cannot be left unset in the configuration editor.
     *
     * @param on true to disallow an unset value; false to allow it.
     * @note Required applies to every item type; only String defaults can be invalidated by an empty value.
     * @throws std::invalid_argument if a required String item has an empty configured default.
     * @return The item itself, for chaining.
     */
    ConfigItem& required(bool on = true);
    /**
     * @brief Sets the default String value.
     *
     * @param v Default value.
     * @throws std::invalid_argument if the item type does not match or the String constraints reject the value.
     */
    ConfigItem& defaultValue(const String& v);
    /**
     * @brief Sets the default String value from a u8 literal.
     *
     * @param v Null-terminated UTF-8 default value.
     */
    ConfigItem& defaultValue(const char8_t* v);
    /**
     * @brief Sets the regular expression constraint for a String item.
     *
     * @details
     * - An empty expression removes the regular expression constraint.
     * - Non-empty expressions use Qt regular-expression syntax and must match the entire value.
     *
     * @param expression Regular expression, or empty to disable the constraint.
     * @throws std::invalid_argument if the item is not String, the expression is invalid, or the configured default
     *         does not satisfy the resulting constraints.
     * @return The item itself, for chaining.
     */
    ConfigItem& regularExpression(const String& expression);
    /**
     * @brief Sets the inclusive range of user-perceived characters for a String item.
     *
     * @details
     * - A user-perceived character is one Unicode grapheme cluster; combining sequences and joined emoji count as one.
     * - Use zero as @p min when only an upper bound is needed.
     *
     * @param min Inclusive minimum character count.
     * @param max Inclusive maximum character count.
     * @throws std::invalid_argument if the item is not String, either bound is negative, min > max, or the configured
     *         default does not satisfy the resulting constraints.
     * @return The item itself, for chaining.
     */
    ConfigItem& lengthRange(int min, int max);
    /**
     * @brief Sets the default bool value.
     *
     * @param v Default value.
     * @throws std::invalid_argument if the item type does not match.
     */
    ConfigItem& defaultValue(bool v);

    /**
     * @brief Sets the default int value (Int, or Choice with int-valued options).
     *
     * @param v Default value.
     * @throws std::invalid_argument if the item type does not match or the value lies outside a configured range.
     */
    ConfigItem& defaultValue(int v);
    /**
     * @brief Sets an int range.
     *
     * @note Resets the step to its default 1.0, so call step() last.
     *
     * @param min Inclusive lower bound.
     * @param max Inclusive upper bound.
     * @throws std::invalid_argument if the item type is not numeric, bounds are invalid, or the default is outside them.
     */
    ConfigItem& range(int min, int max);
    /**
     * @brief Sets an int editor step.
     *
     * @param s Positive step.
     * @throws std::invalid_argument if the item type is not numeric, no range is set, or the step is invalid.
     */
    ConfigItem& step(int s);

    /**
     * @brief Sets the default double value (Double, or Choice with double-valued options).
     *
     * @param v Default value.
     * @throws std::invalid_argument if the item type does not match, the value is non-finite, or it lies outside a
     *         configured range.
     */
    ConfigItem& defaultValue(double v);
    /**
     * @brief Sets a double range.
     *
     * @note Resets the step to its default 1.0, so call step() last.
     *
     * @param min Inclusive lower bound; must be finite.
     * @param max Inclusive upper bound; must be finite and no smaller than @p min.
     * @note Int items require integral bounds representable as int.
     * @throws std::invalid_argument if the item type is not numeric, bounds are invalid, or the default is outside them.
     */
    ConfigItem& range(double min, double max);
    /**
     * @brief Sets a double editor step.
     *
     * @param s Positive finite step; for Int items it must be an integer representable as int.
     * @throws std::invalid_argument if the item type is not numeric, no range is set, or the step is invalid.
     */
    ConfigItem& step(double s);

    /**
     * @brief Sets the choice options (value equals description for each entry).
     *
     * @param cs UTF-8 values, also used as their display descriptions.
     */
    ConfigItem& choices(std::initializer_list<const char8_t*> cs);
    /**
     * @brief Sets choice options as value-description pairs; the value type
     * (int/double/String) is inferred from each literal.
     *
     * @param cs Option values and their display descriptions.
     */
    ConfigItem& choices(std::initializer_list<std::pair<std::variant<int, double, String>, String>> cs);
    /**
     * @brief Sets the read-only flag.
     *
     * @param on true to disable editing.
     */
    ConfigItem& readOnly(bool on);

    /**
     * @brief Marks the item as read once at startup, so a change only takes effect after a restart.
     *
     * @param on true when the item needs a restart to apply.
     * @return The item itself, for chaining.
     */
    ConfigItem& restartRequired(bool on);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFW_NS_END
