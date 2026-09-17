#pragma once

#include <QShowEvent>
#include <SARibbon.h>

#include <vine/Signal.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

V_APPFWGUI_NS_BEGIN

class MainWindowImpl : public SARibbonMainWindow {
  public:
    explicit MainWindowImpl(QWidget* parent = nullptr);
    ~MainWindowImpl() override;

  public:
    void applyAppTheme();

  protected:
    // Guards the one-shot default startup placement (open on the primary
    // screen) applied when the window is first shown.
    bool startup_placed_ = false;

    /// Subscription to the application theme; cancelling it is the handle's job.
    Signal<Theme>::Subscription theme_handler_{};
};

V_APPFWGUI_NS_END
