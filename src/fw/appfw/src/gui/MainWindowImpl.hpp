#pragma once

#include <QShowEvent>
#include <SARibbon.h>

#include <vine/Signal.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>

VN_APPFWGUI_NS_BEGIN

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

    /// Connection to the application theme; cancelling it is the handle's job.
    Connection theme_handler_{};
};

VN_APPFWGUI_NS_END
