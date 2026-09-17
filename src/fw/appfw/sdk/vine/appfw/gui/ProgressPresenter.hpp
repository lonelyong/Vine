#pragma once

#include "Control.hpp"

class QWidget;

V_APPFWGUI_NS_BEGIN

/**
 * @brief Automatic main-thread progress bar.
 *
 * Subscribes to vine::appfw::ProgressHost::changed() and renders the top of the foreground stack
 * (the innermost long-running command) in a compact bar with a cancel button; when nested commands
 * push deeper, a breadcrumb ("parent > child") is shown; concurrent background hosts (outside the
 * foreground chain) are summarized by a small badge. The native widget is hidden by default,
 * appears automatically once an operation has been running past a short threshold, and hides
 * shortly after all operations end, so operations do not need to write any presentation code. A
 * foreground operation that reports no progress is shown as an indeterminate (busy) bar; the
 * cancel button stops the foreground operation.
 *
 * Nothing is polled: a change in the registry redraws the bar, and the two delays (appearing and
 * hiding) are the only timer the presenter arms - an idle window holds none at all. The
 * notification may arrive on any thread, because operations report from wherever they run, and is
 * marshalled to the application thread before a widget is touched.
 *
 * Embed the native widget (impl()) into a status bar, e.g. through
 * QStatusBar::addPermanentWidget(); Qt then owns the native widget and the presenter
 * self-destructs with it (UIElement ownership model).
 */
class V_APPFW_API ProgressPresenter : public Control {
    V_OBJECT_META_DECL

  public:
    explicit ProgressPresenter(QWidget* parent = nullptr);
    ~ProgressPresenter() override;

    /**
     * @brief Returns whether a progress-host-backed operation is running.
     *
     * @return true while the presenter is tracking an active host.
     */
    bool isBusy() const;

  private:
    /// Redraws the bar from the registry; application thread only.
    void refresh();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

V_APPFWGUI_NS_END
