#include "CommandBinding.hpp"

#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>
#include <vine/appfw/gui/MainWindow.hpp>

#include <vine/logging/Log.hpp>

VN_APPFWGUI_NS_BEGIN

namespace detail
{

struct CommandBinding::Lifetime {
};

CommandBinding::CommandBinding()
  : alive_(std::make_shared<Lifetime>())
{
}

CommandBinding::~CommandBinding()
{
    // Posting refreshes hold the lifetime weakly: a queued one that runs after the item is gone finds it expired.
    alive_.reset();
}

void CommandBinding::attach(Apply apply, ReadTooltip read_tooltip)
{
    apply_        = std::move(apply);
    read_tooltip_ = std::move(read_tooltip);
}

void CommandBinding::setCommand(const String& command)
{
    command_ = command;
    if (command_.empty()) {
        // Unbound again: stop listening and leave the item's current state alone.
        detachSubscriptions();
        return;
    }
    refresh();
}

const String& CommandBinding::command() const noexcept
{
    return command_;
}

void CommandBinding::setCheckedProvider(CheckedProvider provider)
{
    checked_provider_ = std::move(provider);
    refresh();
}

void CommandBinding::refresh()
{
    if (!apply_) {
        return;
    }

    // Touching the widget is the application thread's business; an executed notification can arrive anywhere.
    if (!MainThreadDispatcher::isMainThread()) {
        requestRefresh();
        return;
    }

    subscribeIfNeeded();

    auto* manager = Application::current() != nullptr ? Application::current()->commandManager() : nullptr;
    if (manager == nullptr || command_.empty()) {
        return;
    }

    bool enabled = manager->isCommandEnabled(command_);

    // NeedsDocument / NeedsView are the same declarations that turn a missing layer into NotApplicable at dispatch,
    // so the item shows "cannot run now" exactly when the command would be refused. This disables the ITEM only: the
    // persisted enabled preference is a different thing and is never written from here.
    if (const auto flags = manager->commandFlags(command_); enabled && flags) {
        const CommandTarget target = resolveCommandTarget(MainWindow::current());
        if (vn::testFlag(*flags, CommandFlags::NeedsDocument) && target.document == nullptr) {
            enabled = false;
        }
        if (vn::testFlag(*flags, CommandFlags::NeedsView) && target.view == nullptr) {
            enabled = false;
        }
    }

    const String description = manager->commandDescription(command_).value_or(String());
    apply_(enabled, tooltipFor(description), checkedNow());
}

void CommandBinding::subscribeIfNeeded()
{
    auto* app       = Application::current();
    auto* manager   = app != nullptr ? app->commandManager() : nullptr;
    auto* documents = app != nullptr ? app->documentManager() : nullptr;

    if (manager != subscribed_manager_) {
        // A stale connection to a destroyed manager is inert (Connection holds the slot weakly, see Signal), so
        // dropping the handles first is enough; the new manager answers the same two questions.
        commands_changed_.disconnect();
        command_executed_.disconnect();
        subscribed_manager_ = manager;
        if (manager != nullptr) {
            commands_changed_ = manager->commandsChanged.connect(
                [this](CommandManager&, EventArgs&) { requestRefresh(); });
            command_executed_ = manager->executed.connect(
                [this](CommandManager&, CommandExecutedEventArgs&) { requestRefresh(); });
        }
    }

    if (documents != subscribed_documents_) {
        current_document_changed_.disconnect();
        subscribed_documents_ = documents;
        if (documents != nullptr) {
            current_document_changed_ = documents->currentChanged.connect(
                [this](DocumentManager&, DocumentEventArgs&) { requestRefresh(); });
        }
    }
}

void CommandBinding::detachSubscriptions()
{
    commands_changed_.disconnect();
    command_executed_.disconnect();
    current_document_changed_.disconnect();
    subscribed_manager_   = nullptr;
    subscribed_documents_ = nullptr;
}

void CommandBinding::requestRefresh()
{
    if (MainThreadDispatcher::isMainThread()) {
        refresh();
        return;
    }

    std::weak_ptr<Lifetime> alive = alive_;
    MainThreadDispatcher::postToMainThread([this, alive] {
        if (alive.expired()) {
            return; // the item is gone
        }
        refresh();
    });
}

String CommandBinding::tooltipFor(const String& description)
{
    const String current = read_tooltip_ ? read_tooltip_() : String();

    // The caller's own tooltip wins: only text this binding wrote is updated or cleared again.
    if (current != last_tooltip_) {
        return current;
    }

    last_tooltip_ = description;
    return description;
}

std::optional<bool> CommandBinding::checkedNow()
{
    if (!checked_provider_) {
        return std::nullopt;
    }

    try {
        return checked_provider_();
    }
    catch (...) {
        VN_LOGW("A checked-state provider threw; the item keeps its previous toggle state");
        return std::nullopt;
    }
}

} // namespace detail

VN_APPFWGUI_NS_END
