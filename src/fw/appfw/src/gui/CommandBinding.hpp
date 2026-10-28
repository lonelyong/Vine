#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <vine/Events.hpp>
#include <vine/String.hpp>

#include <vine/appfw/Command.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/DocumentManager.hpp>

VN_APPFWGUI_NS_BEGIN

namespace detail
{

/**
 * @brief Keeps one command-bearing item in step with the command it names.
 *
 * @details
 * - A command item is a VIEW of a command: what it does is the command name it carries, and whether it can be
 *   triggered right now is the command manager's answer, not the item's own. This binding is that one answer, shared
 *   by RibbonButton and RibbonAction so the two cannot drift.
 * - It follows `CommandManager::commandsChanged()` (registration, the enabled flag, the description),
 *   `CommandManager::executed()` (a checked provider may answer differently once the command ran) and
 *   `DocumentManager::currentChanged()` (a `NeedsDocument`/`NeedsView` item depends on the target the window
 *   resolves).
 * - Disabling here is a DISPLAY state derived from "can this run now"; it never calls setCommandEnabled(), because
 *   that flag is the user's persisted preference (see CommandStatus::NotApplicable).
 * - An item with an empty command is unbound: it keeps whatever enabled/tooltip/checkable state its owner set,
 *   and the binding holds no subscriptions while it is unbound.
 * - The tooltip follows the command description while the caller leaves the tooltip alone; once the caller sets a
 *   different text, the binding stops writing it.
 * - The control owns the binding, and the binding's destructor cancels its subscriptions: an item may outlive the
 *   application.
 *
 * @note Application (GUI) thread only. A notification that arrives on another thread - an executed event can - is
 *       marshalled to the application thread before it touches the widget.
 */
class CommandBinding {
  public:
    /// Applies the derived state to the native item.
    using Apply = std::function<void(bool enabled, const String& tooltip, std::optional<bool> checked)>;

    /// Reads the tooltip currently on the native item, so a caller-set one can be told apart.
    using ReadTooltip = std::function<String()>;

    /// Answers "is this item's toggle on now"; empty means the item is not toggled through the binding.
    using CheckedProvider = std::function<bool()>;

  public:
    CommandBinding();
    ~CommandBinding();

    CommandBinding(const CommandBinding&)            = delete;
    CommandBinding& operator=(const CommandBinding&) = delete;

  public:
    /**
     * @brief Installs the item-side callbacks; called once, by the owning control's constructor.
     *
     * @param apply        Writes the derived state to the native item; must not be empty.
     * @param read_tooltip Reads the tooltip the native item carries now.
     */
    void attach(Apply apply, ReadTooltip read_tooltip);

    /**
     * @brief Binds the item to a command name; an empty name unbinds it again.
     *
     * @details
     * - The state is applied immediately, so a freshly bound item is correct before it is shown.
     * - Rebinding replaces the previous name; the subscriptions stay, they are about the manager, not the name.
     *
     * @param command Command or alias name, or empty for "not a command item".
     */
    void setCommand(const String& command);

    /**
     * @brief Returns the bound command name.
     *
     * @return The name, or an empty string when the item is not bound.
     */
    const String& command() const noexcept;

    /**
     * @brief Lets the caller answer the toggle state of a checkable command item.
     *
     * @details
     * - With a provider the item becomes checkable and its checked state is re-read whenever the binding refreshes;
     *   without one the binding leaves the toggle to the caller.
     * - The provider runs on the application thread, inside the refresh the item already pays for.
     *
     * @param provider "is this mode on now"; an empty function gives the toggle back to the caller.
     */
    void setCheckedProvider(CheckedProvider provider);

    /**
     * @brief Recomputes the state and applies it now; must run on the application thread.
     */
    void refresh();

  private:
    struct Lifetime;

    void subscribeIfNeeded();
    void detachSubscriptions();
    void requestRefresh();
    String tooltipFor(const String& description);
    std::optional<bool> checkedNow();

  private:
    String                    command_;
    Apply                     apply_;
    ReadTooltip               read_tooltip_;
    CheckedProvider           checked_provider_;
    String                    last_tooltip_;
    std::shared_ptr<Lifetime> alive_;
    Connection                commands_changed_;
    Connection                command_executed_;
    Connection                current_document_changed_;
    CommandManager*           subscribed_manager_   = nullptr;
    DocumentManager*          subscribed_documents_ = nullptr;
};

} // namespace detail

VN_APPFWGUI_NS_END
