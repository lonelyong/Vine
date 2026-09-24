#pragma once

#include "Control.hpp"

VN_APPFWGUI_NS_BEGIN

class VN_APPFW_API StatusBar : public Control {
    VN_OBJECT_META_DECL

  public:
    StatusBar();
    StatusBar(UIElement* parent);
    virtual ~StatusBar();

    void showMessage(const String& msg, int timeout_ms = 0);

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
