#pragma once

#include <vine/appfw/AppInfo.hpp>

#include "Control.hpp"
#include "Gui.hpp"

VN_APPFWGUI_NS_BEGIN

/**
 * @brief Renders the application's about content: identity, build facts, links and credits.
 *
 * @details
 * - The panel is a renderer, not a store: it shows one AppInfo. By default that is Application::appInfo(), so a host
 *   that filled AppConfig::info while building the process gets the same text in the dialog and in a Backstage page
 *   without copying a single string.
 * - The same panel backs AboutDialog and the Backstage page that shows it through BackstagePage::setBody(), so both
 *   surfaces look alike by construction.
 * - Colours follow palette roles (no hard-coded colours), so the content follows the light/dark theme.
 *
 * @note The panel is a plain Control: it owns its native widget unless it was handed to a host
 *   (BackstagePage::setBody() takes that ownership).
 */
class VN_APPFW_API AboutPanel : public Control {
    VN_OBJECT_META_DECL

  public:
    /**
     * @brief Creates a panel showing Application::appInfo().
     *
     * @note Without an application (a headless test that builds the panel directly) the panel starts empty and shows
     *   what setAppInfo() gives it.
     */
    AboutPanel();
    ~AboutPanel() override;

  public:
    /**
     * @brief Shows this application info instead of the process-wide one.
     *
     * @details
     * - For a host that shows a second identity (an OEM variant, a plugin's own "about"): the normal path is to fill
     *   AppConfig::info once and let the panel read it.
     * - The whole content is replaced: facts, links and credits come from the new value, not from the old one.
     *
     * @param info Info to render; it is copied.
     */
    void setAppInfo(const AppInfo& info);

    /**
     * @brief Returns the info currently rendered.
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
