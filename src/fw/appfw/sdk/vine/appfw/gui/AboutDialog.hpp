#pragma once

#include <vine/appfw/gui/Window.hpp>

VN_APPFWGUI_NS_BEGIN

/**
 * @brief About dialog skeleton.
 *
 * Shows an application name, version, description and copyright notice. The
 * framework only provides the frame and placeholder text; each application (or
 * plugin) sets its own content through the setters before showing it.
 */
class VN_APPFW_API AboutDialog : public Window {
    VN_OBJECT_META_DECL;

  public:
    AboutDialog();
    ~AboutDialog() override;

  public:
    void   setAppName(const String& name);
    String appName() const;

    void   setVersion(const String& version);
    String version() const;

    void   setDescription(const String& description);
    String description() const;

    void   setCopyright(const String& copyright);
    String copyright() const;

  private:
    void applyContent();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
