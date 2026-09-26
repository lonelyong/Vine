#pragma once

#include "Control.hpp"
#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

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
class VN_APPFW_API Window : public Control {
    VN_OBJECT_META_DECL

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

    /**
     * @brief Returns whether the window has painted at least once.
     *
     * A window that is up is not a window that has painted: painting it waits for the window system's "it is visible
     * now" notice, which arrives through the event queue, and a boot does not run it (it repaints and reports its
     * progress directly). A window shown during one is therefore an empty window on screen - black under X11,
     * transparent for a translucent frame - until the queue has been dispatched; measured under WSLg, both of the
     * framework's startup windows spent their whole boot that way.
     *
     * The paint of the window itself and the paint of anything inside it both count: which of them comes first is the
     * layout's business, and a window whose area is covered by opaque children paints none of it itself.
     *
     * @return true once the window has painted, false while it has not.
     */
    bool hasPainted() const noexcept;

  protected:
    // Derived classes pass their own data block, deriving from WindowData (see the private
    // WindowData.hpp), so the data hierarchy mirrors the widget hierarchy.
    Window(UIElementData* data, QWidget* native, bool owns = true);
};

VN_APPFWGUI_NS_END
