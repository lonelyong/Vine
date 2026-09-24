#pragma once

#include <vine/appfw/AppBuilder.hpp>

VN_APPFWGUI_NS_BEGIN

class GuiApplication;

/**
 * @brief Builds a GUI application from config.
 *
 * Applies the same settings as createApplication() and returns a
 * GuiApplication instead.
 *
 * @param config Application configuration.
 * @param argc Command line argument count.
 * @param argv Command line arguments.
 * @return The initialized GUI application.
 */
VN_APPFW_API std::unique_ptr<GuiApplication> createGuiApplication(const AppConfig& config, int argc, char** argv);

VN_APPFWGUI_NS_END
