#pragma once

#include <vine/appfw/appfw_global.hpp>

#include <QObject>

#include <vine/String.hpp>

VN_APPFWGUI_NS_BEGIN

struct UIElementData {
    String                  name;
    QObject*                impl         = nullptr;
    bool                    impl_deleted = false;
    bool                    owns_impl    = true;
    QMetaObject::Connection impl_destroyed_connection;

    virtual ~UIElementData() = default;
};

VN_APPFWGUI_NS_END
