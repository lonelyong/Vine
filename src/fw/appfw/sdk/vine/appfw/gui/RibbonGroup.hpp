#pragma once

#include <vine/raw_ptr.hpp>

#include "Control.hpp"
#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

class RibbonButton;
class RibbonAction;
class Control;

/**
 * @brief Ribbon group (wraps SARibbonPanel): a container for a set of
 * buttons/controls.
 *
 * Supports a title, adding/removing buttons, separators, layout modes
 * (three-row/two-row/single-row), panel-level batch icon sizes and styles,
 * and a top-right option button.
 *
 * @note This header includes and exposes no Qt types. Panel-level global
 * properties such as title height and spacing are managed uniformly by
 * SARibbonBar and are not wrapped here.
 */
class VN_APPFW_API RibbonGroup : public Control {
    VN_OBJECT_META_DECL

  public:
    RibbonGroup();
    virtual ~RibbonGroup();

  public:
    /// Sets the group title.
    void setTitle(const String& t);
    /// Gets the group title.
    String title() const;

  public:
    /// Adds a button (row proportion chosen automatically by button size:
    /// large occupies a full row / medium two rows / small one row).
    void addButton(RibbonButton* b);
    /// Removes a button (detaches it from the panel; the underlying control
    /// is released later).
    void removeButton(RibbonButton* b);
    /// Adds a generic control container (any native control; row proportion
    /// same as addButton).
    void addControl(Control* w, RibbonItemSize size);
    /// Removes a generic control container.
    void removeControl(Control* w);
    /// Adds a separator between buttons.
    void addSeparator();

  public:
    /**
     * @brief Sets the panel layout mode: three rows / two rows / single row.
     *
     * @note When the panel is added to a RibbonBar, the BAR's default layout
     * mode overrides any previously set value.
     */
    void                  setLayoutMode(RibbonPanelLayoutMode m);
    RibbonPanelLayoutMode layoutMode() const;
    /// Sets whether the panel expands horizontally (for expandable controls
    /// such as Gallery).
    void setExpanding(bool on);
    bool expanding() const;
    /// Sets whether the user may right-click to customize the panel.
    void setCanCustomize(bool on);
    bool canCustomize() const;
    /// Panel-level large-button icon size (batch setting).
    void setLargeIconSize(const Size& s);
    Size largeIconSize() const;
    /// Panel-level small-button icon size (batch setting).
    void setSmallIconSize(const Size& s);
    Size smallIconSize() const;
    /**
     * @brief Panel-level batch setting: whether text is placed to the right of
     * the icon.
     *
     * Cascades to and overrides the iconRightText state of all buttons in the
     * panel.
     *
     * @note Applies only to RibbonButton inside the panel; plain controls
     * added via addControl are unaffected. This writes the same underlying
     * state as RibbonButton::setIconRightText: a group-level call overrides
     * button-level settings, after which per-button calls can fine-tune until
     * the next group-level call. Reading returns the panel-level flag (the
     * batch setting), not per-button fine-tuning results.
     */
    void setIconRightText(bool on);
    bool iconRightText() const;
    /**
     * @brief Panel-level batch setting: whether button text auto-wraps.
     *
     * @note Applies only to RibbonButton inside the panel; plain controls
     * added via addControl are unaffected. SARibbonPanel::setEnableWordWrap is
     * protected (for internal synchronization only), so here we iterate over
     * the buttons that **already exist** in the panel and set them one by one;
     * **buttons added later will not inherit** the setting. A group-level call
     * overrides button-level wordWrap; reading returns the current setting
     * (stored in Impl).
     */
    void setWordWrap(bool on);
    bool wordWrap() const;

  public:
    /**
     * @brief Sets the option button; pass nullptr to clear it.
     *
     * @note The panel does not own the QAction; when the passed item is
     * destroyed the button is cleared automatically.
     */
    void          setOptionAction(RibbonAction* item);
    raw_ptr<RibbonAction> optionAction() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
