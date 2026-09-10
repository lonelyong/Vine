#pragma once

#include "Control.hpp"
#include "Gui.hpp"

V_APPFWGUI_NS_BEGIN

/**
 * @brief Top-level window base class: wraps a native QWidget (top-level
 * window/dialog) and provides window-level capabilities.
 *
 * Inherits Control (enabled/visible/tooltip/size and other common control
 * properties) and adds window semantics: title (windowTitle), show/close
 * (show/close), modality (modal), size (resize). exec() applies only to
 * windows whose native widget is a QDialog (modal run). Derived by window
 * classes such as ConfigWindow and MainWindow; derived classes build their
 * content into impl<QWidget>().
 */
class V_APPFW_API Window : public Control {
    V_OBJECT_META_DECL

  public:
    explicit Window(QWidget* native, bool owns = true);
    ~Window() override;

  public:
    /// Window title.
    void   setWindowTitle(const String& t);
    String windowTitle() const;
    /// Window modality (setWindowModality).
    void setModal(bool on);
    /// Whether the window is modal (windowModality).
    bool modal() const;
    /// Shows the window (non-modal).
    void show();
    /// Closes the window.
    void close();
    /// Runs modally (blocks until closed), returns the dialog result code.
    int exec();
    /// Sets the window size (in pixels).
    void resize(int w, int h);

  public:
    /// Window state (minimized/maximized/normal).
    void        setWindowState(WindowState state);
    WindowState windowState() const;
    /// Initial window position (currently only recorded; applied on show as needed).
    void            setStartupPosition(StartupPosition position);
    StartupPosition startupPosition() const;
    /// Activates the window (brings it to the foreground).
    void activate();
    /// Whether the window is active.
    bool isActive() const;

  protected:
    // Derived classes pass custom Impl to keep the inheritance chain extensible.
    Window(UIElementData* data, QWidget* native, bool owns = true);

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
