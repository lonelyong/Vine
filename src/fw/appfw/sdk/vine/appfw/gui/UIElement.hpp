#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <vine/Events.hpp>
#include <vine/Object.hpp>
#include <vine/String.hpp>

class QObject;

V_APPFWGUI_NS_BEGIN
using UIObject = QObject;

struct UIElementData;

class V_APPFW_API UIElement : public Object {
    V_OBJECT_META_DECL

  protected:
    UIElement(UIElementData* data, UIObject* impl);
    UIElement(UIObject* impl);

  public:
    virtual ~UIElement();

  public:
    String       name() const;
    virtual void setName(const String& name);


    virtual UIObject* impl() const;

    template <typename TImpl>
    TImpl* impl() const
    {
        return (TImpl*)impl();
    }

    void setOwnsImpl(bool owns);

  public:
    const Event<UIElement, PropertyChangedEventArgs<String>> name_changed;

  protected:
    UIElementData* const d;
};

V_APPFWGUI_NS_END
