#include <vine/appfw/gui/VisualUserIO.hpp>

#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <QPointer>
#include <QWidget>

#include <vine/Signal.hpp>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/Translation.hpp>
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
struct VisualUserIO::Impl {
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
        ConsoleInputAction action;       // Picker the input line offers; kind == None for a plain read.
        String             prompt;       // Prompt text to show.
        String             initial_text; // Text to pre-fill the input line with (the caller's suggestion).
        FileReadOptions    file_options; // What the answer has to look like; only the file kinds use it.
        DirReadOptions     dir_options;  // What the answer has to look like; only the directory kinds use it.
    };

    /// Releases the interaction slot when the read ends - however it ends,
    /// including a coroutine frame that is destroyed before it ever resumed.
    struct ReadScope {
        Impl* self;

        ~ReadScope()
        {
            self->endRead();
        }
    };

    /// Prompt bookkeeping of the read that is waiting for input.
    ///
    /// Read and written on the application thread only: the write happens inside
    /// the marshalled UI call that shows the prompt, and the read inside
    /// reprompt(). It is shared rather than a plain member so a posted UI call
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

    explicit Impl(VisualUserIO* owner)
      : owner(owner)
    {}

    void setConsolePanel(ConsolePanel* console);
    void setCommandManager(vn::appfw::CommandManager* manager);
    void refreshCompletion();
    void putString(const String& str);
    void clear();
    void cancelPendingInput();

    /// Claims the single interaction slot and empties it; false when another read is already waiting.
    ///
    /// The prompt itself is shown by showPrompt(), so this never touches application-thread state: it may
    /// run on the thread the reading command resumed on.
    ///
    /// @param kind Read kind that will be parsed from the answer.
    /// @param slot Answer slot of that kind; emptied here, so an answer a previous read left behind can
    ///             never be mistaken for this one's.
    /// @return true when the slot now belongs to this read.
    template <typename TValue>
    bool beginRead(PendingRead kind, std::optional<TValue> Impl::* slot);
    /// Releases the interaction slot.
    void endRead() noexcept;

    /// Shows the prompt and the picker of one read on the application thread.
    ///
    /// @param ui What the read shows; copied into the posted call, so it may be a temporary.
    void showPrompt(const ReadUi& ui);

    /// Waits for the answer of one read; std::nullopt when the read was cancelled.
    ///
    /// @details
    /// - Claiming the slot, showing the prompt, waiting, taking the answer and releasing the slot happen
    ///   here, in that order, and the answer is taken while this read still owns the slot - so the read
    ///   that starts next cannot overwrite it in between.
    /// - The answer is the slot itself: an empty slot at wake-up is the cancellation, which is why no
    ///   separate "was it answered?" flag exists.
    ///
    /// @param kind Read kind, used by the console side to parse the line it hands back.
    /// @param ui   What the read shows; owned by the returned task, so it outlives every suspension.
    /// @param slot Answer slot of that kind, emptied when the answer is taken.
    /// @return The answer, or std::nullopt when the read was cancelled.
    template <typename TValue>
    vn::async::Task<std::optional<TValue>> read(PendingRead kind, ReadUi ui, std::optional<TValue> Impl::* slot);

    /// Returns the kind of read that is waiting, or None when the line is a command line.
    PendingRead pendingRead() const;

    /// Releases the read that is waiting, if there is one.
    ///
    /// @return true when a read was waiting and has been released.
    bool cancelInteraction();

    /// Refuses every later read and releases the one that is waiting; called by the destructor.
    void close();

    void onLineEntered(const String& text);
    void onEscape();
    void parseAndComplete(PendingRead kind, const String& text);

    /// Shows why an answer was refused and asks again.
    ///
    /// @param message Refusal text.
    /// @param text    What the user typed, put back into the input line; empty clears it - a single value
    ///                starts over, while a read of several paths keeps the entries that were fine.
    void reprompt(const String& message, const String& text);

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

    /// Records one answer and releases the read that asked for it.
    ///
    /// @details
    /// - The store happens under read_mutex, so a cancellation that races with the answer either lets
    ///   the store land (the read then returns it) or has already ended the read - never both.
    /// - An answer whose read is gone, or whose slot a different read already owns, is dropped: it
    ///   belongs to nobody, and handing it on would give another read a value it never asked for.
    ///
    /// @param kind  Read kind the answer was parsed for.
    /// @param slot  Answer slot of that kind.
    /// @param value Answer to store.
    template <typename TValue>
    void complete(PendingRead kind, std::optional<TValue> Impl::* slot, TValue value);

    /// The I/O this state belongs to: the command manager and the number parser are its API.
    VisualUserIO* owner{ nullptr };

    /// Releases the read that is waiting: set by an answer or by a cancellation.
    vn::async::AsyncEvent done;

    /// Guards the transition into and out of one read: claiming `pending`, arming `done`, storing and
    /// taking the answer. Readers on the console's own thread ask pendingRead(); only the transitions take
    /// the lock, and none of them calls out while holding it.
    mutable std::mutex read_mutex;

    /// The read that is waiting, or None; guarded by read_mutex.
    ///
    /// Only one may wait for the user at a time, because the console shows a single prompt and two
    /// waiting reads would share done and the answer slots.
    PendingRead pending{ PendingRead::None };

    /// True once the owner is being destroyed; guarded by read_mutex.
    ///
    /// A read started by a command that the destructor's cancellation just resumed is refused instead of
    /// parking on an event that is about to die.
    bool closed{ false };

    /// Answer of the read that is waiting, one slot per kind, empty until the console side stores it;
    /// guarded by read_mutex. The slot IS the answer: an empty one at wake-up means the read was
    /// cancelled, so nothing else tracks that.
    std::optional<String>                string_result;
    std::optional<int>                   int_result;
    std::optional<double>                double_result;
    std::optional<math::Point3d>         point_result;
    std::optional<std::filesystem::path> path_result;
    std::optional<PathList>              paths_result;

    ConsolePanel* console{ nullptr };
    /// Handlers registered on the bound console, so a rebind can drop them again.
    vn::Connection line_handler{};
    vn::Connection escape_handler{};
    /// Connection on the command manager's commandsChanged().
    vn::Connection commands_handler{};

    /// Prompt bookkeeping of the current read; see PromptState.
    std::shared_ptr<PromptState> prompt_state{ std::make_shared<PromptState>() };
};

VisualUserIO::VisualUserIO()
  : d(std::make_unique<Impl>(this))
{}

VisualUserIO::~VisualUserIO()
{
    // A read parked on done keeps its awaiter in a coroutine frame, so destroying the event under it is
    // undefined (AsyncEvent asserts). Closing refuses reads that the cancellation below resumes, and the
    // cancellation releases the one that is waiting, so the event is destroyed with nobody parked on it.
    // Draining the command that was waiting stays the host's job (Application::shutdown() does it before
    // it destroys the IO); this only makes the destruction itself well-defined.
    d->close();
}

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
    if (this->console == nullptr) {
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
        commands_handler = manager->commandsChanged.connect([this](vn::appfw::CommandManager&, vn::EventArgs&) { refreshCompletion(); });
    }
    refreshCompletion();
}

void VisualUserIO::Impl::refreshCompletion()
{
    if (!console || !owner->commandManager()) {
        return;
    }

    auto* pm = Application::current() ? Application::current()->pluginManager() : nullptr;

    std::vector<ConsoleCommandEntry> entries;
    for (const auto& info : owner->commandManager()->commandInfos()) {
        // A disabled command cannot run, so offering it for completion would only
        // lead to a failed execution.
        if (!info.enabled) {
            continue;
        }

        // The popup prefix is the plugin's human-friendly display name.
        String source = info.owner;
        if (pm != nullptr && !info.owner.empty()) {
            if (auto* plugin = pm->plugin(info.owner)) {
                const auto pinfo = plugin->info();
                source           = pinfo.display_name.empty() ? pinfo.name : pinfo.display_name;
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
    if (console == nullptr) {
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
    if (console == nullptr) {
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
    // Same path as Escape: the awaiting read is released, so a command parked on user input unwinds
    // instead of holding the shutdown drain for its whole bound. Application::shutdown() calls this
    // while the event loop is already stopped, and it may be called from another thread, so nothing
    // here may end up touching the panel.
    static_cast<void>(cancelInteraction());
}

VisualUserIO::Impl::PendingRead VisualUserIO::Impl::pendingRead() const
{
    // Taken under the lock so a reader cannot observe a slot that is being claimed or released.
    const std::lock_guard lock(read_mutex);
    return pending;
}

template <typename TValue>
bool VisualUserIO::Impl::beginRead(PendingRead kind, std::optional<TValue> VisualUserIO::Impl::* slot)
{
    const std::lock_guard lock(read_mutex);
    if (closed) {
        VN_LOGW("A user-input read was started while the visual I/O is shutting down; refusing it");
        return false;
    }
    if (pending != PendingRead::None) {
        VN_LOGW("A user-input read is already waiting; refusing the new one");
        return false;
    }

    // Claiming, emptying and arming are one step: a cancel either arrives before this read exists (and
    // is meant for the previous one) or after it is claimable, and an answer that a previous read left
    // behind (a frame destroyed before it was resumed) cannot be mistaken for this one's.
    pending     = kind;
    this->*slot = std::nullopt;
    done.reset();
    return true;
}

void VisualUserIO::Impl::endRead() noexcept
{
    {
        const std::lock_guard lock(read_mutex);
        pending = PendingRead::None;
    }

    // The picker belongs to the read that asked for it: dropping it here keeps the button from outliving its prompt (and
    // from being clicked for a read that is over). Reads end on the thread that completed them - for a console read that
    // is the application thread, where this call runs inline - so this cannot overtake the next read's own setup.
    if (console != nullptr) {
        auto* panel = console;
        onConsolePanel(panel, [](ConsolePanel* target) { target->setInputAction(ConsoleInputAction{}); });
    }
}

void VisualUserIO::Impl::showPrompt(const ReadUi& ui)
{
    if (console == nullptr) {
        return;
    }

    // The prompt is recorded inside the marshalled call, so the member it lives in is only ever touched on
    // the application thread (reprompt() reads it there when the user enters something invalid).
    auto* panel = console;
    auto  state = prompt_state;
    onConsolePanel(panel, [state, ui](ConsolePanel* target) {
        state->current      = ui.prompt;
        state->file_options = ui.file_options;
        state->dir_options  = ui.dir_options;
        target->setInputAction(ui.action);
        target->beginInput(ui.prompt, ui.initial_text);
    });
}

template <typename TValue>
vn::async::Task<std::optional<TValue>>
VisualUserIO::Impl::read(PendingRead kind, ReadUi ui, std::optional<TValue> VisualUserIO::Impl::* slot)
{
    if (!beginRead(kind, slot)) {
        co_return std::nullopt;
    }
    const ReadScope scope{ this }; // frees the interaction slot however this ends

    showPrompt(ui);

    co_await done;

    // Taken while this read still owns the slot (endRead() runs when this returns) and emptied on the way
    // out, so the value cannot be read twice, and an empty slot is exactly "the read was cancelled".
    std::optional<TValue> answer;
    {
        const std::lock_guard lock(read_mutex);
        answer = std::exchange(this->*slot, std::nullopt);
    }
    co_return answer;
}

void VisualUserIO::Impl::close()
{
    {
        const std::lock_guard lock(read_mutex);
        closed = true;
    }

    cancelInteraction();

    if (pendingRead() != PendingRead::None) {
        // The resumed command started another read; close() already refused it, so reaching this point
        // means a read was still waiting while the owner went away.
        VN_LOGW("VisualUserIO destroyed while a user-input read was still pending: drain the command first");
    }
}

vn::async::Task<std::optional<String>> VisualUserIO::getStringAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    return d->read(Impl::PendingRead::String, std::move(ui), &Impl::string_result);
}

vn::async::Task<std::optional<int>> VisualUserIO::getIntAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    return d->read(Impl::PendingRead::Int, std::move(ui), &Impl::int_result);
}

vn::async::Task<std::optional<double>> VisualUserIO::getDoubleAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    return d->read(Impl::PendingRead::Double, std::move(ui), &Impl::double_result);
}

vn::async::Task<std::optional<math::Point3d>> VisualUserIO::getPoint3dAsync(const String& prompt)
{
    Impl::ReadUi ui;
    ui.prompt = prompt;
    return d->read(Impl::PendingRead::Point, std::move(ui), &Impl::point_result);
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
    if (patterns.empty()) {
        return {};
    }

    String joined;
    for (const String& pattern : patterns) {
        if (!joined.empty()) {
            joined += u8" ";
        }
        joined += pattern;
    }

    // 标题由框架定：读请求说的是"要什么格式"（extensions），给它起名字是选择器的事（与提示文案同源）。
    return vn::appfw::translate("appfw", "Supported formats (") + joined + vn::appfw::translate("appfw", ");;All files (*)");
}

} // namespace

vn::async::Task<std::optional<std::filesystem::path>> VisualUserIO::getFileAsync(const String& prompt, const FileReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::File;
    ui.action.filter  = pickerFilter(filterPatterns(options));
    ui.action.tooltip = vn::appfw::translate("appfw", "Choose a file…") + requirementSummary(options.extensions, /*multiple=*/false);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary(options.extensions, /*multiple=*/false));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.file_options   = options;

    return d->read(Impl::PendingRead::File, std::move(ui), &Impl::path_result);
}

vn::async::Task<std::optional<UserIO::PathList>> VisualUserIO::getFilesAsync(const String& prompt, const FileReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Files;
    ui.action.filter  = pickerFilter(filterPatterns(options));
    ui.action.tooltip = vn::appfw::translate("appfw", "Choose a file…") + requirementSummary(options.extensions, /*multiple=*/true);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary(options.extensions, /*multiple=*/true));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.file_options   = options;

    return d->read(Impl::PendingRead::Files, std::move(ui), &Impl::paths_result);
}

vn::async::Task<std::optional<std::filesystem::path>> VisualUserIO::getDirAsync(const String& prompt, const DirReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Directory;
    ui.action.tooltip = vn::appfw::translate("appfw", "Choose a directory…") + requirementSummary({}, /*multiple=*/false);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary({}, /*multiple=*/false));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.dir_options    = options;

    return d->read(Impl::PendingRead::Dir, std::move(ui), &Impl::path_result);
}

vn::async::Task<std::optional<UserIO::PathList>> VisualUserIO::getDirsAsync(const String& prompt, const DirReadOptions& options)
{
    Impl::ReadUi ui;
    ui.action.kind    = ConsoleInputKind::Directories;
    ui.action.tooltip = vn::appfw::translate("appfw", "Choose a directory…") + requirementSummary({}, /*multiple=*/true);
    ui.prompt         = promptWithRequirements(prompt, requirementSummary({}, /*multiple=*/true));
    ui.initial_text   = String(options.suggested_path.u8string());
    ui.dir_options    = options;

    return d->read(Impl::PendingRead::Dirs, std::move(ui), &Impl::paths_result);
}

void VisualUserIO::Impl::onLineEntered(const String& text)
{
    // One snapshot of the slot decides what this line is: an answer to the pending read, or a command.
    const PendingRead kind = pendingRead();
    if (kind != PendingRead::None) {
        parseAndComplete(kind, text);
        return;
    }

    // Enter on an empty prompt is not a command: the panel has already cleared the line, and
    // dispatching a blank line would report "the command \"\" is not registered".
    if (text.trimmed().empty()) {
        return;
    }

    if (owner->commandManager() && owner->commandManager()->runningCount() > 0) {
        if (console) {
            console->append(ConsoleMessageType::Warning, vn::appfw::translate("appfw", "A command is running, please wait"));
        }
        return;
    }

    if (console) {
        console->append(ConsoleMessageType::Command, text);
    }

    if (owner->commandManager()) {
        // 异步启动命令；失败信息在命令完成后回写。
        // 命令可能在任意线程上结束（await 了定时器/异步读取），因此回写必须
        // 编组到应用线程：控制台面板是 QWidget，只有应用线程可以碰。
        [](Impl* self, vn::async::Task<CommandResult> task) -> vn::async::DetachedTask {
            const auto result = co_await std::move(task);
            if (result.succeeded()) {
                co_return;
            }

            // "Does not apply here" is a hint, not a failure: the command explained what
            // the current context was missing, so show that as a warning - and stay quiet
            // when it did not explain, because "命令执行失败" would be a fault report for
            // something that never went wrong.
            if (result.status() == CommandStatus::NotApplicable) {
                const auto& message = result.message();
                if (!message.empty()) {
                    self->appendOnApplicationThread(message, ConsoleMessageType::Warning);
                }
                co_return;
            }

            const auto& message = result.message();
            self->appendOnApplicationThread(message.empty() ? vn::appfw::translate("appfw", "The command failed") : message, ConsoleMessageType::Error);
        }(this, owner->commandManager()->executeCommandLineAsync(text, resolveCommandTarget(MainWindow::current())));
    }
}

void VisualUserIO::Impl::appendOnApplicationThread(const String& message, ConsoleMessageType type)
{
    if (!console) {
        return;
    }

    auto* panel = console;
    onConsolePanel(panel, [message, type](ConsolePanel* target) { target->append(type, message); });
}

void VisualUserIO::Impl::onEscape()
{
    if (cancelInteraction()) {
        return;
    }

    if (owner->commandManager() && owner->commandManager()->runningCount() > 0) {
        // Cancel the foreground command chain (the one the console just showed as
        // running). Chains pushed out of the foreground are only reachable through
        // cancelAll().
        owner->commandManager()->cancelCurrent();
    }
    else if (console) {
        console->clearInput();
    }
}

void VisualUserIO::Impl::parseAndComplete(PendingRead kind, const String& text)
{
    switch (kind) {
    case PendingRead::String: complete(kind, &Impl::string_result, text); break;

    case PendingRead::Int:
    {
        int value = 0;
        if (VisualUserIO::parseInt(text, value)) {
            complete(kind, &Impl::int_result, value);
        }
        else {
            reprompt(vn::appfw::translate("appfw", "Enter an integer"), {});
        }
        break;
    }

    case PendingRead::Double:
    {
        bool         ok    = false;
        const double value = text.trimmed().toDouble(&ok);
        if (ok && std::isfinite(value)) {
            complete(kind, &Impl::double_result, value);
        }
        else {
            reprompt(vn::appfw::translate("appfw", "Enter a number"), {});
        }
        break;
    }

    case PendingRead::Point:
    {
        const auto parts = text.split(u8',');
        if (parts.size() == 3) {
            bool   x_ok = false;
            bool   y_ok = false;
            bool   z_ok = false;
            double x    = parts[0].trimmed().toDouble(&x_ok);
            double y    = parts[1].trimmed().toDouble(&y_ok);
            double z    = parts[2].trimmed().toDouble(&z_ok);
            if (x_ok && y_ok && z_ok) {
                math::Point3d point;
                point.x = x;
                point.y = y;
                point.z = z;
                complete(kind, &Impl::point_result, point);
                break;
            }
        }
        reprompt(vn::appfw::translate("appfw", "Enter a point as x,y,z"), {});
        break;
    }

    case PendingRead::File:
    {
        const PathCheck check = checkPath(text, prompt_state->file_options);
        if (check.ok) {
            complete(kind, &Impl::path_result, check.path);
        }
        else {
            reprompt(check.message, text);
        }
        break;
    }

    case PendingRead::Files:
    {
        const PathLineCheck check = checkPathLine(text, prompt_state->file_options);
        if (check.ok) {
            complete(kind, &Impl::paths_result, check.paths);
        }
        else {
            reprompt(check.message, text);
        }
        break;
    }

    case PendingRead::Dir:
    {
        const PathCheck check = checkPath(text, prompt_state->dir_options);
        if (check.ok) {
            complete(kind, &Impl::path_result, check.path);
        }
        else {
            reprompt(check.message, text);
        }
        break;
    }

    case PendingRead::Dirs:
    {
        const PathLineCheck check = checkPathLine(text, prompt_state->dir_options);
        if (check.ok) {
            complete(kind, &Impl::paths_result, check.paths);
        }
        else {
            reprompt(check.message, text);
        }
        break;
    }

    case PendingRead::None: break;
    }
}

void VisualUserIO::Impl::reprompt(const String& message, const String& text)
{
    if (!console) {
        return;
    }

    auto*        panel  = console;
    const String prompt = prompt_state->current;
    onConsolePanel(panel, [message, prompt, text](ConsolePanel* target) {
        target->append(ConsoleMessageType::Error, message);
        target->beginInput(prompt, text);
    });
}

template <typename TValue>
void VisualUserIO::Impl::complete(PendingRead kind, std::optional<TValue> VisualUserIO::Impl::* slot, TValue value)
{
    {
        const std::lock_guard lock(read_mutex);
        if (pending != kind) {
            // The read that asked is gone (cancelled), or a different one already owns the slot: the
            // answer belongs to nobody.
            return;
        }
        this->*slot = std::move(value);
    }

    // Outside the lock: set() resumes the waiting coroutine on this thread, and that continuation takes
    // read_mutex again to take the answer.
    done.set();
}

bool VisualUserIO::Impl::cancelInteraction()
{
    {
        const std::lock_guard lock(read_mutex);
        if (pending == PendingRead::None) {
            return false;
        }
    }

    // Outside the lock, like complete(): the read resumes here and takes the (still empty) slot, which is
    // what turns a cancellation into std::nullopt.
    done.set();
    return true;
}

VN_APPFWGUI_NS_END
