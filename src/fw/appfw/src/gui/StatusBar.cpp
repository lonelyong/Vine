#include <vine/appfw/gui/StatusBar.hpp>

#include <QStatusBar>
#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(StatusBar, Control)

struct StatusBar::Impl : public ControlData {};

StatusBar::StatusBar()
  : Control(new Impl(), new QStatusBar())
{}

StatusBar::StatusBar(UIElement* parent)
  : Control(new Impl(), new QStatusBar())
{
    // Attach to the parent widget if provided
    if (parent) {
        auto* pw = static_cast<QWidget*>(parent->impl());
        auto* sb = impl<QStatusBar>();
        if (pw && sb)
            sb->setParent(pw);
    }
}

StatusBar::~StatusBar()
{
    // d is deleted by UIElement
}

void StatusBar::showMessage(const String& msg, int timeout_ms)
{
    auto* sb = impl<QStatusBar>();
    if (!sb)
        return;
    auto utf16 = msg.toUtf16();
    sb->showMessage(QString::fromStdU16String(utf16));
}

inline auto StatusBar::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto StatusBar::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
