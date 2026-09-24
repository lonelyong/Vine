#include <vine/appfw/gui/Control.hpp>

#include <QWidget>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(Control, UIElement)

Control::Control(QWidget* native, bool owns)
  : UIElement(new ControlData(), native)
{
    setOwnsImpl(owns);
}

Control::~Control()
{
    // d is deleted by UIElement
}

void Control::setEnabled(bool on)
{
    if (auto* w = impl<QWidget>())
        w->setEnabled(on);
}

bool Control::enabled() const
{
    auto* w = impl<QWidget>();
    return w && w->isEnabled();
}

void Control::setVisible(bool on)
{
    if (auto* w = impl<QWidget>())
        w->setVisible(on);
}

bool Control::visible() const
{
    auto* w = impl<QWidget>();
    return w && w->isVisible();
}

void Control::setTooltip(const String& t)
{
    auto* w = impl<QWidget>();
    if (!w)
        return;
    auto utf16 = t.toUtf16();
    w->setToolTip(QString::fromStdU16String(utf16));
}

String Control::tooltip() const
{
    auto* w = impl<QWidget>();
    if (!w)
        return {};
    auto qs = w->toolTip();
    return String::fromUtf16((const char16_t*)qs.utf16(), qs.size());
}

int Control::width() const
{
    auto* w = impl<QWidget>();
    return w ? w->width() : 0;
}

int Control::height() const
{
    auto* w = impl<QWidget>();
    return w ? w->height() : 0;
}

Size Control::size() const
{
    auto* w = impl<QWidget>();
    return w ? Size(w->width(), w->height()) : Size();
}

void Control::setSize(const Size& s)
{
    auto* w = impl<QWidget>();
    if (w)
        w->resize(QSize(s.x, s.y));
}

Control::Control(UIElementData* data, QWidget* native, bool owns)
  : UIElement(data, native)
{
    setOwnsImpl(owns);
}

VN_APPFWGUI_NS_END
