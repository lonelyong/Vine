#pragma once

#include "appfw_global.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <vine/Events.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/appfw/Command.hpp>

V_APPFW_NS_BEGIN

class Application;

/**
 * @brief Event arguments fired when a command starts executing.
 *
 * Carries the command that is about to run.
 */
class V_APPFW_API CommandExecutingEventArgs : public EventArgs {
    V_OBJECT_META_DECL

  public:
    explicit CommandExecutingEventArgs(Command* command);

  public:
    /// The command that is about to execute.
    raw_ptr<Command> command() const;

  private:
    Command* command_;
};

/**
 * @brief Event arguments fired when a command finishes executing.
 *
 * Carries the command that ran and its execution result. The notification is
 * synchronous, so Handlers may inspect the command while they run; the
 * originating command is destroyed as soon as the outer execution returns, so
 * a handler must not retain the pointer.
 */
class V_APPFW_API CommandExecutedEventArgs : public EventArgs {
    V_OBJECT_META_DECL

  public:
    explicit CommandExecutedEventArgs(Command* command, const CommandResult& result);

  public:
    /// The command that finished executing; valid only during the notification.
    raw_ptr<Command> command() const;

    /// The execution result.
    const CommandResult& result() const;

  private:
    Command* command_;
    CommandResult result_;
};

/**
 * @brief Metadata of one registered command used for listing.
 */
struct CommandInfo {
    /// Canonical command name.
    String name;

    /// Group the command belongs to (may be empty).
    String group;

    /// Short human-readable description (may be empty).
    String description;

    /// Aliases resolving to this command.
    std::vector<String> aliases;

    /// Name of the plugin that registered this command; empty for host commands.
    String owner;

    /// false while the user disabled the command: it stays registered and listed,
    /// but it cannot be executed until it is enabled again.
    bool enabled = true;
};

/**
 * @brief One past execution, as recorded by the execution history.
 *
 * The history stores what ran by value, never a pointer: a command instance is
 * destroyed as soon as its execution returns, so a retained pointer would
 * dangle. Metadata of the command type itself is available through
 * CommandInfo / commandInfos().
 */
struct CommandHistoryEntry {
    /// Name reported by the command that ran.
    String name;

    /// Meta class of the command that ran.
    TypeId command_class = nullptr;

    /// Outcome of the execution.
    CommandResult result{ CommandStatus::Success };
};

/**
 * @brief Single entry point for running and managing Commands.
 *
 * The CommandManager owns the command execution chains (each chain carries its
 * own call stack and cancellation source), routes nested command execution,
 * enforces exclusive rules, records the execution history of the commands that
 * ran, and registers commands so they can be started by name.
 * Undo/Redo is snapshot-based and owned by the document layer; the manager only
 * notifies the document to snapshot its state before an Undoable command runs.
 * Commands must not start other commands as top-level entries; they run children
 * through CommandExecutionContext::executeChild().
 */
class V_APPFW_API CommandManager
{
  public:
    /**
     * @brief Returns the bounded wait for a taken-over chain to unwind before an Exclusive command starts.
     *
     * This is the bound of the drain wait: an Exclusive command whose
     * predecessor does not stop within it is rejected instead of being run next
     * to it.
     *
     * @return The drain bound.
     */
    static constexpr std::chrono::milliseconds exclusiveDrainTimeout() noexcept { return std::chrono::milliseconds{ 2000 }; }

    /**
     * @brief Returns the upper bound of the execution history.
     *
     * The oldest entry is dropped once the history holds this many executions.
     *
     * @return The history capacity.
     */
    static constexpr std::size_t maxHistoryEntries() noexcept { return 1024; }

    /**
     * @brief Returns the upper bound of the nesting depth of one chain.
     *
     * CommandExecutionContext::executeChild() refuses to nest deeper.
     *
     * @return The maximum number of commands of a chain.
     */
    static constexpr int maxChainDepth() noexcept { return 64; }

  public:
    explicit CommandManager(Application* app);
    ~CommandManager();

  public:
    /**
     * @brief Fired when a command begins executing (before its business logic runs).
     *
     * Fired once per run, nested children included. A command refused by the
     * serialization gate, or stopped by a failing snapshot handler, never gets
     * that far, so neither this event nor the executed one is fired for it.
     */
    Event<CommandManager, CommandExecutingEventArgs> executing;

    /**
     * @brief Fired when a command finishes executing, carrying its result.
     *
     * Fired for every run that started, cancelled and failed ones included. The
     * notification is synchronous and runs on the thread the command ended on
     * (the initiating thread when the command never suspended), so a handler must
     * not assume the application thread.
     */
    Event<CommandManager, CommandExecutedEventArgs> executed;

  public:
    /**
     * @brief Returns the application this manager belongs to.
     *
     * @return The hosting Application.
     */
    raw_ptr<Application> application() const noexcept;

    /**
     * @brief Executes a command, blocking until it finishes.
     *
     * Top-level entry point: the command runs on a chain of its own, which
     * becomes the foreground chain, so it is subject to the serialization gate
     * and owns the foreground while it runs. To run a child as part of the
     * current execution use CommandExecutionContext::executeChild(): that is
     * what shares the chain, the cancellation source and the nesting bound, and
     * what bypasses the gate. Calling this from inside a command starts an
     * independent top-level chain instead.
     *
     * Blocks the calling thread for the whole execution (see syncWait, which also
     * documents when that deadlocks); inside a coroutine prefer co_awaiting
     * executeCommandAsync(), which never blocks the thread the command runs on.
     *
     * Exclusive commands first cancel the running command chain; when that
     * chain refuses to unwind within exclusiveDrainTimeout() the takeover is
     * rejected with a CommandStatus::Failed result instead of running next to
     * it, because two chains running at once would break the exclusivity the
     * caller asked for. Undoable commands notify the document snapshot handler
     * before executing.
     *
     * @param command Command to execute; must not be null.
     * @return The execution outcome; Failed when the command's name is registered
     *         and disabled, so this entry point cannot bypass setCommandEnabled().
     */
    CommandResult executeCommand(Command* command);

    /**
     * @brief Starts a registered command by name, blocking until it finishes.
     *
     * Creates a fresh instance through the registered factory and runs it as a
     * top-level command, exactly like executeCommand(Command*): own chain,
     * subject to the serialization gate, caller blocked.
     *
     * @param name Registered command name.
     * @return The execution outcome; Failed when the name is not registered.
     */
    CommandResult executeCommand(const String& name);

    /**
     * @brief Executes a command asynchronously.
     *
     * The returned task is lazy: the command starts when the task is awaited (or
     * driven by syncWait), and abandoning the task means it never runs. It is a
     * top-level entry point, so the command gets a chain of its own, becomes the
     * foreground chain and is subject to the serialization gate; a nested child
     * goes through CommandExecutionContext::executeChild() instead.
     *
     * @param command Command to execute; must not be null.
     * @return A task yielding the execution outcome; Failed when the command's name
     *         is registered and disabled, so this entry point cannot bypass
     *         setCommandEnabled().
     */
    vine::async::Task<CommandResult> executeCommandAsync(Command* command);

    /**
     * @brief Executes a registered command by name asynchronously.
     *
     * Same as executeCommandAsync(Command*) with the instance created through
     * the registered factory.
     *
     * @param name Registered command name.
     * @return A lazy task yielding the execution outcome; Failed when not
     *         registered.
     */
    vine::async::Task<CommandResult> executeCommandAsync(const String& name);

    /**
     * @brief Executes a registered command by name in the background.
     *
     * Fire-and-forget: a top-level entry point, so the command gets its own chain
     * and is subject to the serialization gate. A command that runs delivers its
     * outcome through the executed event. This entry point exists for UI triggers
     * (ribbon buttons and actions), which nobody waits for, so a refusal - a name
     * that is not registered, or one the user disabled - and a failure are reported
     * through Application::userIO() as well as logged; a throwing command must not
     * terminate the process. Commands entered in the console go through
     * executeCommandAsync() instead and report themselves, so this never doubles up.
     *
     * @param name Registered command name.
     */
    void executeDetached(const String& name);

    /**
     * @brief Returns the command at the top of the foreground execution chain.
     *
     * The foreground chain is the one started by the most recent top-level
     * execution; commands started in the background with executeDetached()
     * replace it in the same way. Commands that run concurrently each own a
     * chain, so this reports the chain the user is currently interacting with.
     * The returned pointer is only valid while the command runs.
     *
     * A command that awaits a top-level entry of its own (any entry point except
     * CommandExecutionContext::executeChild()) hands the foreground over to that
     * chain, which is not given back when it ends: this can therefore report
     * nothing while commands are still running.
     *
     * @return The innermost running command of the foreground chain, or nullptr
     *         when nothing is running.
     */
    raw_ptr<Command> currentCommand() const;

    /**
     * @brief Returns the number of nested commands running in the foreground chain.
     *
     * Nested children started through CommandExecutionContext::executeChild()
     * count as well; independent chains do not.
     *
     * @return Foreground chain depth.
     */
    int runningCount() const;

    /**
     * @brief Requests cancellation of the foreground command chain.
     *
     * Cancels the chain currentCommand() reports members of: the running command
     * and its nested children. Commands on other chains are unaffected — a chain
     * that executeDetached() started and a later top-level execution pushed out of
     * the foreground, for instance. Cooperative: the running command observes the
     * request through CommandExecutionContext::stopToken()/isCancelled().
     * Cancellable async operations throw TaskCancelledException. A nested command
     * rethrows the exception so cancellation propagates upward by default; the
     * outermost command reports it as a CommandStatus::Cancelled result. A command
     * that wants to react differently to a cancelled child catches the exception in
     * its own execute(). No-op when the foreground chain has already ended, even
     * when other chains are still running: use cancelAll() for those.
     */
    void cancelCurrent();

    /**
     * @brief Requests cancellation of every live command chain.
     *
     * Unlike cancelCurrent(), this also reaches chains that were pushed out of
     * the foreground by a later top-level execution — typically commands started
     * with executeDetached(). Use it for "stop everything" paths such as
     * application shutdown. Like every other request here it is cooperative:
     * a command that ignores its token keeps running.
     */
    void cancelAll();

    /**
     * @brief Returns the number of executions recorded in the history.
     *
     * The history retains at most maxHistoryEntries() executions; the oldest
     * entries are dropped once the bound is reached.
     *
     * @return History size.
     */
    int historyCount() const;

    /**
     * @brief Returns the execution recorded at the given history index.
     *
     * Index 0 is the oldest execution still retained. The entry is a value
     * snapshot (name, meta class and result), safe to keep after the manager or
     * the command is gone.
     *
     * @param index History index.
     * @return The recorded execution, or std::nullopt when out of range.
     */
    std::optional<CommandHistoryEntry> historyAt(int index) const;

    /**
     * @brief Clears the execution history.
     */
    void clearHistory();

    /**
     * @brief Registers a command so it can be started by name.
     *
     * The factory is invoked once here to cache the listing metadata reported
     * by commandInfos(); a factory that returns nullptr or throws only logs a
     * warning: the command stays registered with empty metadata, so a plugin
     * whose services are not ready yet can still register. Failures raised by
     * the factory never propagate; an allocation failure while inserting the
     * entry still can.
     *
     * @param command_class Meta class of the command.
     * @param name Unique name used to start the command; must not be empty.
     * @param factory Factory creating a new command instance; must not be empty.
     * @return true if registered, false when the name is empty, the factory is
     *         empty or the name is already taken.
     */
    bool registerCommand(TypeId command_class, String name, std::function<Command*()> factory);

    /**
     * @brief Registers a default-constructible command type by name.
     *
     * The command type must have a no-arg constructor and Object meta
     * (V_OBJECT_META_IMPL); both are checked at compile time.
     *
     * @tparam T Command type.
     * @param name Unique name used to start the command.
     * @return true if registered, false when the name is empty or already taken
     *         (the factory lambda is never empty here).
     */
    template <typename T>
    bool registerCommand(String name)
    {
        static_assert(std::is_base_of_v<Command, T>, "T must derive from Command");
        static_assert(std::is_default_constructible_v<T>,
                      "registerCommand<T> builds instances through the default constructor; use the factory overload otherwise");
        return registerCommand(T::desc(), std::move(name), [] { return new T; });
    }

    /**
     * @brief Unregisters a command by name.
     *
     * @param name Command name.
     * @return true if removed, false if not found.
     */
    bool unregisterCommand(const String& name);

    /**
     * @brief Unregisters all commands of the given meta class.
     *
     * @param command_class Meta class of the commands to remove.
     * @return true if at least one command was removed.
     */
    bool unregisterCommand(TypeId command_class);

    /**
     * @brief Disables or enables a command without unregistering it.
     *
     * Disabling is a flag, not a removal: the command stays registered and listed
     * (CommandInfo::enabled reports the state), it only stops being executable, and
     * enabling it restores execution right away.
     * The choice is also written to the host configuration (disabledConfigKey()), so
     * a command registered again later - plugins register theirs on every load -
     * comes back already disabled. Without a host config manager the preference
     * only lives for this process.
     *
     * @param name    Command name.
     * @param enabled true to enable the command, false to disable it.
     * @return true when the preference was recorded, false when the name is empty.
     *         A name that is not registered right now still records the preference;
     *         it is applied when the command registers again.
     */
    bool setCommandEnabled(const String& name, bool enabled);

    /**
     * @brief Returns whether a command is registered and enabled.
     *
     * @param name Command or alias name.
     * @return true only for a name that resolves to a registered, enabled command.
     */
    bool isCommandEnabled(const String& name) const;

    /**
     * @brief Returns the command names the user disabled.
     *
     * This is the persisted preference, so it may also contain names that are not
     * registered right now.
     *
     * @return Disabled command names, in the order they were disabled.
     */
    std::vector<String> disabledCommands() const;

    /**
     * @brief Returns the configuration key holding the disabled command names.
     *
     * The value is a string array, mirroring PluginManager::disabledConfigKey().
     *
     * @return Key to use with the host ConfigManager.
     */
    static const String& disabledConfigKey();

    /**
     * @brief Registers an alias that resolves to an existing command name.
     *
     * Executing the alias by name runs the target command. The target does
     * not need to be registered when the alias is added, and it may itself be
     * an alias: names are resolved to the registered command in one pass at use
     * time, and a cycle (a → b → a) simply resolves to nothing.
     *
     * @param alias Alias name.
     * @param target Canonical command name the alias resolves to.
     * @return true if the alias was added, false if the alias is already taken.
     */
    bool registerAlias(const String& alias, const String& target);

    /**
     * @brief Unregisters an alias.
     *
     * @param alias Alias name.
     * @return true if removed, false if not found.
     */
    bool unregisterAlias(const String& alias);

    /**
     * @brief Returns whether a name can be executed.
     *
     * True for a registered, enabled command name and for an alias that resolves
     * (possibly through other aliases) to one. A disabled command is still listed by
     * names() and commandInfos(), but it cannot be executed.
     *
     * @param name Command or alias name.
     * @return true if executing the name runs a registered, enabled command.
     */
    bool isRegistered(const String& name) const;

    /**
     * @brief Returns the names of all registered commands.
     *
     * @return Registered command names.
     */
    std::vector<String> names() const;

    /**
     * @brief Returns the registered aliases as (alias, target) pairs.
     *
     * These are the raw registrations, so a target may itself be an alias;
     * commandInfos() reports every alias under the command it finally resolves to.
     *
     * @return Alias entries; each pair maps an alias name to the name it points at.
     */
    std::vector<std::pair<String, String>> aliases() const;

    /**
     * @brief Returns metadata of all registered commands, sorted by name.
     *
     * Each entry carries the canonical name, its group, its description, the
     * plugin that registered it and the aliases resolving (through any other
     * aliases) to it. The metadata was cached at registration, so listing does
     * not instantiate commands.
     *
     * @return Command metadata ordered by command name.
     */
    std::vector<CommandInfo> commandInfos() const;

    /**
     * @brief Returns metadata of the commands registered by one plugin.
     *
     * @param owner Plugin name; empty selects host-app commands.
     * @return The matching command metadata, ordered by command name.
     */
    std::vector<CommandInfo> commandInfosForPlugin(const String& owner) const;

    /**
     * @brief Sets the owner tag applied to subsequently registered commands.
     *
     * The PluginManager sets this to the plugin name while the plugin registers
     * its commands (vinePluginRegisterCommands and the load lifecycle), so the
     * commands it registers are attributed to it. Reset to an empty string once
     * the plugin has finished loading; commands registered outside a plugin load
     * are attributed to the host application.
     *
     * @param owner Plugin name, or empty for host commands.
     */
    void setRegistrationOwner(String owner);

    /**
     * @brief Returns the current registration owner tag.
     *
     * @return A copy of the owner set by setRegistrationOwner(); returning by
     *         value keeps the read safe against a concurrent plugin unload.
     */
    String registrationOwner() const;

    /**
     * @brief Sets the handler invoked before an Undoable command executes.
     *
     * The document registers this handler to snapshot its state so the command can
     * be undone later. Undo/Redo themselves are owned by the document layer, not by
     * the CommandManager. A handler that throws stops the command: it does not run,
     * no executed event is fired for it and nothing is recorded in the history
     * (running it without a snapshot would make the later undo restore a state the
     * document never had). The handler is called outside every lock and may be
     * replaced at any time, including while commands are running.
     *
     * @param handler Snapshot callback; empty disables the notification.
     */
    void setSnapshotHandler(std::function<void()> handler);

  private:
    class Context;

    /// One command chain: its running commands and the source that cancels them.
    struct Chain;

    struct Impl;

    /// Shared execution path.
    ///
    /// When nested is false a fresh chain is created and becomes the foreground
    /// chain; when nested is true the command joins the chain passed by the
    /// context, which also skips the serialization gate.
    vine::async::Task<CommandResult> executeCommandAsyncImpl(Command* command, std::shared_ptr<Chain> chain, bool nested);

    /// Creates a fresh registered command instance by name (or alias), or
    /// nullptr when the name is not registered or is disabled. Propagates whatever
    /// the factory throws; callers on the noexcept paths convert that to a Failed
    /// result.
    ///
    /// @param name     Command or alias name.
    /// @param disabled When not null, receives whether the name was refused for
    ///                 being disabled rather than for being unknown.
    [[nodiscard]] std::unique_ptr<Command> createCommandByName(const String& name, bool* disabled = nullptr);

    std::unique_ptr<Impl> d;
};

V_APPFW_NS_END
