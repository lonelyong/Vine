#pragma once

#include <utility>

#include <vine/String.hpp>
#include <vine/appfw/CommandManager.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief RAII: attributes the registrations made inside the scope to one plugin.
 *
 * @details
 * - The tag is read once per registration (CommandManager::registerCommand()), so it has to be set while the
 *   registration runs and restored afterwards; this class is the one place that does both.
 * - The previous tag is restored rather than cleared: an outer registration (the application's own, or a nested
 *   plugin hook) keeps the attribution it had.
 *
 * @warning
 * - The scope must not span a suspension point. While a plugin hook is suspended the main thread may deliver other
 *   work, and every registration made there would inherit this scope's owner. Attribute a hook's own registrations
 *   with PluginLoadContext::registerCommand(), which sets the tag around one call.
 */
class RegistrationOwnerScope {
  public:
    /**
     * @brief Sets the registration owner for the scope's lifetime.
     *
     * @param manager Command manager whose application carries the tag; nullptr is tolerated.
     * @param owner   Plugin name to attribute registrations to.
     */
    RegistrationOwnerScope(CommandManager* manager, String owner)
      : manager_(manager)
    {
        if (manager_ != nullptr) {
            previous_ = manager_->registrationOwner();
            manager_->setRegistrationOwner(std::move(owner));
        }
    }

    /// Restores the registration owner the scope replaced.
    ~RegistrationOwnerScope()
    {
        if (manager_ != nullptr) {
            manager_->setRegistrationOwner(std::move(previous_));
        }
    }

    RegistrationOwnerScope(const RegistrationOwnerScope&)            = delete;
    RegistrationOwnerScope& operator=(const RegistrationOwnerScope&) = delete;

  private:
    CommandManager* manager_;
    String          previous_;
};

VN_APPFW_NS_END
