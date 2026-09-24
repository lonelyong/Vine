#pragma once

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
    /// Sets whether the action is enabled.
    void setEnabled(bool on);
    bool enabled() const;
    /// Sets whether the action is checkable (toggle state).
    void setCheckable(bool on);
    bool checkable() const;
    /// Sets whether the action is checked (requires checkable(true) first).
    void setChecked(bool on);
    bool checked() const;

  public:
    /// Binds a user custom data pointer (the framework does not manage its lifetime).
    void  setData(void* dptr);
    void* data() const;

    /// Sets the command name executed when the action is triggered (empty = none).
    void setCommand(const String& command);
    /// Gets the configured command name.
    String command() const;

  public:
    /// Triggered when the action is triggered (e.g. from a menu).
    Event<RibbonAction, EventArgs> triggered;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
