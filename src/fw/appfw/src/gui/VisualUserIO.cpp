#include <vine/appfw/gui/VisualUserIO.hpp>

#include <atomic>
#include <cassert>
#include <cmath>
#include <memory>
#include <utility>

#include <QPointer>
#include <QWidget>

#include <vine/Signal.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>

#include <vine/async/AsyncEvent.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/logging/Log.hpp>

VN_APPFWGUI_NS_BEGIN

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
    // The inline path is for when there is no queue to reach the application thread through (a headless run, the
    // application going down) and for the application thread itself; both questions are answered by the marshaller's
    // statics, so this neither holds nor dereferences the marshaller object.
    if (MainThreadDispatcher::isMainThread() || !MainThreadDispatcher::hasEventLoop()) {
        fn();
        return;
    }

    // Dropped when the event loop stops before it gets to the call: during a
    // shutdown nobody is left to read the panel anyway.
    static_cast<void>(MainThreadDispatcher::postToMainThread(std::forward<TFn>(fn)));
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

VN_OBJECT_META_IMPL(VisualUserIO, UserIO)

/// Everything the visual I/O owns: the panel it writes to, the interaction slot, and the bookkeeping of the read that is
/// waiting for the user.
///
/// Nested and defined here rather than private members on the class, so that the public header stays free of the state
/// and of the helpers below - a host binds a console panel, it has no business with the rest (VisualUserIO.hpp).
struct VisualUserIO::Impl
{
    enum class PendingRead
    {
        None,
        String,
        Int,
        Double,
        Point,
        File,
        Files,
        Dir,
        Dirs
    };

    /// What a read shows and what the input line offers while it waits.
    ///
    /// Built by the getXxxAsync() entry points, because that is where the options live; the prompt text arrives already
    /// woven with the requirements so the visual and the headless implementations show the same words.
    struct ReadUi {
        ConsoleInputAction action;            ///< Picker the input line offers; kind == None for a plain read.
        String             prompt;            ///< Prompt text to show.
        String             initial_text;      ///< Text to pre-fill the input line with (the caller's suggestion).
        FileReadOptions    file_options;      ///< What the answer has to look like; only the file kinds use it.
        DirReadOptions     dir_options;       ///< What the answer has to look like; only the directory kinds use it.
    };

    /// Releases the interaction slot when the read ends - however it ends,
    /// including a coroutine frame that is destroyed before it ever resumed.
    struct ReadScope {
        Impl* self;
        ~ReadScope() { self->endRead(); }
    };

    /// Prompt bookkeeping of the read that is waiting for input.
    ///
    /// Read and written on the application thread only: the write happens inside
    /// the marshalled UI call that shows the prompt, and the read inside
    /// repromptError(). It is shared rather than a plain member so a posted UI call
    /// can write it without capturing this object, which may be gone by the time
    /// the call runs (every other posted call in this file is written the same
    /// way).
    ///
    /// The accepted requirements live here too, and for the same reason: the read is started from whatever thread the
    /// command resumed on, while the answer is parsed on the application thread, so the options may only travel
    /// through this shared state (a plain member would make the two threads race on it).
    struct PromptState {
        String          current;
        FileReadOptions file_options;
        DirReadOptions  dir_options;
    };

    explicit Impl(VisualUserIO* owner) : owner(owner) {}

    void setConsolePanel(ConsolePanel* console);
    void setCommandManager(vn::appfw::CommandManager* manager);
    void refreshCompletion();
    void putString(const String& str);
    void clear();
    void cancelPendingInput();

    /// Claims the single interaction slot and shows the prompt; false when another
    /// read is already waiting.
    bool beginRead(PendingRead kind, const String& prompt);
    /// Releases the interaction slot.
    void endRead() noexcept;
    /// Waits for the user to answer the prompt; true when a value arrived, false
    /// when the interaction was cancelled.
    vn::async::Task<bool> waitForInput(PendingRead kind, const ReadUi& ui);

    void onLineEntered(const String& text);
    void onEscape();
    void parseAndComplete(const String& text);
    void repromptError(const String& message);
    /// Re-shows the current prompt with the text the user typed still in the input line, so a refusal does not make them
    /// type the whole thing again.
    void repromptPathError(const String& message, const String& text);

    /**
     * @brief Appends a command outcome to the console on the application thread.
     *
     * The command completion callback runs on the thread that finished the
     * command, which is not necessarily the application thread; the console panel
     * is a QWidget and may only be touched there.
     *
     * @param message Message to show; empty messages are ignored by the panel.
     * @param type    Console message type describing how loud the outcome is.
     */
    void appendOnApplicationThread(const String& message, ConsoleMessageType type);

    void completeString(const String& value);
    void completeInt(int value);
    void completeDouble(double value);
    void completePoint(const math::Point3d& value);
    void completePath(std::filesystem::path value);
    void completePaths(PathList value);
    void cancelInteraction();

    /// The I/O this state belongs to: the command manager and the number parser are its API.
    VisualUserIO* owner{ nullptr };

    vn::async::AsyncEvent done;
    std::atomic<bool>    cancelled{ false };

    /// The interaction slot: only one read may wait for the user at a time, because
    /// the console shows a single prompt and two waiting reads would share done
    /// and each other's result fields. Claimed and released with compare/exchange,
    /// so a second read can never slip in.
    std::atomic<PendingRead> pending{ PendingRead::None };

    String        string_result;
    int           int_result{ 0 };
    double        double_result{ 0.0 };
    math::Point3d point_result;
    std::filesystem::path path_result;
    PathList              paths_result;

    ConsolePanel* console{ nullptr };
    /// Handlers registered on the bound console, so a rebind can drop them again.
    vn::Connection line_handler{};
    vn::Connection escape_handler{};
    /// Connection on the command manager's commandsChanged().
    vn::Connection commands_handler{};

    /// Prompt bookkeeping of the current read; see PromptState.
    std::shared_ptr<PromptState> prompt_state{ std::make_shared<PromptState>() };
};

VisualUserIO::VisualUserIO() : d(std::make_unique<Impl>(this)) {}

VisualUserIO::~VisualUserIO() = default;

void VisualUserIO::setConsolePanel(ConsolePanel* console)
{
    d->setConsolePanel(console);
}

void VisualUserIO::Impl::setConsolePanel(ConsolePanel* console)
{
    if (this->console != nullptr) {
        // Drop the handlers of the panel being left behind: binding a panel twice
        // would otherwise run every entered line through onLineEntered() twice, and
        // an idle line would start its command twice.
        line_handler.disconnect();
        escape_handler.disconnect();
    }

    this->console = console;
    if (this->console == nullptr)
    {
        return;
    }
    line_handler   = this->console->lineEntered.connect([this](const String& text) { onLineEntered(text); });
    escape_handler = this->console->escapePressed.connect([this] { onEscape(); });
    refreshCompletion();
}

void VisualUserIO::setCommandManager(vn::appfw::CommandManager* manager)
{
    d->setCommandManager(manager);
}

void VisualUserIO::Impl::setCommandManager(vn::appfw::CommandManager* manager)
{
    if (auto* previous = owner->commandManager(); previous != nullptr && previous != manager) {
        commands_handler.disconnect();
    }

    owner->UserIO::setCommandManager(manager);

    if (manager != nullptr && !commands_handler.isActive()) {
        // The completion list is a snapshot: follow the command set so commands of
        // plugins that register after the console was bound still show up.
        commands_handler = manager->commandsChanged.connect(
            [this](vn::appfw::CommandManager&, vn::EventArgs&) { refreshCompletion(); });
    }
    refreshCompletion();
}

void VisualUserIO::Impl::refreshCompletion()
{
    if (!console || !owner->commandManager())
    {
        return;
    }

    auto* pm = Application::current() ? Application::current()->pluginManager() : nullptr;

    std::vector<ConsoleCommandEntry> entries;
    for (const auto& info : owner->commandManager()->commandInfos())
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

    auto* panel = console;
    onConsolePanel(panel, [entries = std::move(entries)](ConsolePanel* target) { target->setCommandEntries(entries); });
}

void VisualUserIO::putString(const String& str)
{
    d->putString(str);
}

void VisualUserIO::Impl::putString(const String& str)
{
    if (console == nullptr)
    {
        return;
    }

    // Commands print from whatever thread they resumed on, so the panel write is
    // marshalled: the panel is a QWidget and only the application thread may touch
    // it. Calling this from any thread is therefore safe.
    auto* panel = console;
    onConsolePanel(panel, [str](ConsolePanel* target) { target->append(ConsoleMessageType::Normal, str); });
}

void VisualUserIO::clear()
{
    d->clear();
}

void VisualUserIO::Impl::clear()
{
    if (console == nullptr)
    {
        return;
    }

    auto* panel = console;
    onConsolePanel(panel, [](ConsolePanel* target) { target->clear(); });
}

void VisualUserIO::cancelPendingInput()
{
    d->cancelPendingInput();
}

void VisualUserIO::Impl::cancelPendingInput()
{
    // Same path as Escape: the awaiting read resumes with std::nullopt, so a command
    // parked on user input unwinds instead of holding the shutdown drain for its
    // whole bound. Application::shutdown() calls this while the event loop is already
    // stopped, and it may be called from another thread, so nothing here may end up
    // touching the panel.
    if (pending.load() != PendingRead::None)
    {
        cancelInteraction();
    }
}

bool VisualUserIO::Impl::beginRead(PendingRead kind, const String& prompt)
{
    PendingRead expected = PendingRead::None;
    if (!pending.compare_exchange_strong(expected, kind))
    {
        VN_LOGW("A user-input read is already waiting; refusing the new one");
        return false;
    }

    // The prompt itself is recorded where it is shown (waitForInput's marshalled UI
    // call), so this method never touches application-thread state: it may run on
    // the thread the reading command resumed on.
    cancelled.store(false);
    done.reset();
    return true;
}

void VisualUserIO::Impl::endRead() noexcept
{
    pending.store(PendingRead::None);

    // The picker belongs to the read that asked for it: dropping it here keeps the button from outliving its prompt (and
    // from being clicked for a read that is over). Reads end on the thread that completed them - for a console read that
    // is the application thread, where this call runs inline - so this cannot overtake the next read's own setup.
    if (console != nullptr)
    {
        auto* panel = console;
        onConsolePanel(panel, [](ConsolePanel* target) { target->setInputAction(ConsoleInputAction{}); });
    }
}

vn::async::Task<bool> VisualUserIO::Impl::waitForInput(PendingRead kind, const ReadUi& ui)
{
    if (!beginRead(kind, ui.prompt))
    {
        co_return false;
    }
    const ReadScope scope{ this }; // frees the interaction slot however this ends

    if (console != nullptr)
    {
        auto* panel = console;
        // The prompt is recorded here, inside the marshalled call, so the member it
        // lives in is only ever touched on the application thread (repromptError()
        // reads it there when the user enters something invalid).
        auto state  = prompt_state;
        onConsolePanel(panel, [state, ui](ConsolePanel* target) {
            state->current      = ui.prompt;
            state->file_options = ui.file_options;
            state->dir_options  = ui.dir_options;
            target->setInputAction(ui.action);
            target->beginInput(ui.prompt, ui.initial_text);
        });
    }

    co_await done;
    co_return !cancelled.load();
}

vn::async::Task<std::optional<String>> VisualUserIO::getStringAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    if (!co_await d->waitForInput(Impl::PendingRead::String, ui))
    {
        co_return std::nullopt;
    }
    co_return d->string_result;
}

vn::async::Task<std::optional<int>> VisualUserIO::getIntAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    if (!co_await d->waitForInput(Impl::PendingRead::Int, ui))
    {
        co_return std::nullopt;
    }
    co_return d->int_result;
}

vn::async::Task<std::optional<double>> VisualUserIO::getDoubleAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    if (!co_await d->waitForInput(Impl::PendingRead::Double, ui))
    {
        co_return std::nullopt;
    }
    co_return d->double_result;
}

vn::async::Task<std::optional<math::Point3d>> VisualUserIO::getPoint3dAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    if (!co_await d->waitForInput(Impl::PendingRead::Point, ui))
    {
        co_return std::nullopt;
    }
    co_return d->point_result;
}

namespace
{

/// The Qt name filter for a file read: the accepted patterns under a fixed caption, plus the always-available
/// "every file" group. The panel hands this to the dialog as it stands.
///
/// Takes the pieces instead of the options: the pattern normalization lives in UserIO, which a free function may not
/// reach into, so the caller (a member of this class) passes the result in.
String pickerFilter(const std::vector<String>& patterns)
{
    if (patterns.empty())
    {
        return {};
    }

    String joined;
    for (const String& pattern : patterns)
    {
        if (!joined.empty())
        {
            joined += u8" ";
        }
        joined += pattern;
    }

    // 标题由框架定：读请求说的是"要什么格式"（extensions），给它起名字是选择器的事（与提示文案同源）。
    return String(u8"支持的格式 (") + joined + u8");;所有文件 (*)";
}

} // namespace

vn::async::Task<std::optional<std::filesystem::path>> VisualUserIO::getFileAsync(const String&           prompt,
                                                                                const FileReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::File;
    ui.action.filter  = pickerFilter(filterPatterns(options));
    ui.action.tooltip = String(u8"选择文件…") + requirementSummary(options.extensions, /*multiple=*/false);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary(options.extensions, /*multiple=*/false));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.file_options   = options;

    if (!co_await d->waitForInput(Impl::PendingRead::File, ui))
    {
        co_return std::nullopt;
    }
    co_return d->path_result;
}

vn::async::Task<std::optional<UserIO::PathList>> VisualUserIO::getFilesAsync(const String& prompt, const FileReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Files;
    ui.action.filter  = pickerFilter(filterPatterns(options));
    ui.action.tooltip = String(u8"选择文件…") + requirementSummary(options.extensions, /*multiple=*/true);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary(options.extensions, /*multiple=*/true));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.file_options   = options;

    if (!co_await d->waitForInput(Impl::PendingRead::Files, ui))
    {
        co_return std::nullopt;
    }
    co_return d->paths_result;
}

vn::async::Task<std::optional<std::filesystem::path>> VisualUserIO::getDirAsync(const String& prompt, const DirReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Directory;
    ui.action.tooltip = String(u8"选择目录…") + requirementSummary({}, /*multiple=*/false);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary({}, /*multiple=*/false));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.dir_options    = options;

    if (!co_await d->waitForInput(Impl::PendingRead::Dir, ui))
    {
        co_return std::nullopt;
    }
    co_return d->path_result;
}

vn::async::Task<std::optional<UserIO::PathList>> VisualUserIO::getDirsAsync(const String& prompt, const DirReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Directories;
    ui.action.tooltip = String(u8"选择目录…") + requirementSummary({}, /*multiple=*/true);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary({}, /*multiple=*/true));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.dir_options    = options;

    if (!co_await d->waitForInput(Impl::PendingRead::Dirs, ui))
    {
        co_return std::nullopt;
    }
    co_return d->paths_result;
}

void VisualUserIO::Impl::onLineEntered(const String& text)
{
    if (pending != PendingRead::None)
    {
        parseAndComplete(text);
        return;
    }

    if (owner->commandManager() && owner->commandManager()->runningCount() > 0)
    {
        if (console)
        {
            console->append(ConsoleMessageType::Warning, String(u8"命令正在执行，请稍候"));
        }
        return;
    }

    if (console)
    {
        console->append(ConsoleMessageType::Command, text);
    }

    if (owner->commandManager())
    {
        // 异步启动命令；失败信息在命令完成后回写。
        // 命令可能在任意线程上结束（await 了定时器/异步读取），因此回写必须
        // 编组到应用线程：控制台面板是 QWidget，只有应用线程可以碰。
        [](Impl* self, vn::async::Task<CommandResult> task) -> vn::async::DetachedTask {
            const auto result = co_await std::move(task);
            if (result.succeeded())
            {
                co_return;
            }

            // "Does not apply here" is a hint, not a failure: the command explained what
            // the current context was missing, so show that as a warning - and stay quiet
            // when it did not explain, because "命令执行失败" would be a fault report for
            // something that never went wrong.
            if (result.status() == CommandStatus::NotApplicable)
            {
                const auto& message = result.message();
                if (!message.empty())
                {
                    self->appendOnApplicationThread(message, ConsoleMessageType::Warning);
                }
                co_return;
            }

            const auto& message = result.message();
            self->appendOnApplicationThread(message.empty() ? String(u8"命令执行失败") : message,
                                            ConsoleMessageType::Error);
        }(this, owner->commandManager()->executeCommandAsync(text, resolveCommandTarget(MainWindow::current())));
    }
}

void VisualUserIO::Impl::appendOnApplicationThread(const String& message, ConsoleMessageType type)
{
    if (!console)
    {
        return;
    }

    auto* panel = console;
    onConsolePanel(panel, [message, type](ConsolePanel* target) { target->append(type, message); });
}

void VisualUserIO::Impl::onEscape()
{
    if (pending != PendingRead::None)
    {
        cancelInteraction();
    }
    else if (owner->commandManager() && owner->commandManager()->runningCount() > 0)
    {
        // Cancel the foreground command chain (the one the console just showed as
        // running). Chains pushed out of the foreground are only reachable through
        // cancelAll().
        owner->commandManager()->cancelCurrent();
    }
    else if (console)
    {
        console->clearInput();
    }
}

void VisualUserIO::Impl::parseAndComplete(const String& text)
{
    switch (pending)
    {
    case PendingRead::String:
        completeString(text);
        break;

    case PendingRead::Int:
    {
        int value = 0;
        if (VisualUserIO::parseInt(text, value))
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

    case PendingRead::File:
    {
        const PathCheck check = checkPath(text, prompt_state->file_options);
        if (check.ok)
        {
            completePath(check.path);
        }
        else
        {
            repromptPathError(check.message, text);
        }
        break;
    }

    case PendingRead::Files:
    {
        const PathLineCheck check = checkPathLine(text, prompt_state->file_options);
        if (check.ok)
        {
            completePaths(check.paths);
        }
        else
        {
            repromptPathError(check.message, text);
        }
        break;
    }

    case PendingRead::Dir:
    {
        const PathCheck check = checkPath(text, prompt_state->dir_options);
        if (check.ok)
        {
            completePath(check.path);
        }
        else
        {
            repromptPathError(check.message, text);
        }
        break;
    }

    case PendingRead::Dirs:
    {
        const PathLineCheck check = checkPathLine(text, prompt_state->dir_options);
        if (check.ok)
        {
            completePaths(check.paths);
        }
        else
        {
            repromptPathError(check.message, text);
        }
        break;
    }

    case PendingRead::None:
        break;
    }
}

void VisualUserIO::Impl::repromptPathError(const String& message, const String& text)
{
    if (!console)
    {
        return;
    }

    auto*        panel  = console;
    const String prompt = prompt_state->current;
    onConsolePanel(panel, [message, prompt, text](ConsolePanel* target) {
        target->append(ConsoleMessageType::Error, message);
        // The rejected text goes back into the input line: with several paths in one line, clearing it would make the
        // user retype the ones that were fine.
        target->beginInput(prompt, text);
    });
}

void VisualUserIO::Impl::repromptError(const String& message)
{
    if (!console)
    {
        return;
    }

    auto*        panel  = console;
    const String prompt = prompt_state->current;
    onConsolePanel(panel, [message, prompt](ConsolePanel* target) {
        target->append(ConsoleMessageType::Error, message);
        target->beginInput(prompt);
    });
}

void VisualUserIO::Impl::completeString(const String& value)
{
    string_result = value;
    done.set();
}

void VisualUserIO::Impl::completeInt(int value)
{
    int_result = value;
    done.set();
}

void VisualUserIO::Impl::completeDouble(double value)
{
    double_result = value;
    done.set();
}

void VisualUserIO::Impl::completePoint(const math::Point3d& value)
{
    point_result = value;
    done.set();
}

void VisualUserIO::Impl::completePath(std::filesystem::path value)
{
    path_result = std::move(value);
    done.set();
}

void VisualUserIO::Impl::completePaths(PathList value)
{
    paths_result = std::move(value);
    done.set();
}

void VisualUserIO::Impl::cancelInteraction()
{
    cancelled = true;
    done.set();
}

VN_APPFWGUI_NS_END
