#pragma once

#include "Gui.hpp"
#include "UIElement.hpp"

class QWidget;

V_APPFWGUI_NS_BEGIN

/**
 * @brief Generic control container: wraps any native QWidget so it fits into
 * the framework's UIElement system.
 *
 * Used to attach third-party/custom controls (QComboBox, QLineEdit,
 * self-drawn controls, etc.) to ribbon groups, dock panels, or anywhere that
 * accepts a UIElement; it is also the common base class for control wrappers
 * such as RibbonButton/RibbonGroup/RibbonTab/RibbonBar/DockPanel/StatusBar,
 * uniformly providing QWidget-level common properties.
 *
 * @note This header only forward-declares QWidget and includes no Qt headers;
 * with owns=true (default) this container owns the native control. Once the
 * control is taken over by a host (e.g. SARibbonPanel), destroying the
 * control triggers this container's release automatically.
 */
class V_APPFW_API Control : public UIElement {
    V_OBJECT_META_DECL

  public:
    explicit Control(QWidget* native, bool owns = true);
    ~Control() override;

  public:
    /// Whether the control is enabled.
    void setEnabled(bool on);
    bool enabled() const;
    /// Whether the control is visible.
    void setVisible(bool on);
    bool visible() const;
    /// Tooltip text.
    void   setTooltip(const String& t);
    String tooltip() const;
    /// Control width.
    int width() const;
    /// Control height.
    int height() const;
    /// Control size.
    Size size() const;
    void setSize(const Size& s);

  protected:
    // Lets derived classes (RibbonButton, etc.) pass custom Impl to keep the
    // inheritance chain extensible.
    Control(UIElementData* data, QWidget* native, bool owns = true);

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
