#pragma once

#include "appfw_global.hpp"

#include <memory>

#include <vine/appfw/AppConfig.hpp>

VN_APPFW_NS_BEGIN

class Application;

/**
 * @brief Builds a headless application from config.
 *
 * Applies the plugin directory and then constructs the application, which does the rest of the work itself: the process
 * identity (name, organization), the Qt application object, the user IO and the configuration persistence.
 *
 * @param config Application configuration.
 * @param argc Command line argument count.
 * @param argv Command line arguments.
 * @return The initialized application.
 */
VN_APPFW_API std::unique_ptr<Application> createApplication(const AppConfig& config, int argc, char** argv);

VN_APPFW_NS_END
