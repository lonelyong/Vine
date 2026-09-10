#include <vine/appfw/CommandManager.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Command.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/UserIO.hpp>

#include <vine/async/Cancellation.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/Sleep.hpp>
#include <vine/async/SyncWait.hpp>
#include <vine/async/Task.hpp>

#include <vine/logging/Log.hpp>

#include <vine/progress/ProgressHost.hpp>

V_APPFW_NS_BEGIN

V_OBJECT_META_IMPL(CommandExecutingEventArgs, EventArgs)

CommandExecutingEventArgs::CommandExecutingEventArgs(Command* command)
  : command_(command)
{}

raw_ptr<Command> CommandExecutingEventArgs::command() const
{
    return command_;
}

V_OBJECT_META_IMPL(CommandExecutedEventArgs, EventArgs)

CommandExecutedEventArgs::CommandExecutedEventArgs(Command* command, const CommandResult& result)
  : command_(command)
  , result_(result)
{}

raw_ptr<Command> CommandExecutedEventArgs::command() const
{
    return command_;
}

const CommandResult& CommandExecutedEventArgs::result() const
{
    return result_;
}

namespace
{

/**
 * @brief A registered command: its meta class, a no-arg factory, the plugin
 * that registered it (empty for host-app commands) and the listing metadata
 * cached at registration time.
 */
struct RegisteredCommand {
    TypeId                    class_type;
    std::function<Command*()> factory;
    String                    owner;
    String                    group;
    String                    description;

    /// false while the user disabled the command: registered and listed, but not
    /// executable until it is enabled again.
    bool enabled{ true };
};

/// Returns whether a command flag set contains the given bit.
constexpr bool hasFlag(CommandFlags value, CommandFlags bit) noexcept
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(bit)) != 0;
}

/// Views a String's UTF-8 bytes without allocating.
///
/// Several log calls sit inside catch blocks and inside detached coroutines, where
/// an allocation or a logger failure would turn a handled error into a
/// std::terminate(). Logging guarantees the second half (see Logger: no level
/// function, log() or defaultLogger() call throws); passing a view instead of a
/// std::string keeps this side allocation-free as well.
std::string_view toUtf8View(const String& s) noexcept
{
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

/// Builds the failure message of a command that cannot be started by name.
///
/// The console prints this text verbatim, so it names the command and says what the
/// user can do about it instead of being a developer-facing code.
///
/// @param name     Command (or alias) name that was refused.
/// @param disabled true when the command is registered but disabled, false when the
///                 name is not registered at all.
/// @return The message to put in the Failed result.
String refusalMessage(const String& name, bool disabled)
{
    if (disabled) {
        return String(u8"命令“") + name + String(u8"”已被禁用；可在「命令管理器」中启用。");
    }
    return String(u8"命令“") + name + String(u8"”未注册。");
}

/// The Cancelled result reported for a cancellation.
CommandResult cancelledResult()
{
    return CommandResult(CommandStatus::Cancelled, String(u8"命令已取消"));
}

/// The Failed result carrying a message.
CommandResult failedResult(String message)
{
    return CommandResult(CommandStatus::Failed, std::move(message));
}

/// Builds the Failed result reported when a command throws.
///
/// The what() text is copied inside a try: building the result must not throw out of
/// a catch block, and an exception without a description still leaves a message the
/// UI can show.
CommandResult failedResultFromException(const std::exception& e)
{
    try {
        String message(reinterpret_cast<const char8_t*>(e.what()));
        if (!message.empty()) {
            return failedResult(std::move(message));
        }
    }
    catch (...) {
    }
    return failedResult(String(u8"command threw an exception"));
}

/// Runs a top-level execution path and turns everything it throws into a result.
///
/// Callers are UI event handlers and detached tasks, where an escaping exception
/// would end the process instead of reaching anybody who could handle it, so this is
/// the single place where that policy lives. The body is awaited here, which means the
/// closure is copied into this coroutine's frame (the safe form of a coroutine
/// lambda: see the caveat in async_global.hpp).
///
/// @param body Coroutine producing the outcome; called once, immediately.
/// @return The outcome, or the Failed/Cancelled result of whatever it threw.
template <typename Body>
vine::async::Task<CommandResult> runGuarded(Body body)
{
    try {
        co_return co_await body();
    }
    catch (const vine::async::TaskCancelledException&) {
        co_return cancelledResult();
    }
    catch (const std::exception& e) {
        V_LOGE("Command execution failed: {}", e.what());
        co_return failedResultFromException(e);
    }
    catch (...) {
        V_LOGE("Command execution failed with a non-standard exception");
        co_return failedResult(String(u8"command execution failed"));
    }
}

/// Interval between two liveness checks while waiting for chains to unwind.
///
/// The wait polls instead of parking on an event: a bounded wait that abandons a
/// queued waiter would destroy a coroutine frame while the completing thread may
/// be resuming it, which the async module's lifetime contract forbids (see
/// async_global.hpp). Sleep slices keep the wait cooperative and cheap - only an
/// Exclusive takeover and shutdown ever run it.
constexpr auto kDrainPollInterval = std::chrono::milliseconds{ 5 };

/// Fires one CommandManager event, logging whatever a handler throws.
///
/// Event handlers are user code, and nothing above a command (a UI handler, a
/// detached task) is required to catch: a throwing listener must not be able to
/// abort the command or tear down the process.
///
/// @param event Event to fire (executing or executed).
/// @param owner Manager firing it.
/// @param args  Notification arguments.
template <typename EventType, typename ArgsType>
void fireEvent(EventType& event, CommandManager& owner, ArgsType& args)
{
    try {
        event.trigger(owner, args);
    }
    catch (const std::exception& e) {
        V_LOGE("Command event handler threw: {}", e.what());
    }
    catch (...) {
        V_LOGE("Command event handler threw");
    }
}

/// Fires commandsChanged(), which every registry and alias mutation reports.
///
/// Call it with no lock held: a handler is user code and may call back into the
/// manager (that is exactly what a completion refresh does).
void fireCommandsChanged(CommandManager& manager)
{
    EventArgs args;
    fireEvent(manager.commandsChanged, manager, args);
}

/// Logs a finished execution at the level matching its outcome.
void logOutcome(const Command& command, const CommandResult& result)
{
    if (result.succeeded()) {
        V_LOGI("Command succeeded: {}", toUtf8View(command.name()));
    }
    else if (result.status() == CommandStatus::Cancelled) {
        V_LOGW("Command cancelled: {}", toUtf8View(command.name()));
    }
    else {
        V_LOGE("Command failed: {}: {}", toUtf8View(command.name()), toUtf8View(result.message()));
    }
}

/// Adds or removes a name from a disabled list.
///
/// @param list     Disabled names to update.
/// @param name     Name to add or remove.
/// @param disabled true to disable the name, false to enable it.
/// @return true when the list changed and therefore has to be persisted.
bool updateDisabledList(std::vector<String>& list, const String& name, bool disabled)
{
    const auto it = std::ranges::find(list, name);
    if (disabled == (it != list.end())) {
        return false;  // Already in the requested state.
    }

    if (disabled) {
        list.push_back(name);
    }
    else {
        list.erase(it);
    }
    return true;
}

/// Hands a user-visible message to the thread that owns the UI.
///
/// A command that awaited a timer or an asynchronous read finishes on the thread
/// that completed it, so a report must never reach userIO() directly: the GUI
/// UserIO appends to a QWidget, which only the application thread may touch. This
/// mirrors what ConsoleLogRouter does with log records. The message is delivered
/// inline when the caller already runs on the application thread, or when there is
/// no event loop to marshal onto, so console ordering is unchanged on the common
/// path.
///
/// @param app     Application owning the UserIO; must outlive the call.
/// @param message Message to show; an empty message is ignored.
void reportToUser(Application& app, const String& message)
{
    if (message.empty()) {
        return;
    }

    UserIO* io = app.userIO();
    if (io == nullptr) {
        return;
    }

    MainThreadDispatcher* dispatcher = app.mainThreadDispatcher();
    if (dispatcher == nullptr || dispatcher->isMainThread() || !dispatcher->hasEventLoop()) {
        io->putString(message);
        return;
    }

    // Dropped when the event loop stops before it gets to the task, which is the
    // right outcome during a shutdown: nobody is left to read the message.
    static_cast<void>(dispatcher->postToMain([io, message] { io->putString(message); }));
}

} // namespace

/**
 * @brief One command chain: what runs together and what cancels it.
 *
 * A chain is created for each top-level execution and shared with every nested
 * child, so it carries its own call stack and its own cancellation source.
 * Chains that run at the same time (several top-level entries, e.g. a detached
 * command next to a foreground one) therefore cannot pop each other's stack
 * entries nor cancel each other's tokens: the source is only ever stopped,
 * never replaced, so a token handed out earlier always stays valid.
 */
struct CommandManager::Chain {
    /// Registers a command that starts running on this chain.
    void enter(Command* command);

    /// Unregisters a command that finished from the chain's stack.
    void leaveStack(Command* command);

    /// Unregisters a finished run; the chain is thus done once none is left.
    void leaveChain();

    /// Returns the number of commands of this chain that have not finished yet.
    int liveRuns() const;

    /// Returns the number of commands currently on the chain's stack.
    int stackSize() const;

    /// Returns the innermost running command, or nullptr when idle.
    Command* innermost() const;

    /// Commands of this chain that are running, innermost last (guarded by mutex).
    std::vector<Command*> commands;

    /// Commands of this chain that have not finished yet (guarded by mutex).
    ///
    /// Never smaller than commands.size(): the stack entry is popped first and
    /// the run is accounted for only after the frame's progress host is gone, so
    /// zero here means the whole frame has been torn down and an Exclusive command
    /// waiting for the chain to unwind never observes a half-dead chain.
    int runs{ 0 };

    /// Cancellation source of the whole chain; stopped, never replaced.
    std::stop_source stop_source;

    /// Guards commands and runs.
    mutable std::mutex mutex;
};

void CommandManager::Chain::enter(Command* command)
{
    std::lock_guard<std::mutex> lock(mutex);
    commands.push_back(command);
    ++runs;
}

void CommandManager::Chain::leaveStack(Command* command)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (const auto it = std::ranges::find(commands, command); it != commands.end()) {
        commands.erase(it);
    }
}

void CommandManager::Chain::leaveChain()
{
    // No callback, no resume: whoever waits for this chain polls liveRuns(). The
    // caller guarantees this is the last step of the frame (stack entry and
    // progress host are already gone), so a reader that sees zero sees a settled
    // chain.
    std::lock_guard<std::mutex> lock(mutex);
    assert(runs > 0);
    --runs;
}

int CommandManager::Chain::liveRuns() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return runs;
}

int CommandManager::Chain::stackSize() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return static_cast<int>(commands.size());
}

Command* CommandManager::Chain::innermost() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return commands.empty() ? nullptr : commands.back();
}

/**
 * @brief Private data of CommandManager.
 */
struct CommandManager::Impl {
    /**
     * @brief Creates the private data of a manager.
     *
     * @param owner Application that owns the manager (non-owning).
     */
    explicit Impl(Application* owner)
      : app(owner)
    {}

    /**
     * @brief Resolves a command name through the alias map.
     *
     * Alias targets may themselves be aliases; the walk stops at the first
     * registered command or when it revisits a name, so a cycle resolves to
     * nothing instead of looping.
     *
     * The caller must hold registry_mutex.
     *
     * @param name Command or alias name.
     * @return The canonical command name, or the unresolved name.
     */
    String resolveName(const String& name) const;

    /**
     * @brief Returns the host config manager, or nullptr when there is none.
     *
     * @return The config manager of the owning application, or nullptr.
     */
    ConfigManager* configManager() const;

    /**
     * @brief Returns the effective disabled-command preference.
     *
     * Prefers the host configuration and falls back to the process-local list when
     * the application has no config manager.
     *
     * @return Disabled command names.
     */
    std::vector<String> disabledList() const;

    /**
     * @brief Returns whether the preference marks a command name as disabled.
     *
     * @param name Command name.
     * @return true when the name is in the disabled list.
     */
    bool isDisabled(const String& name) const;

    /**
     * @brief Returns whether a name is registered and currently disabled.
     *
     * Unlike isCommandEnabled(), which also reports 'not registered', this one tells
     * the two apart, which is what the caller-supplied-instance entry point needs: an
     * unregistered name must keep running.
     *
     * @param name Command or alias name.
     * @return true only for a registered command that is disabled.
     */
    bool isDisabledRegistration(const String& name) const;

    /**
     * @brief Records the disabled-command preference.
     *
     * The caller must not hold registry_mutex: the process-local fallback takes it.
     *
     * @param name     Command name.
     * @param disabled true to add the name to the disabled list, false to remove it.
     */
    void setDisabledPreference(const String& name, bool disabled);

    /**
     * @brief Adds a chain to the live registry that cancelAll() walks.
     *
     * The caller must hold mutex. Entries whose chain already ended are dropped
     * first, so the registry stays bounded by the number of live chains.
     *
     * @param chain Chain to remember.
     */
    void rememberChain(const std::shared_ptr<Chain>& chain);

    /**
     * @brief Drops live-registry entries whose chain has ended.
     *
     * The caller must hold mutex.
     */
    void pruneChains();

    /// How a bounded drain wait ended.
    enum class DrainOutcome
    {
        Drained,   ///< Every chain finished within the bound.
        TimedOut,  ///< The bound elapsed with a chain still running.
        Cancelled  ///< A "stop everything" request invalidated the wait.
    };

    /// How a takeover ended.
    enum class TakeOverOutcome
    {
        TakenOver,      ///< Nothing is left running, the new command may start.
        StillStopping,  ///< A predecessor outlived the bound; refuse the command.
        Cancelled       ///< cancelAll() arrived while waiting; refuse the command.
    };

    /// Awaits, bounded, until every given chain has no live run left.
    ///
    /// The wait is cooperative: the caller's coroutine suspends between polls
    /// instead of blocking its thread, so the chains keep the ability to unwind.
    /// It polls rather than waiting on a notification because the bound must not
    /// destroy a parked waiter (see kDrainPollInterval).
    ///
    /// @param chains   Chains to wait for; consumable, keeps them alive meanwhile.
    /// @param timeout  Upper bound for the wait.
    /// @param generation Cancellation generation the caller observed before
    ///                 starting to wait; passing it makes the wait stop early with
    ///                 DrainOutcome::Cancelled once cancelAll() bumps it. A caller
    ///                 that is itself cancelling (cancelAllAndWait) leaves it empty.
    /// @return Why the wait ended.
    vine::async::Task<DrainOutcome> waitChainsDrained(std::vector<std::shared_ptr<Chain>> chains,
                                                     std::chrono::milliseconds                timeout,
                                                     std::optional<std::uint64_t>              generation = std::nullopt);

    /// Returns a strong reference to every chain that has not ended yet.
    ///
    /// Must not be called with mutex held. Entries whose chain already ended are
    /// dropped first, so the result is bounded by the number of live chains.
    ///
    /// @return Snapshot of the live chains.
    std::vector<std::shared_ptr<Chain>> collectLiveChains();

    /// Requests cancellation of every live chain.
    ///
    /// This is the "stop everything" primitive: it bumps cancel_generation first (a
    /// takeover waiting for chains to unwind belongs to the set of things that
    /// stops) and then requests a stop on every live chain.
    ///
    /// @return The chains that were live when the request was made.
    std::vector<std::shared_ptr<Chain>> cancelLiveChains();

    /**
     * @brief Asks every live chain to stop and waits for them to unwind.
     *
     * Taking over the foreground chain alone is not enough for the exclusivity an
     * Exclusive command asks for: a detached command keeps its own chain, so
     * cancelling only the foreground would let the new command run next to it.
     * Either every chain unwinds within the bound, or the takeover is refused
     * instead of silently overlapping.
     *
     * A takeover in progress is itself cancellable: cancelAll() bumps the
     * cancellation generation, which ends the wait right away. Without that, "stop
     * everything" (application shutdown) would still sit out the whole bound before
     * the waiting command gave up.
     *
     * @param command Command whose rights are being claimed; only used for logging.
     * @return How the takeover ended.
     */
    vine::async::Task<TakeOverOutcome> takeOverForeground(const Command& command);

    /**
     * @brief Decides whether a command may run and gives it a chain.
     *
     * Evaluation of the serialization gate, becoming the foreground chain and
     * taking the gate flag happen in one critical section, so two top-level
     * commands racing on different threads cannot both pass a check whose effect
     * they only apply later. The ambient progress host is sampled before that
     * section (its accessor locks the progress registry, which must not be nested
     * with this manager's mutex); the progress host itself is created by the caller
     * after this returns, and has to outlive the critical section anyway.
     *
     * @param flags     Flags of the command being admitted.
     * @param exclusive Whether the command takes the foreground over the gate.
     * @param scope     Whether the command starts a chain or joins its parent's.
     * @param chain     Chain of a nested command, or an empty pointer for a
     *                  top-level one; replaced with the chain to run on.
     * @return true when the command may run, false when the gate refuses it.
     */
    bool admit(CommandFlags flags, bool exclusive, ChainScope scope, std::shared_ptr<Chain>& chain);

    /**
     * @brief Reports a finished execution: outcome log, executed event, history.
     *
     * Every step is best effort. The event handlers are user code that may throw,
     * and the history record is bookkeeping, so none of it may turn a finished
     * command into a failed one.
     *
     * @param owner   Manager firing the event.
     * @param command Command that ran; must outlive the call.
     * @param result  Outcome to report.
     */
    void report(CommandManager& owner, Command& command, const CommandResult& result);

    /// Appends the execution to the bounded history; best effort.
    void recordHistory(const Command& command, const CommandResult& result);

    /// Application that owns this manager (non-owning).
    Application* app;

    /// Chain the user interacts with: the most recent top-level chain.
    std::shared_ptr<Chain> foreground;

    /// Every chain that has not ended yet; cancelAll() stops them all.
    std::vector<std::weak_ptr<Chain>> live_chains;

    /// True while an admitted top-level LongRunning command has not drained.
    bool foreground_busy{ false };

    /// True while an admitted top-level Exclusive command has not drained.
    ///
    /// An Exclusive command is let past the busy gate on purpose - taking over the
    /// running work is what it is for - so this flag is what keeps the second one out
    /// when two of them are submitted at the same time and neither had anything to
    /// take over from. Two Exclusive commands running together is exactly the state
    /// "exclusive" forbids.
    bool exclusive_busy{ false };

    /// Bumped by every "stop everything" request (cancelAll() and cancelAllAndWait()),
    /// so a takeover that is waiting for chains to unwind can notice that the whole
    /// application changed its mind and stop waiting.
    ///
    /// Atomic because the waiters poll it without taking mutex, and it never has to
    /// be consistent with the chain snapshot: a generation change always means
    /// "abandon the wait", an extra or missing bump only costs one poll interval.
    std::atomic<std::uint64_t> cancel_generation{ 0 };

    /// Execution history: executions that ran (by value, oldest first).
    std::deque<CommandHistoryEntry> history;

    /// Registered commands by name (factory + meta class + cached metadata).
    std::map<String, RegisteredCommand> registry;

    /// Disabled commands; the fallback used when the host has no config manager.
    std::vector<String> disabled_fallback;

    /// Owner tag applied to commands registered while it is non-empty.
    String registration_owner;

    /// Snapshot handler invoked before an Undoable command runs (document layer).
    std::function<void()> snapshot_handler;

    /// Command aliases (alias -> canonical command name, or another alias).
    std::map<String, String> aliases;

    /// Guards foreground, live_chains, foreground_busy, history and snapshot_handler,
    /// i.e. everything a running command reads or writes through this manager.
    /// Critical sections here are short: no user code (factory, command, event
    /// handler) runs while it is held. It is also never held together with
    /// Chain::mutex, and never with the progress registry lock behind
    /// ProgressHost::current() (admit samples it before entering, and the progress
    /// host is built after leaving): the few sites that need both take them one
    /// after the other, releasing the first before taking the second.
    mutable std::mutex mutex;

    /// Guards registry, registration_owner and aliases.
    /// Leaf lock: never taken while another lock of this manager is held, and no
    /// user code (factory, command, event handler) runs while it is held.
    mutable std::mutex registry_mutex;
};

void CommandManager::Impl::rememberChain(const std::shared_ptr<Chain>& chain)
{
    pruneChains();
    live_chains.push_back(chain);
}

void CommandManager::Impl::pruneChains()
{
    std::erase_if(live_chains, [](const std::weak_ptr<Chain>& chain) { return chain.expired(); });
}

std::vector<std::shared_ptr<CommandManager::Chain>> CommandManager::Impl::collectLiveChains()
{
    std::vector<std::shared_ptr<Chain>> chains;
    std::lock_guard<std::mutex>         lock(mutex);
    pruneChains();
    chains.reserve(live_chains.size());
    for (const auto& weak : live_chains) {
        if (auto chain = weak.lock()) {
            chains.push_back(std::move(chain));
        }
    }
    return chains;
}

std::vector<std::shared_ptr<CommandManager::Chain>> CommandManager::Impl::cancelLiveChains()
{
    // Bumped before the snapshot: a takeover that is waiting for chains to unwind
    // belongs to the set of things "stop everything" stops, and it notices through
    // the generation.
    cancel_generation.fetch_add(1, std::memory_order_relaxed);

    std::vector<std::shared_ptr<Chain>> chains = collectLiveChains();
    for (const auto& chain : chains) {
        chain->stop_source.request_stop();
    }
    return chains;
}

vine::async::Task<CommandManager::Impl::DrainOutcome> CommandManager::Impl::waitChainsDrained(
    std::vector<std::shared_ptr<Chain>> chains,
    std::chrono::milliseconds           timeout,
    std::optional<std::uint64_t>        generation)
{
    if (chains.empty()) {
        co_return DrainOutcome::Drained;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const bool drained = std::ranges::none_of(chains, [](const std::shared_ptr<Chain>& chain) {
            return chain->liveRuns() > 0;
        });
        if (drained) {
            co_return DrainOutcome::Drained;
        }
        // Checked after the liveness check: a chain that finished while the
        // stop-everything request came in is a success, not an abort.
        if (generation.has_value() && cancel_generation.load(std::memory_order_relaxed) != *generation) {
            co_return DrainOutcome::Cancelled;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            co_return DrainOutcome::TimedOut;
        }
        co_await vine::async::sleepFor(kDrainPollInterval);
    }
}

vine::async::Task<CommandManager::Impl::TakeOverOutcome> CommandManager::Impl::takeOverForeground(const Command& command)
{
    std::vector<std::shared_ptr<Chain>> chains = collectLiveChains();

    // Every live chain is cancelled, not just the foreground one: a chain that a
    // detached command owns is invisible to the foreground but would still run
    // next to the new command, which is exactly what Exclusive forbids.
    bool any_live = false;
    for (const auto& chain : chains) {
        if (chain->liveRuns() > 0) {
            any_live = true;
        }
        chain->stop_source.request_stop();
    }

    if (!any_live) {
        co_return TakeOverOutcome::TakenOver;
    }

    // Sampled after the stop request, so a cancelAll() that arrives from here on is
    // observed as a new generation and aborts the wait.
    const std::uint64_t generation = cancel_generation.load(std::memory_order_relaxed);
    switch (co_await waitChainsDrained(std::move(chains), CommandManager::exclusiveDrainTimeout(), generation)) {
    case DrainOutcome::Drained:
        co_return TakeOverOutcome::TakenOver;
    case DrainOutcome::Cancelled:
        V_LOGW("Exclusive command cancelled while waiting for the running chain: {}", toUtf8View(command.name()));
        co_return TakeOverOutcome::Cancelled;
    case DrainOutcome::TimedOut:
        break;
    }

    V_LOGE("Exclusive command rejected: a running chain did not stop within the drain bound: {}", toUtf8View(command.name()));
    co_return TakeOverOutcome::StillStopping;
}

bool CommandManager::Impl::admit(CommandFlags flags, bool exclusive, ChainScope scope, std::shared_ptr<Chain>& chain)
{
    if (scope == ChainScope::Nested) {
        if (!chain) {
            // A nested command always arrives with its parent's chain; keep a
            // private one instead of dereferencing nothing.
            chain = std::make_shared<Chain>();
        }
        return true;
    }

    // Sampled before the critical section: ProgressHost::current() locks the
    // progress registry, which is a module-global lock, and taking it while this
    // manager's mutex is held would nest the two (the manager never calls out
    // under its own lock). The ambient host only complements the gate flag; the
    // gate decision itself is still one atomic step with the flag update below.
    const bool ambient_host_busy = vine::progress::ProgressHost::current() != nullptr;

    std::lock_guard<std::mutex> lock(mutex);
    if (exclusive) {
        // One Exclusive command at a time; see exclusive_busy. A command that took
        // over another Exclusive one is fine: that one drained before this check.
        if (exclusive_busy) {
            return false;
        }
        exclusive_busy = true;
    }
    else if (foreground_busy || ambient_host_busy) {
        return false;
    }

    chain = std::make_shared<Chain>();
    foreground = chain;
    rememberChain(chain);
    if (hasFlag(flags, CommandFlags::LongRunning)) {
        foreground_busy = true;
    }
    return true;
}

void CommandManager::Impl::report(CommandManager& owner, Command& command, const CommandResult& result)
{
    logOutcome(command, result);

    CommandExecutedEventArgs args(&command, result);
    fireEvent(owner.executed, owner, args);

    recordHistory(command, result);
}

void CommandManager::Impl::recordHistory(const Command& command, const CommandResult& result)
{
    // The execution is recorded by value: the command instance is owned by the caller
    // and is destroyed as soon as the execution finishes, so keeping a pointer to it
    // would leave the history dangling. The snapshot is built before the lock is
    // taken - name() and getType() are command code, and no user callback runs while
    // this manager's lock is held - and a record that cannot be built or appended is
    // only logged, never reported.
    try {
        const CommandHistoryEntry      entry{ command.name(), command.getType(), result };
        std::lock_guard<std::mutex> lock(mutex);
        if (history.size() >= CommandManager::maxHistoryEntries()) {
            history.pop_front();
        }
        history.push_back(entry);
    }
    catch (const std::exception& e) {
        V_LOGE("Failed to record the execution history: {}", e.what());
    }
    catch (...) {
        V_LOGE("Failed to record the execution history");
    }
}

String CommandManager::Impl::resolveName(const String& name) const
{
    // A registered command name wins over an alias with the same name.
    if (registry.find(name) != registry.end()) {
        return name;
    }

    // Aliases may point at other aliases, so keep walking until a registered
    // command shows up. visited bounds the walk: a cycle (a -> b -> a) stops at
    // the repeated name, which no longer resolves and is reported as such.
    std::vector<String> visited;
    String              current = name;
    while (true) {
        const auto it = aliases.find(current);
        if (it == aliases.end()) {
            return current;
        }
        if (std::ranges::find(visited, current) != visited.end()) {
            return current;
        }
        visited.push_back(current);
        current = it->second;
        if (registry.find(current) != registry.end()) {
            return current;
        }
    }
}

ConfigManager* CommandManager::Impl::configManager() const
{
    return app != nullptr ? app->configManager() : nullptr;
}

std::vector<String> CommandManager::Impl::disabledList() const
{
    if (ConfigManager* cfg = configManager(); cfg != nullptr) {
        return cfg->getStringArray(CommandManager::disabledConfigKey());
    }

    std::lock_guard<std::mutex> lock(registry_mutex);
    return disabled_fallback;
}

bool CommandManager::Impl::isDisabled(const String& name) const
{
    const std::vector<String> list = disabledList();
    return std::ranges::find(list, name) != list.end();
}

bool CommandManager::Impl::isDisabledRegistration(const String& name) const
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto                  it = registry.find(resolveName(name));
    return it != registry.end() && !it->second.enabled;
}

void CommandManager::Impl::setDisabledPreference(const String& name, bool disabled)
{
    if (ConfigManager* cfg = configManager(); cfg != nullptr) {
        std::vector<String> list = cfg->getStringArray(CommandManager::disabledConfigKey());
        if (updateDisabledList(list, name, disabled)) {
            cfg->setStringArray(CommandManager::disabledConfigKey(), list);
        }
        return;
    }

    // No host configuration: keep the preference for this process only.
    std::lock_guard<std::mutex> lock(registry_mutex);
    updateDisabledList(disabled_fallback, name, disabled);
}

/**
 * @brief Private implementation of CommandExecutionContext.
 */
class CommandManager::Context : public CommandExecutionContext {
  public:
    Context(CommandManager* manager, std::shared_ptr<Chain> chain)
      : manager_(manager)
      , chain_(std::move(chain))
    {}

    Application* application() const override
    {
        return manager_->application();
    }

    std::stop_token stopToken() const override
    {
        return chain_->stop_source.get_token();
    }

    bool isCancelled() const override
    {
        return chain_->stop_source.stop_requested();
    }

    vine::async::Task<CommandResult> executeChild(const String& name) override
    {
        // Bound the nesting so a command that calls itself can only exhaust the
        // chain's budget, not the process's coroutine frames.
        if (chain_->stackSize() >= CommandManager::maxChainDepth()) {
            V_LOGE("Command nesting is too deep; refusing child command: {}", toUtf8View(name));
            co_return failedResult(String(u8"Command nesting is too deep"));
        }

        // The factory is user code: a child that cannot even be built must come
        // back as a Failed outcome, not as an exception in the parent's frame.
        bool                     disabled = false;
        std::unique_ptr<Command> command;
        try {
            command = manager_->createCommandByName(name, &disabled);
        }
        catch (const std::exception& e) {
            V_LOGE("Child command factory threw: {}: {}", toUtf8View(name), e.what());
            co_return failedResultFromException(e);
        }
        catch (...) {
            V_LOGE("Child command factory threw: {}", toUtf8View(name));
            co_return failedResult(String(u8"command factory threw"));
        }

        if (!command) {
            co_return failedResult(refusalMessage(name, disabled));
        }
        co_return co_await manager_->executeCommandAsyncImpl(command.get(), chain_, ChainScope::Nested);
    }

  private:
    CommandManager*        manager_;
    std::shared_ptr<Chain> chain_;
};

CommandManager::CommandManager(Application* app)
  : d(new Impl(app))
{}

CommandManager::~CommandManager()
{
    // Nothing is cancelled or awaited here on purpose: blocking a destructor on
    // coroutines unwinding would deadlock whenever a command needs the thread being
    // torn down to make progress. The host establishes that precondition with
    // cancelAllAndWait() (Application::shutdown() does); violating it is reported
    // rather than passed over silently, because the live frames still point at this
    // manager (ChainGuard::impl) and at the chains (Context::manager_) and would resume
    // into freed memory.
    std::vector<std::weak_ptr<Chain>> live;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        live = d->live_chains;
    }

    // Checked outside the manager mutex: liveRuns() takes the chain's own mutex, and
    // the two are never held together.
    const bool still_running = std::ranges::any_of(live, [](const std::weak_ptr<Chain>& weak) {
        const std::shared_ptr<Chain> chain = weak.lock();
        return chain != nullptr && chain->liveRuns() > 0;
    });

    if (still_running) {
        V_LOGW("CommandManager destroyed while command chains are still running");
    }
}

raw_ptr<Application> CommandManager::application() const noexcept
{
    return d->app;
}

CommandResult CommandManager::executeCommand(Command* command)
{
    return vine::async::syncWait(executeCommandAsync(command));
}

CommandResult CommandManager::executeCommand(const String& name)
{
    return vine::async::syncWait(executeCommandAsync(name));
}

vine::async::Task<CommandResult> CommandManager::executeCommandAsync(Command* command)
{
    // The disabled-instance check is inside the funnel on purpose: it calls
    // command->name() (user code) and copies the name (allocates).
    return runGuarded([this, command]() -> vine::async::Task<CommandResult> {
        // A caller-supplied instance runs under its own name, so it must not become
        // a back door around a disabled registration. An instance whose name is not
        // registered (a private command built on the spot) is the caller's business
        // and runs as before.
        const String command_name = command != nullptr ? command->name() : String{};
        if (command != nullptr && d->isDisabledRegistration(command_name)) {
            V_LOGI("Command '{}' is disabled; refusing the caller-supplied instance", toUtf8View(command_name));
            co_return failedResult(refusalMessage(command_name, /*disabled=*/true));
        }

        co_return co_await executeCommandAsyncImpl(command, /*chain=*/{}, ChainScope::TopLevel);
    });
}

vine::async::Task<CommandResult> CommandManager::executeCommandAsync(const String& name)
{
    // The copy happens here, while the caller's argument is still alive: the task is
    // lazy, so a coroutine that held a reference to name would read a destroyed
    // temporary when the caller awaits it later.
    return executeNamedCommand(String(name));
}

vine::async::Task<CommandResult> CommandManager::executeNamedCommand(String name)
{
    return runGuarded([this, name = std::move(name)]() -> vine::async::Task<CommandResult> {
        bool                     disabled = false;
        std::unique_ptr<Command> command  = createCommandByName(name, &disabled);
        if (!command) {
            // A command the user disabled is a different situation from a name that
            // was never registered: it can be re-enabled in the command manager.
            co_return failedResult(refusalMessage(name, disabled));
        }
        co_return co_await executeCommandAsyncImpl(command.get(), /*chain=*/{}, ChainScope::TopLevel);
    });
}

std::unique_ptr<Command> CommandManager::createCommandByName(const String& name, bool* disabled)
{
    std::function<Command*()> factory;
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        const auto                  it = d->registry.find(d->resolveName(name));
        if (it == d->registry.end() || !it->second.factory) {
            return nullptr;
        }
        if (!it->second.enabled) {
            if (disabled != nullptr) {
                *disabled = true;
            }
            return nullptr;
        }
        factory = it->second.factory;
    }

    // Called outside the lock: a factory is user code and may register commands
    // itself, which would deadlock the non-recursive registry lock.
    return std::unique_ptr<Command>(factory());
}

vine::async::Task<CommandResult> CommandManager::executeCommandAsyncImpl(Command* command, std::shared_ptr<Chain> chain, ChainScope scope)
{
    if (!command) {
        co_return failedResult(String(u8"Command is null"));
    }

    const CommandFlags flags     = command->flags();
    const bool         exclusive = hasFlag(flags, CommandFlags::Exclusive);
    const bool         top_level = scope == ChainScope::TopLevel;

    if (exclusive && top_level) {
        switch (co_await d->takeOverForeground(*command)) {
        case Impl::TakeOverOutcome::TakenOver:
            break;
        case Impl::TakeOverOutcome::StillStopping:
            co_return failedResult(String(u8"Another operation is still stopping"));
        case Impl::TakeOverOutcome::Cancelled:
            co_return cancelledResult();
        }
    }

    if (!d->admit(flags, exclusive, scope, chain)) {
        // Visible for the callers that cannot read the result: a detached command
        // that is refused would otherwise fail silently.
        V_LOGW("Command refused by the serialization gate: {}", toUtf8View(command->name()));
        co_return failedResult(String(u8"Another operation is in progress"));
    }

    // Whether this run took the serialization gate (a top-level LongRunning
    // command) or the exclusive occupancy (a top-level Exclusive command); the guard
    // below releases whichever it is when the run ends.
    const bool holds_gate      = top_level && hasFlag(flags, CommandFlags::LongRunning);
    const bool holds_exclusive = top_level && exclusive;

    // Declared before the progress host and the stack guard below, so it is
    // destroyed after both: the chain is reported drained only once this
    // frame's host and stack entry are gone, which keeps an Exclusive command
    // that waited for the drain from resuming into a half-torn-down state. The
    // gate is released before the drain signal, so a command woken by it also
    // sees a cleared gate.
    struct ChainGuard {
        Impl*  impl;
        Chain* chain;
        bool   holds_gate;
        bool   holds_exclusive;

        ~ChainGuard()
        {
            if (holds_gate || holds_exclusive) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                if (holds_gate) {
                    impl->foreground_busy = false;
                }
                if (holds_exclusive) {
                    impl->exclusive_busy = false;
                }
            }
            chain->leaveChain();
        }
    } chain_guard{ d.get(), chain.get(), holds_gate, holds_exclusive };

    // Every LongRunning command — top-level or nested — owns its own progress
    // host, pushed onto the foreground stack so a nested child takes over the
    // bar and the parent's host is restored when the child ends. The host is a
    // coroutine-local: it lives for this command's execution and is destroyed
    // (popping the stack) when the command completes. All hosts in the chain
    // bind to the same chain-wide cancellation source.
    std::unique_ptr<vine::progress::ProgressHost> progress_host;
    if (hasFlag(flags, CommandFlags::LongRunning)) {
        progress_host = std::make_unique<vine::progress::ProgressHost>(chain->stop_source);
        progress_host->setForeground(true);
    }

    chain->enter(command);

    V_LOGI("Executing command: {}", toUtf8View(command->name()));

    // Leaves the chain's stack when the command completes, keeping the manager
    // usable. The chain outlives this frame — the shared_ptr parameter is
    // destroyed after the frame's locals — so a raw pointer is enough.
    struct StackGuard {
        Chain*   chain;
        Command* command;

        ~StackGuard()
        {
            chain->leaveStack(command);
        }
    } guard{ chain.get(), command };

    Context context(this, chain);

    // Undoable commands notify the document to snapshot its state first. The
    // handler is taken under the lock and called outside it: it is user code from
    // the document layer, which may replace it at any time. A snapshot that fails
    // must stop the command: running it without a snapshot would make the later
    // undo restore a state the document never had.
    std::function<void()> snapshot_handler;
    if (hasFlag(flags, CommandFlags::Undoable)) {
        std::lock_guard<std::mutex> lock(d->mutex);
        snapshot_handler = d->snapshot_handler;
    }

    if (snapshot_handler) {
        try {
            snapshot_handler();
        }
        catch (const std::exception& e) {
            V_LOGE("Snapshot handler failed; command not executed: {}", e.what());
            co_return failedResultFromException(e);
        }
        catch (...) {
            V_LOGE("Snapshot handler failed; command not executed");
            co_return failedResult(String(u8"snapshot handler failed"));
        }
    }

    CommandExecutingEventArgs args(command);
    fireEvent(executing, *this, args);

    CommandResult result;
    try {
        result = co_await command->execute(&context);
    }
    catch (const vine::async::TaskCancelledException&) {
        // Nested commands propagate the cancellation upward by default; only the
        // command that started the chain reports it as a Cancelled result. The
        // chain's own stack is the judge: it still holds this command, so a
        // depth above one means a parent is waiting for the result. A command
        // that wants to handle a cancelled child catches the exception in its
        // own execute().
        if (chain->stackSize() > 1) {
            // It ran (the executing event was fired), so it owes its own executed
            // event and history entry before the exception travels to the parent:
            // a consumer pairing the two events must see a pair for every run.
            d->report(*this, *command, cancelledResult());
            throw;
        }
        result = cancelledResult();
    }
    catch (const std::exception& e) {
        // Anything else is reported as a Failed outcome instead of propagating:
        // commands are started from UI event handlers and from detached tasks
        // where an escaping exception terminates the process.
        V_LOGE("Command threw an exception: {}: {}", toUtf8View(command->name()), e.what());
        result = failedResultFromException(e);
    }
    catch (...) {
        V_LOGE("Command threw a non-standard exception: {}", toUtf8View(command->name()));
        result = failedResult(String(u8"command threw an exception"));
    }

    d->report(*this, *command, result);

    co_return result;
}

void CommandManager::executeDetached(const String& name)
{
    // Backstop for the fire-and-forget path: DetachedTask terminates the process
    // on an uncaught exception, and this wrapper has no caller to report to.
    // Anything thrown before the execution path takes over (a throwing factory,
    // for instance) is swallowed here with a log.
    //
    // The trigger is a UI element (a ribbon button or action) and nobody waits for
    // the outcome, so a refusal or a failure has to be reported here or it would be
    // invisible. Commands entered in the console are started through
    // executeCommandAsync() and report themselves, so they are not reported twice.
    Application* owner = d->app;
    try {
        // Starting the task can fail too (copying the name, allocating the coroutine
        // frame), and that happens before the wrapper exists, so it needs this catch
        // as well: a fire-and-forget entry point must not throw at its caller.
        [](Application* app, vine::async::Task<CommandResult> task) -> vine::async::DetachedTask {
            CommandResult result{ CommandStatus::Failed };
            try {
                result = co_await std::move(task);
            }
            catch (const std::exception& e) {
                V_LOGE("Detached command failed: {}", e.what());
                co_return;
            }
            catch (...) {
                V_LOGE("Detached command failed with a non-standard exception");
                co_return;
            }

            if (result.succeeded() || app == nullptr) {
                co_return;
            }
            // The command most likely ran to its end on another thread (a timer or an
            // asynchronous read completes it), so this must not touch userIO() directly:
            // the GUI one writes to a QWidget.
            reportToUser(*app, result.message().empty() ? String(u8"命令执行失败") : result.message());
        }(owner, executeCommandAsync(name));
    }
    catch (const std::exception& e) {
        V_LOGE("Detached command could not be started: {}", e.what());
    }
    catch (...) {
        V_LOGE("Detached command could not be started");
    }
}

std::shared_ptr<CommandManager::Chain> CommandManager::foregroundChain() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return d->foreground;
}

raw_ptr<Command> CommandManager::currentCommand() const
{
    const std::shared_ptr<Chain> chain = foregroundChain();
    return chain ? chain->innermost() : nullptr;
}

int CommandManager::runningCount() const
{
    const std::shared_ptr<Chain> chain = foregroundChain();
    return chain ? chain->stackSize() : 0;
}

void CommandManager::cancelCurrent()
{
    if (const std::shared_ptr<Chain> chain = foregroundChain(); chain) {
        chain->stop_source.request_stop();
    }
}

void CommandManager::cancelAll()
{
    // The snapshot is only a by-product here: cancelLiveChains() already stopped
    // every chain it returns.
    static_cast<void>(d->cancelLiveChains());
}

bool CommandManager::cancelAllAndWait(std::chrono::milliseconds timeout)
{
    std::vector<std::shared_ptr<Chain>> chains = d->cancelLiveChains();
    if (chains.empty()) {
        return true;
    }

    // Bounded and cooperative, driven on the calling thread: this is the shutdown
    // path, where "everything stopped" is a precondition for destroying the manager
    // (a live frame still points at it) but a hostile command must not hang the
    // teardown forever. The wait watches no generation here - this call is the one
    // that bumps it.
    return vine::async::syncWait(d->waitChainsDrained(std::move(chains), timeout)) == Impl::DrainOutcome::Drained;
}

int CommandManager::historyCount() const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    return static_cast<int>(d->history.size());
}

std::optional<CommandHistoryEntry> CommandManager::historyAt(int index) const
{
    std::lock_guard<std::mutex> lock(d->mutex);
    if (index < 0 || index >= static_cast<int>(d->history.size())) {
        return std::nullopt;
    }
    return d->history[static_cast<std::size_t>(index)];
}

void CommandManager::clearHistory()
{
    std::lock_guard<std::mutex> lock(d->mutex);
    d->history.clear();
}

bool CommandManager::registerCommand(TypeId command_class, String name, std::function<Command*()> factory)
{
    if (name.empty() || !factory) {
        V_LOGW("Ignoring command registration with an empty name or an empty factory");
        return false;
    }

    // Cheap rejection first: registering an existing name must not pay for a
    // probe whose result would be thrown away.
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        if (d->registry.find(name) != d->registry.end()) {
            return false;
        }
    }

    // The listing metadata is cached once, here: commandInfos() must not
    // instantiate commands (a factory can be expensive, and the caller may not
    // expect its side effects on every query). The probe runs outside every lock
    // and only logs on failure: a factory that returns nullptr or throws (its
    // constructor may need services that are not up yet at load time) still gets
    // the command registered, just without cached metadata.
    String group;
    String description;
    try {
        std::unique_ptr<Command> probe(factory());
        if (probe) {
            group       = probe->group();
            description = probe->description();
        }
        else {
            V_LOGW("Command factory returned no instance; listing metadata stays empty: {}", toUtf8View(name));
        }
    }
    catch (const std::exception& e) {
        V_LOGW("Command factory threw during the metadata probe: {}: {}", toUtf8View(name), e.what());
    }
    catch (...) {
        V_LOGW("Command factory threw during the metadata probe: {}", toUtf8View(name));
    }

    // Applied here, outside every lock: a command the user disabled comes back
    // disabled when its plugin registers it again at the next start.
    const bool enabled = !d->isDisabled(name);

    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        RegisteredCommand           entry{ command_class, std::move(factory), d->registration_owner, std::move(group), std::move(description) };
        entry.enabled = enabled;
        if (!d->registry.emplace(std::move(name), std::move(entry)).second) {
            return false;
        }
    }
    fireCommandsChanged(*this);
    return true;
}

void CommandManager::setRegistrationOwner(String owner)
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    d->registration_owner = std::move(owner);
}

String CommandManager::registrationOwner() const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    return d->registration_owner;
}

bool CommandManager::unregisterCommand(const String& name)
{
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        if (d->registry.erase(name) == 0) {
            return false;
        }
    }
    fireCommandsChanged(*this);
    return true;
}

bool CommandManager::unregisterCommand(TypeId command_class)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        for (auto it = d->registry.begin(); it != d->registry.end();) {
            if (it->second.class_type == command_class) {
                it      = d->registry.erase(it);
                removed = true;
            }
            else {
                ++it;
            }
        }
    }
    if (removed) {
        fireCommandsChanged(*this);
    }
    return removed;
}

const String& CommandManager::disabledConfigKey()
{
    static const String s_key{ u8"commands.disabled" };
    return s_key;
}

std::vector<String> CommandManager::disabledCommands() const
{
    return d->disabledList();
}

bool CommandManager::isCommandEnabled(const String& name) const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    const auto it = d->registry.find(d->resolveName(name));
    return it != d->registry.end() && it->second.enabled;
}

bool CommandManager::setCommandEnabled(const String& name, bool enabled)
{
    if (name.empty()) {
        V_LOGW("Cannot enable or disable a command without a name");
        return false;
    }

    // The flag and the preference belong to the command the name runs, not to the
    // alias the caller typed: every execution path resolves aliases, so a
    // preference recorded under the alias would be silently ineffective.
    String canonical;
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        canonical = d->resolveName(name);
    }

    // Written first: the preference has to survive a command that is not registered
    // right now, which is the normal case for a plugin that is not loaded yet.
    d->setDisabledPreference(canonical, !enabled);

    bool applied = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        const auto                  it = d->registry.find(canonical);
        if (it != d->registry.end()) {
            changed            = it->second.enabled != enabled;
            it->second.enabled = enabled;
            applied            = true;
        }
    }
    if (changed) {
        fireCommandsChanged(*this);
    }

    if (applied) {
        V_LOGI("Command '{}' is now {}", toUtf8View(canonical), enabled ? "enabled" : "disabled");
    }
    else {
        V_LOGI("Command '{}' is not registered; the preference applies when it registers again: {}", toUtf8View(canonical),
               enabled ? "enabled" : "disabled");
    }
    return true;
}

bool CommandManager::registerAlias(const String& alias, const String& target)
{
    if (alias.empty() || target.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        if (!d->aliases.emplace(alias, target).second) {
            V_LOGW("Alias already registered, ignoring target: {} -> {}", toUtf8View(alias), toUtf8View(target));
            return false;
        }
    }
    fireCommandsChanged(*this);
    return true;
}

bool CommandManager::unregisterAlias(const String& alias)
{
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        if (d->aliases.erase(alias) == 0) {
            return false;
        }
    }
    fireCommandsChanged(*this);
    return true;
}

bool CommandManager::isRegistered(const String& name) const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    const auto it = d->registry.find(d->resolveName(name));
    return it != d->registry.end() && it->second.enabled;
}

std::vector<String> CommandManager::names() const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    std::vector<String>         result;
    result.reserve(d->registry.size());
    for (const auto& entry : d->registry) {
        result.push_back(entry.first);
    }
    return result;
}

std::vector<std::pair<String, String>> CommandManager::aliases() const
{
    std::lock_guard<std::mutex>                             lock(d->registry_mutex);
    std::vector<std::pair<String, String>> result;
    result.reserve(d->aliases.size());
    for (const auto& entry : d->aliases) {
        result.emplace_back(entry.first, entry.second);
    }
    return result;
}

std::vector<CommandInfo> CommandManager::commandInfos() const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);

    // std::map keeps registry keys sorted, so the result is ordered by name.
    std::vector<CommandInfo>      result;
    std::map<String, std::size_t> index_by_name;
    result.reserve(d->registry.size());

    for (const auto& entry : d->registry) {
        CommandInfo info;
        info.name        = entry.first;
        info.group       = entry.second.group;
        info.description = entry.second.description;
        info.owner       = entry.second.owner;
        info.enabled     = entry.second.enabled;
        index_by_name.emplace(info.name, result.size());
        result.push_back(std::move(info));
    }

    // Attach every alias to the entry of the command it finally resolves to, so
    // an alias of an alias is listed with its command as well.
    for (const auto& alias : d->aliases) {
        const auto it = index_by_name.find(d->resolveName(alias.second));
        if (it != index_by_name.end()) {
            result[it->second].aliases.push_back(alias.first);
        }
    }

    return result;
}

std::vector<CommandInfo> CommandManager::commandInfosForPlugin(const String& owner) const
{
    std::vector<CommandInfo> result;
    for (const auto& info : commandInfos()) {
        if (info.owner == owner) {
            result.push_back(info);
        }
    }
    return result;
}

void CommandManager::setSnapshotHandler(std::function<void()> handler)
{
    std::lock_guard<std::mutex> lock(d->mutex);
    d->snapshot_handler = std::move(handler);
}

V_APPFW_NS_END
