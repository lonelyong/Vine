#include <vine/appfw/gui/UIElement.hpp>

#include <QAction>

#include <vine/appfw/gui/UIElementData.hpp>


VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(UIElement, Object)

namespace
{} // namespace

UIElement::UIElement(UIElementData* data, QObject* impl)
  : d(data)
{
    d->impl = impl;

    // A null impl is valid for detached elements (e.g. DockPanel) whose native
    // widget is attached later; skip the destroyed hook in that case.
    if (impl) {
        d->impl_destroyed_connection = QObject::connect(impl, &QObject::destroyed, [this, data](QObject* obj) {
            data->impl         = nullptr; // prevent dtor from double-deleting
            data->impl_deleted = true;
            if (data->owns_impl) {
                delete this; // self-destruct when impl goes away
            }
        });
    }
}

UIElement::UIElement(UIObject* impl)
  : d(new UIElementData())
{
    d->impl             = impl;
    UIElementData* dptr = d;
    if (impl) {
        d->impl_destroyed_connection = QObject::connect(impl, &QObject::destroyed, [this, dptr](QObject* obj) {
            dptr->impl         = nullptr; // prevent dtor from double-deleting
            dptr->impl_deleted = true;
            if (dptr->owns_impl) {
                delete this; // self-destruct when impl goes away
            }
        });
    }
}

UIElement::~UIElement()
{
    if (d->impl) {
        QObject::disconnect(d->impl_destroyed_connection);
        if (d->owns_impl) {
            delete d->impl;
        }
    }
    delete d;
}

String UIElement::name() const
{
    return d->name;
}

void UIElement::setName(const String& name)
{
    d->name = name;
}

QObject* UIElement::impl() const
{
    return d->impl;
}

void UIElement::setOwnsImpl(bool owns)
{
    d->owns_impl = owns;
}

VN_APPFWGUI_NS_END
