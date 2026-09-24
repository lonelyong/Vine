#pragma once

#include <memory>

#include <vine/String.hpp>
#include <vine/appfw/appfw_global.hpp>

class QIcon;

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Icon object: holds a QIcon internally (hidden via Pimpl; the header
 * includes no Qt headers).
 *
 * Used by gui wrapper classes such as RibbonButton/RibbonAction to set icons,
 * so framework headers neither expose nor include Qt. Can be constructed from
 * an existing QIcon or a file path (String).
 *
 * @note This is the only bridge type in the framework that touches QIcon:
 * QIcon is only forward-declared in headers, and code that actually needs
 * QIcon (implementation files) must include <QIcon> itself.
 */
class VN_APPFW_API Icon {
  public:
    Icon();
    explicit Icon(const QIcon& qicon);
    explicit Icon(const String& path);
    Icon(const Icon& other);
    Icon& operator=(const Icon& other);
    ~Icon();

  public:
    /// The underlying QIcon (read-only).
    const QIcon& value() const;

    /// Whether the icon is null.
    bool isNull() const;

    /// Implicitly converts to QIcon.
    operator QIcon() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

VN_APPFWGUI_NS_END
