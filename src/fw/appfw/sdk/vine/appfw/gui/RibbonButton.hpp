#pragma once

#include <vector>

#include "Control.hpp"
#include "Gui.hpp"
#include "Icon.hpp"

V_APPFWGUI_NS_BEGIN

class RibbonAction;

/**
 * @brief Ribbon button (wraps SARibbonToolButton).
 *
 * Supports text, icon, common impl method wrappers, and a drop-down list:
 * once a RibbonAction is added, a QMenu is built automatically and attached
 * to the button (InstantPopup).
 *
 * @note This header includes and exposes no Qt types (icons are bridged via
 * Icon). Drop-down ownership rules: after an item's QAction is added to the
 * menu it is owned by the menu (the item is set to not own its impl); call
 * removeDropDownItem() before deleting an item. Separators are inserted via
 * addSeparator() and are not counted by dropDownItemCount()/dropDownItemAt();
 * to remove items or separators by position, use
 * dropDownEntryCount()/removeDropDownEntryAt() (unified counting).
 */
class V_APPFW_API RibbonButton : public Control {
    V_OBJECT_META_DECL

  public:
    RibbonButton();
    ~RibbonButton() override;

  public:
    /// Sets the button text.
    void setText(const String& t);
    /// Gets the button text.
    String text() const;

    /// Sets the button icon (bridged via Icon, no Qt exposed).
    void setIcon(const Icon& ic);
    /// Gets the button icon.
    Icon icon() const;

    /// Sets whether the button is checkable (toggle state).
    void setCheckable(bool on);
    bool checkable() const;
    /// Sets whether the button is checked (requires setCheckable(true) first).
    void setChecked(bool on);
    bool checked() const;
    /**
     * @brief Sets the button size: large / medium / small.
     *
     * @note Medium is expressed through the panel row proportion; the button
     * itself renders as a small button (SARibbon has only two levels).
     */
    void           setButtonSize(RibbonItemSize s);
    RibbonItemSize buttonSize() const;

    /**
     * @brief Sets the arrangement of icon and text (including icon-only).
     *
     * @note Large buttons are "icon on top, text below" by default; style()
     * mainly affects small buttons or special arrangements.
     */
    void              setStyle(RibbonButtonStyle s);
    RibbonButtonStyle style() const;
    /// Sets the large-button icon size (used with setButtonSize(Large)).
    void setLargeIconSize(const Size& s);
    Size largeIconSize() const;
    /// Sets the small-button icon size.
    void setSmallIconSize(const Size& s);
    Size smallIconSize() const;
    /**
     * @brief Whether this button's text auto-wraps (common for large buttons).
     *
     * @note Overridden by RibbonGroup::setWordWrap (panel-level batch); the
     * group-level setting affects only buttons that exist at that time, and
     * buttons added later do not inherit it.
     */
    void setWordWrap(bool on);
    bool wordWrap() const;
    /**
     * @brief Whether this button's text is shown to the right of the icon.
     *
     * @note Overridden by RibbonGroup::setIconRightText (panel-level batch).
     */
    void setIconRightText(bool on);
    bool iconRightText() const;

    /// Appends a drop-down item (builds a QMenu and attaches it to the button,
    /// InstantPopup).
    void addDropDownItem(RibbonAction* item);
    /// Removes a drop-down item (returns its QAction ownership).
    void removeDropDownItem(RibbonAction* item);
    /// Clears all drop-down items (including separators).
    void clearDropDownItems();
    /// Inserts a separator at the end of the drop-down menu (not counted as an item).
    void addSeparator();
    /// Number of drop-down items (excluding separators).
    size_t dropDownItemCount() const;
    /// Gets the item by drop-down index (excludes separators; out of range
    /// returns nullptr).
    RibbonAction* dropDownItemAt(size_t i) const;
    /// Total entry count (including separators), used with removeDropDownEntryAt.
    size_t dropDownEntryCount() const;
    /// Removes by total entry index (items/separators counted uniformly,
    /// 0-based; out of range is ignored).
    void removeDropDownEntryAt(size_t i);

    /// Binds a user custom data pointer (the framework does not manage its lifetime).
    void  setData(void* dptr);
    void* data() const;

    /// Sets the command name executed when the button is clicked (empty = none).
    void setCommand(const String& command);
    /// Gets the configured command name.
    String command() const;

  public:
    /// Triggered when the button is clicked.
    Event<RibbonButton, EventArgs> clicked;

  private:
    void rebuildMenu();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
