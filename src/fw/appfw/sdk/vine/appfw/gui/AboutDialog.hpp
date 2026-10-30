#pragma once

#include <vine/appfw/AppInfo.hpp>

#include "Gui.hpp"
#include "Window.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Non-modal-capable dialog that shows the application's about content.
 *
 * @details
 * - The dialog is a shell around AboutPanel: name, version, description, copyright, build facts, links and credits all
 *   come from one AppInfo, so the dialog and a Backstage page showing the same panel cannot disagree.
 * - By default it shows Application::appInfo(), which is what the host filled while building the process; a host that
 *   wants a second identity calls setAppInfo() before exec()/show().
 *
 * @note The dialog owns the panel it builds; there is no per-field setter, because the content belongs to the
 *   application (AppConfig::info), not to the dialog.
 */
class VN_APPFW_API AboutDialog : public Window {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Creates the dialog showing Application::appInfo().
     */
    AboutDialog();
    ~AboutDialog() override;

  public:
    /**
     * @brief Shows this application info instead of the process-wide one.
     *
     * @param info Info to render; it is copied into the dialog's panel.
     */
    void setAppInfo(const AppInfo& info);

    /**
     * @brief Returns the info the dialog currently shows.
     *
     * @return The rendered info, including the resolved name.
     */
    const AppInfo& appInfo() const;

  private:
    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
