#pragma once

#include <vine/raw_ptr.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>

#include <vine/appfw/gui/Window.hpp>

V_APPFWGUI_NS_BEGIN

/**
 * @brief Configuration window: renders editors from the registry (ConfigRegistry)
 * and reads/writes values through ConfigManager.
 *
 * Each item gets an editor by type (String -> QLineEdit, Bool -> QCheckBox,
 * Int -> QSpinBox, Double -> QDoubleSpinBox, Choice -> QComboBox), shown in a
 * two-level "category -> group" layout (ConfigCategory -> ConfigGroup).
 * Edits write back to ConfigManager immediately (its changed event fires);
 * refresh() reloads values from storage and reset() restores defaults.
 * Inherits Window; show() non-modally or exec() modally.
 *
 * @note The window snapshots the registry item tree when it is built: a plugin
 * that registers items afterwards changes nothing in an existing window, so
 * build a new one to show them. Editors keep following their own key, so such a
 * registration cannot shift a value into the wrong row.
 * @note A read-only item is shown disabled; a Choice item whose stored value
 * matches none of its choices is shown with no selection rather than with the
 * first choice.
 */
class V_APPFW_API ConfigWindow : public Window {
    V_OBJECT_META_DECL

  public:
    /**
     * @brief Builds the window content from the registry item tree, using config
     * as the data source.
     *
     * @param registry Registry holding the item tree.
     * @param config   Config manager holding the values.
     */
    ConfigWindow(ConfigRegistry* registry, ConfigManager* config);
    ~ConfigWindow() override;

  public:
    /**
     * @brief Reloads all editor values from ConfigManager, each one from the key
     * it was built for; items removed from the registry in the meantime are left
     * untouched.
     */
    void refresh();
    /**
     * @brief Restores defaults: writes the default value where present,
     * otherwise removes the key.
     */
    void reset();

    /**
     * @brief The associated registry.
     */
    raw_ptr<ConfigRegistry> registry() const;
    /**
     * @brief The associated config manager.
     */
    raw_ptr<ConfigManager> config() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
