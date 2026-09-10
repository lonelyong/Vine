#include <vine/appfw/CommandManager.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/Command.hpp>

#include <vine/async/AsyncEvent.hpp>
#include <vine/async/Cancellation.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/SyncWait.hpp>
#include <vine/async/Task.hpp>
#include <vine/async/WithTimeout.hpp>

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
};

/// Returns whether a command flag set contains the given bit.
constexpr bool hasFlag(CommandFlags value, CommandFlags bit) noexcept
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(bit)) != 0;
}

/// Views a String's UTF-8 bytes without allocating, for the noexcept log paths.
std::string_view toUtf8View(const String& s) noexcept
{
    return { reinterpret_cast<const char*>(s.data()), s.size() };
}

/// Severity of a logNoThrow() message; the level macros cannot be selected dynamically.
enum class LogSeverity { Info, Warning, Error };

/**
 * @brief Logs one message, swallowing every logging failure.
 *
 * Logging must never decide what happens next: these calls run inside catch
 * blocks, inside detached coroutines (where a throw reaches DetachedTask and
 * terminates the process) and in the middle of a command, where a formatting
 * failure would otherwise turn a finished command into a failed one. The message
 * is assembled inside the guarded call, so nothing on the caller's side (argument
 * conversion included) can throw either.
 *
 * @param severity Level to report at.
 * @param context  Short description of what happened.
 * @param detail   Optional first detail, appended after a colon.
 * @param suffix   Optional second detail, appended after another colon.
 */
void logNoThrow(LogSeverity severity, const char* context, std::string_view detail = {}, std::string_view suffix = {}) noexcept
{
    try {
        std::string message{ context };
        if (!detail.empty()) {
            message += ": ";
            message += detail;
        }
        if (!suffix.empty()) {
            message += ": ";
            message += suffix;
        }

        switch (severity) {
        case LogSeverity::Info:
            V_LOGI("{}", message);
            break;
        case LogSeverity::Warning:
            V_LOGW("{}", message);
            break;
        case LogSeverity::Error:
            V_LOGE("{}", message);
            break;
        }
    }
    catch (...) {
        // A logger that cannot log is not allowed to become the caller's problem.
    }
}

/**
 * @brief logNoThrow() at info level.
 *
 * @param context Short description of what happened.
 * @param detail  Optional first detail, appended after a colon.
 * @param suffix  Optional second detail, appended after another colon.
 */
void logInfoNoThrow(const char* context, std::string_view detail = {}, std::string_view suffix = {}) noexcept
{
    logNoThrow(LogSeverity::Info, context, detail, suffix);
}

/**
 * @brief logNoThrow() at warning level.
 *
 * @param context Short description of what happened.
 * @param detail  Optional first detail, appended after a colon.
 * @param suffix  Optional second detail, appended after another colon.
 */
void logWarnNoThrow(const char* context, std::string_view detail = {}, std::string_view suffix = {}) noexcept
{
    logNoThrow(LogSeverity::Warning, context, detail, suffix);
}

/**
 * @brief logNoThrow() at error level.
 *
 * @param context Short description of what happened.
 * @param detail  Optional first detail, appended after a colon.
 * @param suffix  Optional second detail, appended after another colon.
 */
void logErrorNoThrow(const char* context, std::string_view detail = {}, std::string_view suffix = {}) noexcept
{
    logNoThrow(LogSeverity::Error, context, detail, suffix);
}

/// Builds the failure message of an exception; empty when even that cannot be built.
String messageFromException(const std::exception& e) noexcept
{
    try {
        return String(reinterpret_cast<const char8_t*>(e.what()));
    }
    catch (...) {
        return {};
    }
}

/// Builds the Failed result reported when a command throws.
CommandResult failureFromException(const std::exception& e)
{
    String message = messageFromException(e);
    if (message.empty()) {
        message = String(u8"command threw an exception");
    }
    return CommandResult(CommandStatus::Failed, std::move(message));
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

    /// Unregisters a finished run, signalling drained when it was the last one.
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
    /// that a drain signal is never sent while part of the frame is still alive.
    int runs{ 0 };

    /// Cancellation source of the whole chain; stopped, never replaced.
    std::stop_source stop_source;

    /// Set once when the last run of this chain finishes.
    vine::async::AsyncEvent drained;

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
    bool is_drained = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        is_drained = (--runs == 0);
    }

    if (is_drained) {
        // Resumed outside the lock: an Exclusive command waiting for this chain
        // continues here, and must never run under our mutex. The caller makes
        // sure this happens only after the frame's stack entry and progress host
        // are gone, so the resuming command sees a settled state.
        drained.set();
    }
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

    /// Awaits a single signal of an AsyncEvent.
    static vine::async::Task<void> waitSignal(vine::async::AsyncEvent& event);

    /**
     * @brief Waits until the chain has no live run left, bounded.
     *
     * @param chain Chain to wait for; must outlive the wait.
     * @return true when the chain drained, false when the bound elapsed first.
     */
    static vine::async::Task<bool> waitDrained(Chain& chain);

    /**
     * @brief Asks the foreground chain to stop and waits for it to unwind.
     *
     * The wait is cooperative and bounded by exclusiveDrainTimeout(); a command
     * that ignores its token makes the takeover impossible, and running the new
     * command next to it would break the exclusivity the caller asked for, so it
     * is refused instead of silently overlapping.
     *
     * @param command Command whose rights are being claimed; only used for logging.
     * @return true when nothing is in the way, or when it stopped in time; false
     *         when the foreground chain did not stop within the bound.
     */
    vine::async::Task<bool> takeOverForeground(const Command& command);

    /**
     * @brief Decides whether a command may run and gives it a chain.
     *
     * Evaluation of the serialization gate, becoming the foreground chain and
     * taking the gate flag happen in one critical section, so two top-level
     * commands racing on different threads cannot both pass a check whose effect
     * they only apply later. The progress host is created by the caller after this
     * returns: building it here would nest the manager's lock with the progress
     * registry's lock, and the host has to outlive the critical section anyway.
     *
     * @param flags     Flags of the command being admitted.
     * @param exclusive Whether the command takes the foreground over the gate.
     * @param nested    Whether the command joins its parent's chain instead.
     * @param chain     Chain of a nested command, or an empty pointer for a
     *                  top-level one; replaced with the chain to run on.
     * @return true when the command may run, false when the gate refuses it.
     */
    bool admit(CommandFlags flags, bool exclusive, bool nested, std::shared_ptr<Chain>& chain);

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

    /// Application that owns this manager (non-owning).
    Application* app;

    /// Chain the user interacts with: the most recent top-level chain.
    std::shared_ptr<Chain> foreground;

    /// Every chain that has not ended yet; cancelAll() stops them all.
    std::vector<std::weak_ptr<Chain>> live_chains;

    /// True while an admitted top-level LongRunning command has not drained.
    bool foreground_busy{ false };

    /// Execution history: executions that ran (by value, oldest first).
    std::deque<CommandHistoryEntry> history;

    /// Registered commands by name (factory + meta class + cached metadata).
    std::map<String, RegisteredCommand> registry;

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
    /// Chain::mutex: the few sites that need both (ChainGuard, cancelAll) take them
    /// one after the other, releasing the first before taking the second.
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

vine::async::Task<void> CommandManager::Impl::waitSignal(vine::async::AsyncEvent& event)
{
    co_await event;
}

vine::async::Task<bool> CommandManager::Impl::waitDrained(Chain& chain)
{
    if (chain.liveRuns() == 0) {
        co_return true;
    }

    // Cooperative: this coroutine suspends instead of blocking its thread, so
    // the cancelled chain keeps the ability to unwind. The event is
    // level-triggered, so a drain happening between the check above and the
    // await below is not lost. The bound keeps a command that ignores its
    // cancellation token from holding the caller forever.
    try {
        co_await vine::async::withTimeout(waitSignal(chain.drained), CommandManager::exclusiveDrainTimeout());
        co_return true;
    }
    catch (const vine::async::TimeoutException&) {
        co_return false;
    }
}

vine::async::Task<bool> CommandManager::Impl::takeOverForeground(const Command& command)
{
    std::shared_ptr<Chain> previous;
    {
        std::lock_guard<std::mutex> lock(mutex);
        previous = foreground;
    }

    if (!previous || previous->liveRuns() == 0) {
        co_return true;
    }

    previous->stop_source.request_stop();
    if (!co_await waitDrained(*previous)) {
        logErrorNoThrow("Exclusive command rejected: the running chain did not stop within the drain bound", toUtf8View(command.name()));
        co_return false;
    }
    co_return true;
}

bool CommandManager::Impl::admit(CommandFlags flags, bool exclusive, bool nested, std::shared_ptr<Chain>& chain)
{
    if (nested) {
        if (!chain) {
            // A nested command always arrives with its parent's chain; keep a
            // private one instead of dereferencing nothing.
            chain = std::make_shared<Chain>();
        }
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (!exclusive && (foreground_busy || vine::progress::ProgressHost::current() != nullptr)) {
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
    if (result.succeeded()) {
        logInfoNoThrow("Command succeeded", toUtf8View(command.name()));
    }
    else if (result.status() == CommandStatus::Cancelled) {
        logWarnNoThrow("Command cancelled", toUtf8View(command.name()));
    }
    else {
        logErrorNoThrow("Command failed", toUtf8View(command.name()), toUtf8View(result.message()));
    }

    try {
        CommandExecutedEventArgs args(&command, result);
        owner.executed.trigger(owner, args);
    }
    catch (const std::exception& e) {
        logErrorNoThrow("Executed event handler threw", e.what());
    }
    catch (...) {
        logErrorNoThrow("Executed event handler threw");
    }

    // Record the execution by value: the command instance is owned by the caller
    // and is destroyed as soon as the execution finishes, so keeping a pointer to
    // it would leave the history dangling. The history is bounded so a
    // long-running application cannot grow it without limit.
    //
    // The snapshot is built before the lock is taken: name() and getType() are
    // command code, and no user callback runs while this manager's lock is held.
    // A record that cannot be built or appended is only logged, never reported.
    try {
        const CommandHistoryEntry entry{ command.name(), command.getType(), result };
        std::lock_guard<std::mutex> lock(mutex);
        if (history.size() >= CommandManager::maxHistoryEntries()) {
            history.pop_front();
        }
        history.push_back(entry);
    }
    catch (const std::exception& e) {
        logErrorNoThrow("Failed to record the execution history", e.what());
    }
    catch (...) {
        logErrorNoThrow("Failed to record the execution history");
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

/**
 * @brief Private implementation of CommandExecutionContext.
 */
class CommandManager::Context : public CommandExecutionContext {
  public:
    Context(CommandManager* mgr, Application* app, std::shared_ptr<Chain> chain)
      : mgr_(mgr)
      , app_(app)
      , chain_(std::move(chain))
    {}

    Application* application() const override
    {
        return app_;
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
            logErrorNoThrow("Command nesting is too deep; refusing child command", toUtf8View(name));
            co_return CommandResult(CommandStatus::Failed, String(u8"Command nesting is too deep"));
        }

        // The factory is user code: a child that cannot even be built must come
        // back as a Failed outcome, not as an exception in the parent's frame.
        std::unique_ptr<Command> command;
        try {
            command = mgr_->createCommandByName(name);
        }
        catch (const std::exception& e) {
            logErrorNoThrow("Child command factory threw", toUtf8View(name), e.what());
            co_return failureFromException(e);
        }
        catch (...) {
            logErrorNoThrow("Child command factory threw", toUtf8View(name));
            co_return CommandResult(CommandStatus::Failed, String(u8"command factory threw"));
        }

        if (!command) {
            co_return CommandResult(CommandStatus::Failed, String(u8"Command not registered"));
        }
        co_return co_await mgr_->executeCommandAsyncImpl(command.get(), chain_, /*nested=*/true);
    }

  private:
    CommandManager*        mgr_;
    Application*           app_;
    std::shared_ptr<Chain> chain_;
};

CommandManager::CommandManager(Application* app)
  : d(new Impl(app))
{}

CommandManager::~CommandManager() = default;

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
    // Choke point for exceptions: whatever escapes the execution path (a throwing
    // factory, an allocation failure) becomes a Failed result here. Callers are
    // event handlers and detached tasks, where an escaping exception would end
    // the process instead of reaching anybody who could handle it.
    try {
        co_return co_await executeCommandAsyncImpl(command, /*chain=*/{}, /*nested=*/false);
    }
    catch (const vine::async::TaskCancelledException&) {
        co_return CommandResult(CommandStatus::Cancelled, String(u8"命令已取消"));
    }
    catch (const std::exception& e) {
        logErrorNoThrow("Command execution failed", e.what());
        co_return failureFromException(e);
    }
    catch (...) {
        logErrorNoThrow("Command execution failed with a non-standard exception");
        co_return CommandResult(CommandStatus::Failed, String(u8"command execution failed"));
    }
}

std::unique_ptr<Command> CommandManager::createCommandByName(const String& name)
{
    std::function<Command*()> factory;
    {
        std::lock_guard<std::mutex> lock(d->registry_mutex);
        const auto it = d->registry.find(d->resolveName(name));
        if (it == d->registry.end() || !it->second.factory) {
            return nullptr;
        }
        factory = it->second.factory;
    }

    // Called outside the lock: a factory is user code and may register commands
    // itself, which would deadlock the non-recursive registry lock.
    return std::unique_ptr<Command>(factory());
}

vine::async::Task<CommandResult> CommandManager::executeCommandAsyncImpl(Command* command, std::shared_ptr<Chain> chain, bool nested)
{
    if (!command) {
        co_return CommandResult(CommandStatus::Failed, String(u8"Command is null"));
    }

    const CommandFlags flags     = command->flags();
    const bool         exclusive = hasFlag(flags, CommandFlags::Exclusive);

    if (exclusive && !nested && !co_await d->takeOverForeground(*command)) {
        co_return CommandResult(CommandStatus::Failed, String(u8"Another operation is still stopping"));
    }

    if (!d->admit(flags, exclusive, nested, chain)) {
        // Visible for the callers that cannot read the result: a detached command
        // that is refused would otherwise fail silently.
        logWarnNoThrow("Command refused by the serialization gate", toUtf8View(command->name()));
        co_return CommandResult(CommandStatus::Failed, String(u8"Another operation is in progress"));
    }

    // Whether this run took the serialization gate (a top-level LongRunning
    // command); the guard below releases it when the run ends.
    const bool holds_gate = !nested && hasFlag(flags, CommandFlags::LongRunning);

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

        ~ChainGuard()
        {
            if (holds_gate) {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->foreground_busy = false;
            }
            chain->leaveChain();
        }
    } chain_guard{ d.get(), chain.get(), holds_gate };

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

    logInfoNoThrow("Executing command", toUtf8View(command->name()));

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

    Context context(this, d->app, chain);

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
            logErrorNoThrow("Snapshot handler failed; command not executed", e.what());
            co_return failureFromException(e);
        }
        catch (...) {
            logErrorNoThrow("Snapshot handler failed; command not executed");
            co_return CommandResult(CommandStatus::Failed, String(u8"snapshot handler failed"));
        }
    }

    // Event handlers are user code, and nothing above a command (a UI handler, a
    // detached task) is required to catch: a throwing listener must not be able
    // to abort the command or tear down the process.
    try {
        CommandExecutingEventArgs args(command);
        executing.trigger(*this, args);
    }
    catch (const std::exception& e) {
        logErrorNoThrow("Executing event handler threw", e.what());
    }
    catch (...) {
        logErrorNoThrow("Executing event handler threw");
    }

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
            throw;
        }
        result = CommandResult(CommandStatus::Cancelled, String(u8"命令已取消"));
    }
    catch (const std::exception& e) {
        // Anything else is reported as a Failed outcome instead of propagating:
        // commands are started from UI event handlers and from detached tasks
        // where an escaping exception terminates the process.
        logErrorNoThrow("Command threw an exception", toUtf8View(command->name()), e.what());
        result = failureFromException(e);
    }
    catch (...) {
        logErrorNoThrow("Command threw a non-standard exception", toUtf8View(command->name()));
        result = CommandResult(CommandStatus::Failed, String(u8"command threw an exception"));
    }

    d->report(*this, *command, result);

    co_return result;
}

vine::async::Task<CommandResult> CommandManager::executeCommandAsync(const String& name)
{
    try {
        std::unique_ptr<Command> command = createCommandByName(name);
        if (!command) {
            co_return CommandResult(CommandStatus::Failed, String(u8"Command not registered"));
        }
        co_return co_await executeCommandAsyncImpl(command.get(), /*chain=*/{}, /*nested=*/false);
    }
    catch (const vine::async::TaskCancelledException&) {
        co_return CommandResult(CommandStatus::Cancelled, String(u8"命令已取消"));
    }
    catch (const std::exception& e) {
        logErrorNoThrow("Command execution failed", e.what());
        co_return failureFromException(e);
    }
    catch (...) {
        logErrorNoThrow("Command execution failed with a non-standard exception");
        co_return CommandResult(CommandStatus::Failed, String(u8"command execution failed"));
    }
}

void CommandManager::executeDetached(const String& name)
{
    // Backstop for the fire-and-forget path: DetachedTask terminates the process
    // on an uncaught exception, and this wrapper has no caller to report to.
    // Anything thrown before the execution path takes over (a throwing factory,
    // for instance) is swallowed here with a log.
    [](vine::async::Task<CommandResult> task) -> vine::async::DetachedTask {
        try {
            (void)co_await std::move(task);
        }
        catch (const std::exception& e) {
            logErrorNoThrow("Detached command failed", e.what());
        }
        catch (...) {
            logErrorNoThrow("Detached command failed with a non-standard exception");
        }
    }(executeCommandAsync(name));
}

raw_ptr<Command> CommandManager::currentCommand() const
{
    std::shared_ptr<Chain> foreground;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        foreground = d->foreground;
    }
    return foreground ? foreground->innermost() : nullptr;
}

int CommandManager::runningCount() const
{
    std::shared_ptr<Chain> foreground;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        foreground = d->foreground;
    }
    return foreground ? foreground->stackSize() : 0;
}

void CommandManager::cancelCurrent()
{
    std::shared_ptr<Chain> foreground;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        foreground = d->foreground;
    }
    if (foreground) {
        foreground->stop_source.request_stop();
    }
}

void CommandManager::cancelAll()
{
    // Copy the chains out under the lock and stop them outside it: a chain that
    // finishes concurrently resets its weak reference, and stop_source is safe
    // to use from any thread anyway.
    std::vector<std::shared_ptr<Chain>> chains;
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->pruneChains();
        chains.reserve(d->live_chains.size());
        for (const auto& weak : d->live_chains) {
            if (auto chain = weak.lock()) {
                chains.push_back(std::move(chain));
            }
        }
    }

    for (const auto& chain : chains) {
        chain->stop_source.request_stop();
    }
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
        logWarnNoThrow("Ignoring command registration with an empty name or an empty factory");
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
            logWarnNoThrow("Command factory returned no instance; listing metadata stays empty", toUtf8View(name));
        }
    }
    catch (const std::exception& e) {
        logWarnNoThrow("Command factory threw during the metadata probe", toUtf8View(name), e.what());
    }
    catch (...) {
        logWarnNoThrow("Command factory threw during the metadata probe", toUtf8View(name));
    }

    std::lock_guard<std::mutex> lock(d->registry_mutex);
    RegisteredCommand           entry{ command_class, std::move(factory), d->registration_owner, std::move(group), std::move(description) };
    return d->registry.emplace(std::move(name), std::move(entry)).second;
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
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    return d->registry.erase(name) > 0;
}

bool CommandManager::unregisterCommand(TypeId command_class)
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    bool removed = false;
    for (auto it = d->registry.begin(); it != d->registry.end();) {
        if (it->second.class_type == command_class) {
            it      = d->registry.erase(it);
            removed = true;
        }
        else {
            ++it;
        }
    }
    return removed;
}

bool CommandManager::registerAlias(const String& alias, const String& target)
{
    if (alias.empty() || target.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(d->registry_mutex);
    const bool                  inserted = d->aliases.emplace(alias, target).second;
    if (!inserted) {
        logWarnNoThrow("Alias already registered, ignoring target", toUtf8View(alias), toUtf8View(target));
    }
    return inserted;
}

bool CommandManager::unregisterAlias(const String& alias)
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    return d->aliases.erase(alias) > 0;
}

bool CommandManager::isRegistered(const String& name) const
{
    std::lock_guard<std::mutex> lock(d->registry_mutex);
    return d->registry.find(d->resolveName(name)) != d->registry.end();
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
