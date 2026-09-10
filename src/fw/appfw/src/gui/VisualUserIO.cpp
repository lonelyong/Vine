#include "VisualUserIO.hpp"

#include <cmath>

#include <QPointer>
#include <QWidget>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>

#include <vine/async/DetachedTask.hpp>
#include <vine/logging/Log.hpp>

V_APPFWGUI_NS_BEGIN

namespace
{

/// Runs fn on the application thread.
///
/// Every touch of the console panel in this file goes through here: the panel is a
/// QWidget, and the UserIO entry points may be called from any thread - a command
/// prints from whatever thread it resumed on, and a read may be started from one
/// too. The call runs inline when the caller already is on the application thread,
/// or when there is no event loop to marshal onto, so console ordering is unchanged
/// on the common path.
///
/// @param fn Callable to run; captured by value into the posted call.
template <typename TFn>
void onApplicationThread(TFn&& fn)
{
    auto* app        = Application::current();
    auto* dispatcher = app ? app->mainThreadDispatcher() : nullptr;
    if (dispatcher == nullptr || dispatcher->isMainThread() || !dispatcher->hasEventLoop()) {
        fn();
        return;
    }

    // Dropped when the event loop stops before it gets to the call: during a
    // shutdown nobody is left to read the panel anyway.
    static_cast<void>(dispatcher->postToMain(std::forward<TFn>(fn)));
}

/// Runs fn on the application thread with a live panel, or does nothing.
///
/// Every console touch goes through here: the call may be queued behind the event
/// loop, and both the panel and the owner that bound it (a window, a test) can be
/// gone by the time it runs. The QPointer turns that into a no-op instead of a write
/// into freed memory.
///
/// @param panel Panel to guard; must not be null.
/// @param fn    Callable taking the panel to touch.
template <typename TFn>
void onConsolePanel(ConsolePanel* panel, TFn&& fn)
{
    QPointer<QWidget> alive = panel->impl<QWidget>();
    onApplicationThread([panel, alive, fn = std::forward<TFn>(fn)]() mutable {
        if (alive) {
            fn(panel);
        }
    });
}

} // namespace

V_OBJECT_META_IMPL(VisualUserIO, UserIO)

VisualUserIO::VisualUserIO() = default;

VisualUserIO::~VisualUserIO() = default;

void VisualUserIO::setConsolePanel(ConsolePanel* console)
{
    if (console_ != nullptr) {
        // Drop the handlers of the panel being left behind: binding a panel twice
        // would otherwise run every entered line through onLineEntered() twice, and
        // an idle line would start its command twice.
        console_->lineEntered.removeHandler(line_handler_);
        console_->escapePressed.removeHandler(escape_handler_);
    }

    console_ = console;
    if (console_ == nullptr)
    {
        return;
    }
    line_handler_   = console_->lineEntered.addHandler([this](const String& text) { onLineEntered(text); });
    escape_handler_ = console_->escapePressed.addHandler([this] { onEscape(); });
    refreshCompletion();
}

void VisualUserIO::setCommandManager(vine::appfw::CommandManager* manager)
{
    if (auto* previous = commandManager(); previous != nullptr && previous != manager) {
        previous->commandsChanged.removeHandler(commands_handler_);
        commands_handler_ = 0;
    }

    UserIO::setCommandManager(manager);

    if (manager != nullptr && commands_handler_ == 0) {
        // The completion list is a snapshot: follow the command set so commands of
        // plugins that register after the console was bound still show up.
        commands_handler_ = manager->commandsChanged.addHandler(
            [this](vine::appfw::CommandManager&, vine::EventArgs&) { refreshCompletion(); });
    }
    refreshCompletion();
}

void VisualUserIO::refreshCompletion()
{
    if (!console_ || !commandManager())
    {
        return;
    }

    auto* pm = Application::current() ? Application::current()->pluginManager() : nullptr;

    std::vector<ConsoleCommandEntry> entries;
    for (const auto& info : commandManager()->commandInfos())
    {
        // A disabled command cannot run, so offering it for completion would only
        // lead to a failed execution.
        if (!info.enabled)
        {
            continue;
        }

        // The popup prefix is the plugin's human-friendly display name.
        String source = info.owner;
        if (pm != nullptr && !info.owner.empty())
        {
            if (auto* plugin = pm->plugin(info.owner))
            {
                const auto pinfo = plugin->info();
                source = pinfo.display_name.empty() ? pinfo.name : pinfo.display_name;
            }
        }
        entries.push_back(ConsoleCommandEntry{ info.name, info.description, info.aliases, source });
    }

    auto* panel = console_;
    onConsolePanel(panel, [entries = std::move(entries)](ConsolePanel* target) { target->setCommandEntries(entries); });
}

void VisualUserIO::putString(const String& str)
{
    if (console_ == nullptr)
    {
        return;
    }

    // Commands print from whatever thread they resumed on, so the panel write is
    // marshalled: the panel is a QWidget and only the application thread may touch
    // it. Calling this from any thread is therefore safe.
    auto* panel = console_;
    onConsolePanel(panel, [str](ConsolePanel* target) { target->append(ConsoleMessageType::Normal, str); });
}

void VisualUserIO::clear()
{
    if (console_ == nullptr)
    {
        return;
    }

    auto* panel = console_;
    onConsolePanel(panel, [](ConsolePanel* target) { target->clear(); });
}

void VisualUserIO::cancelPendingInput()
{
    // Same path as Escape: the awaiting read resumes with std::nullopt, so a command
    // parked on user input unwinds instead of holding the shutdown drain for its
    // whole bound. Application::shutdown() calls this while the event loop is already
    // stopped, and it may be called from another thread, so nothing here may end up
    // touching the panel.
    if (pending_.load() != PendingRead::None)
    {
        cancelInteraction();
    }
}

bool VisualUserIO::beginRead(PendingRead kind, const String& prompt)
{
    PendingRead expected = PendingRead::None;
    if (!pending_.compare_exchange_strong(expected, kind))
    {
        V_LOGW("A user-input read is already waiting; refusing the new one");
        return false;
    }

    currentPrompt_ = prompt;
    cancelled_.store(false);
    done_.reset();
    return true;
}

void VisualUserIO::endRead() noexcept
{
    pending_.store(PendingRead::None);
}

vine::async::Task<bool> VisualUserIO::waitForInput(PendingRead kind, const String& prompt)
{
    if (!beginRead(kind, prompt))
    {
        co_return false;
    }
    const ReadScope scope{ this }; // frees the interaction slot however this ends

    if (console_ != nullptr)
    {
        auto* panel = console_;
        onConsolePanel(panel, [prompt](ConsolePanel* target) { target->beginInput(prompt); });
    }

    co_await done_;
    co_return !cancelled_.load();
}

vine::async::Task<std::optional<String>> VisualUserIO::getStringAsync(const String& prompt)
{
    if (!co_await waitForInput(PendingRead::String, prompt))
    {
        co_return std::nullopt;
    }
    co_return stringResult_;
}

vine::async::Task<std::optional<int>> VisualUserIO::getIntAsync(const String& prompt)
{
    if (!co_await waitForInput(PendingRead::Int, prompt))
    {
        co_return std::nullopt;
    }
    co_return intResult_;
}

vine::async::Task<std::optional<double>> VisualUserIO::getDoubleAsync(const String& prompt)
{
    if (!co_await waitForInput(PendingRead::Double, prompt))
    {
        co_return std::nullopt;
    }
    co_return doubleResult_;
}

vine::async::Task<std::optional<math::Point3d>> VisualUserIO::getPoint3dAsync(const String& prompt)
{
    if (!co_await waitForInput(PendingRead::Point, prompt))
    {
        co_return std::nullopt;
    }
    co_return pointResult_;
}

void VisualUserIO::onLineEntered(const String& text)
{
    if (pending_ != PendingRead::None)
    {
        parseAndComplete(text);
        return;
    }

    if (commandManager() && commandManager()->runningCount() > 0)
    {
        if (console_)
        {
            console_->append(ConsoleMessageType::Warning, String(u8"命令正在执行，请稍候"));
        }
        return;
    }

    if (console_)
    {
        console_->append(ConsoleMessageType::Command, text);
    }

    if (commandManager())
    {
        // 异步启动命令；失败信息在命令完成后回写。
        // 命令可能在任意线程上结束（await 了定时器/异步读取），因此回写必须
        // 编组到应用线程：控制台面板是 QWidget，只有应用线程可以碰。
        [](VisualUserIO* self, vine::async::Task<CommandResult> task) -> vine::async::DetachedTask {
            const auto result = co_await std::move(task);
            if (result.succeeded())
            {
                co_return;
            }

            const auto& message = result.message();
            self->appendOnApplicationThread(message.empty() ? String(u8"命令执行失败") : message);
        }(this, commandManager()->executeCommandAsync(text));
    }
}

void VisualUserIO::appendOnApplicationThread(const String& message)
{
    if (!console_)
    {
        return;
    }

    auto* panel = console_;
    onConsolePanel(panel, [message](ConsolePanel* target) { target->append(ConsoleMessageType::Error, message); });
}

void VisualUserIO::onEscape()
{
    if (pending_ != PendingRead::None)
    {
        cancelInteraction();
    }
    else if (commandManager() && commandManager()->runningCount() > 0)
    {
        // Cancel the foreground command chain (the one the console just showed as
        // running). Chains pushed out of the foreground are only reachable through
        // cancelAll().
        commandManager()->cancelCurrent();
    }
    else if (console_)
    {
        console_->clearInput();
    }
}

void VisualUserIO::parseAndComplete(const String& text)
{
    switch (pending_)
    {
    case PendingRead::String:
        completeString(text);
        break;

    case PendingRead::Int:
    {
        int value = 0;
        if (parseInt(text, value))
        {
            completeInt(value);
        }
        else
        {
            repromptError(String(u8"请输入整数"));
        }
        break;
    }

    case PendingRead::Double:
    {
        bool         ok    = false;
        const double value = text.trimmed().toDouble(&ok);
        if (ok && std::isfinite(value))
        {
            completeDouble(value);
        }
        else
        {
            repromptError(String(u8"请输入数字"));
        }
        break;
    }

    case PendingRead::Point:
    {
        const auto parts = text.split(u8',');
        if (parts.size() == 3)
        {
            bool   xOk = false;
            bool   yOk = false;
            bool   zOk = false;
            double x   = parts[0].trimmed().toDouble(&xOk);
            double y   = parts[1].trimmed().toDouble(&yOk);
            double z   = parts[2].trimmed().toDouble(&zOk);
            if (xOk && yOk && zOk)
            {
                math::Point3d p;
                p.x = x;
                p.y = y;
                p.z = z;
                completePoint(p);
                break;
            }
        }
        repromptError(String(u8"请输入 x,y,z 格式的点"));
        break;
    }

    case PendingRead::None:
        break;
    }
}

void VisualUserIO::repromptError(const String& message)
{
    if (!console_)
    {
        return;
    }

    auto*        panel  = console_;
    const String prompt = currentPrompt_;
    onConsolePanel(panel, [message, prompt](ConsolePanel* target) {
        target->append(ConsoleMessageType::Error, message);
        target->beginInput(prompt);
    });
}

void VisualUserIO::completeString(const String& value)
{
    stringResult_ = value;
    done_.set();
}

void VisualUserIO::completeInt(int value)
{
    intResult_ = value;
    done_.set();
}

void VisualUserIO::completeDouble(double value)
{
    doubleResult_ = value;
    done_.set();
}

void VisualUserIO::completePoint(const math::Point3d& value)
{
    pointResult_ = value;
    done_.set();
}

void VisualUserIO::cancelInteraction()
{
    cancelled_ = true;
    done_.set();
}

V_APPFWGUI_NS_END
