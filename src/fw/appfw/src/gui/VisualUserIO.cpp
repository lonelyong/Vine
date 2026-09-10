#include "VisualUserIO.hpp"

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>

#include <vine/async/DetachedTask.hpp>

V_APPFWGUI_NS_BEGIN

V_OBJECT_META_IMPL(VisualUserIO, UserIO)

VisualUserIO::VisualUserIO() = default;

VisualUserIO::~VisualUserIO() = default;

void VisualUserIO::setConsolePanel(ConsolePanel* console)
{
    console_ = console;
    if (!console_)
    {
        return;
    }
    console_->lineEntered.addHandler([this](const String& text) { onLineEntered(text); });
    console_->escapePressed.addHandler([this] { onEscape(); });
    refreshCompletion();
}

void VisualUserIO::setCommandManager(vine::appfw::CommandManager* manager)
{
    UserIO::setCommandManager(manager);
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
    console_->setCommandEntries(entries);
}

void VisualUserIO::putString(const String& str)
{
    if (console_)
    {
        console_->append(ConsoleMessageType::Normal, str);
    }
}

void VisualUserIO::clear()
{
    if (console_)
    {
        console_->clear();
    }
}

void VisualUserIO::cancelPendingInput()
{
    // Same path as Escape: the awaiting read resumes with std::nullopt, so a command
    // parked on user input unwinds instead of holding the shutdown drain for its
    // whole bound.
    if (pending_ != PendingRead::None)
    {
        cancelInteraction();
    }
}

vine::async::Task<std::optional<String>> VisualUserIO::getStringAsync(const String& prompt)
{
    pending_       = PendingRead::String;
    currentPrompt_ = prompt;
    cancelled_     = false;
    done_.reset();

    if (console_)
    {
        console_->beginInput(prompt);
    }

    co_await done_;

    if (cancelled_)
    {
        pending_ = PendingRead::None;
        co_return std::nullopt;
    }
    pending_ = PendingRead::None;
    co_return stringResult_;
}

vine::async::Task<std::optional<int8_t>> VisualUserIO::getIntAsync(const String& prompt)
{
    pending_       = PendingRead::Int;
    currentPrompt_ = prompt;
    cancelled_     = false;
    done_.reset();

    if (console_)
    {
        console_->beginInput(prompt);
    }

    co_await done_;

    if (cancelled_)
    {
        pending_ = PendingRead::None;
        co_return std::nullopt;
    }
    pending_ = PendingRead::None;
    co_return intResult_;
}

vine::async::Task<std::optional<double>> VisualUserIO::getDoubleAsync(const String& prompt)
{
    pending_       = PendingRead::Double;
    currentPrompt_ = prompt;
    cancelled_     = false;
    done_.reset();

    if (console_)
    {
        console_->beginInput(prompt);
    }

    co_await done_;

    if (cancelled_)
    {
        pending_ = PendingRead::None;
        co_return std::nullopt;
    }
    pending_ = PendingRead::None;
    co_return doubleResult_;
}

vine::async::Task<std::optional<math::Point3d>> VisualUserIO::getPoint3dAsync(const String& prompt)
{
    pending_       = PendingRead::Point;
    currentPrompt_ = prompt;
    cancelled_     = false;
    done_.reset();

    if (console_)
    {
        console_->beginInput(prompt);
    }

    co_await done_;

    if (cancelled_)
    {
        pending_ = PendingRead::None;
        co_return std::nullopt;
    }
    pending_ = PendingRead::None;
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

    auto* dispatcher = Application::current() ? Application::current()->mainThreadDispatcher() : nullptr;
    if (dispatcher == nullptr || dispatcher->isMainThread() || !dispatcher->hasEventLoop())
    {
        console_->append(ConsoleMessageType::Error, message);
        return;
    }

    auto* panel = console_;
    static_cast<void>(dispatcher->postToMain([panel, message] { panel->append(ConsoleMessageType::Error, message); }));
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
        bool      ok = false;
        const int v  = text.trimmed().toInt(&ok);
        if (ok)
        {
            completeInt(static_cast<int8_t>(v));
        }
        else
        {
            repromptError(String(u8"请输入整数"));
        }
        break;
    }

    case PendingRead::Double:
    {
        bool   ok = false;
        double v  = text.trimmed().toDouble(&ok);
        if (ok)
        {
            completeDouble(v);
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
    if (console_)
    {
        console_->append(ConsoleMessageType::Error, message);
        console_->beginInput(currentPrompt_);
    }
}

void VisualUserIO::completeString(const String& value)
{
    stringResult_ = value;
    done_.set();
}

void VisualUserIO::completeInt(int8_t value)
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
