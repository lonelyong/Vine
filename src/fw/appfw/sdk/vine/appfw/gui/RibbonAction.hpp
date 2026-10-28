#pragma once

#include <functional>

#include "Icon.hpp"
#include "UIElement.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Ribbon action (wraps QAction).
 *
 * An executable command/action: can be attached to a RibbonButton drop-down
 * menu, the RibbonBar application menu, or used as a RibbonGroup option button.
 *
 * @note This header includes and exposes no Qt types (icons are bridged via
 * Icon); once added to a menu, its QAction is owned by the menu (see the
 * ownership notes in RibbonButton).
 */
class VN_APPFW_API RibbonAction : public UIElement {
    VN_OBJECT_META_DECL

  public:
    RibbonAction();
    virtual ~RibbonAction();

  public:
    /// Sets the action text.
    void setText(const String& t);
    /// Gets the action text.
    String text() const;

    /// Sets the action icon (bridged via Icon, no Qt exposed).
    void setIcon(const Icon& ic);
    /// Gets the action icon.
    Icon icon() const;

    /// Sets the tooltip text.
    void   setTooltip(const String& t);
    String tooltip() const;

    /**
     * @brief Sets whether the action is enabled.
     *
     * @note On an action bound to a command (setCommand()) the enabled state is DERIVED from the command manager and
     * overwritten at the next refresh; use CommandManager::setCommandEnabled() to disable the command for good.
     *
     * @param on true to enable the action.
     */
    void setEnabled(bool on);

    /**
     * @brief Gets whether the action can be triggered.
     *
     * @return true when the action is enabled.
     */
    bool isEnabled() const;

    /// Sets whether the action is checkable (toggle state).
    void setCheckable(bool on);

    /**
     * @brief Gets whether the action has a toggle state.
     *
     * @return true when the action is checkable.
     */
    bool isCheckable() const;

    /// Sets whether the action is checked (requires checkable(true) first).
    void setChecked(bool on);

    /**
     * @brief Gets whether the action is currently checked.
     *
     * @return true when the checkable action is checked.
     */
    bool isChecked() const;

  public:
    /// Binds a user custom data pointer (the framework does not manage its lifetime).
    void  setData(void* dptr);
    void* data() const;

    /**
     * @brief Sets the command name executed when the action is triggered (empty = unbinds the action).
     *
     * @details
     * - A bound action is a view of the command: its enabled state follows the command manager (registration, the
     *   user's enabled flag, and whether the command's NeedsDocument/NeedsView prerequisite is met right now), and
     *   its tooltip follows the command description.
     * - This is what a ribbon drop-down entry, an application-menu entry and a Backstage item use: the command name
     *   is the item's identity.
     *
     * @param command Registered command (or alias) name, or empty for "not a command action".
     */
    void setCommand(const String& command);
    /// Gets the configured command name.
    String command() const;

    /**
     * @brief Lets the caller answer the toggle state of a command action.
     *
     * @details
     * - Once a provider is set the action becomes checkable, and the provider is asked again whenever the action
     *   refreshes (the command state changed, a command finished, the current document changed).
     * - The provider belongs to the bound command: with no command set there is nothing to refresh, so it takes
     *   effect once setCommand() names one. Without a provider the binding leaves the toggle to the caller.
     *
     * @param provider "is this mode on now"; an empty function gives the toggle back to the caller.
     */
    void setCheckedProvider(std::function<bool()> provider);

  public:
    /// Triggered when the action is triggered (e.g. from a menu).
    Event<RibbonAction, EventArgs> triggered;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
