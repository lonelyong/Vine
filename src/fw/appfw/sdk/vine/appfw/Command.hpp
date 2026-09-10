#pragma once

#include "appfw_global.hpp"

#include <any>
#include <cstdint>
#include <stop_token>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/async/Task.hpp>

V_APPFW_NS_BEGIN

class Application;
class CommandManager;
class Command;

/**
 * @brief Execution characteristics of a Command.
 *
 * Flags describe how the CommandManager treats a command at run time.
 * Version 1 defines three flags: Undoable for commands that modify business
 * data, Exclusive for commands that take over the foreground, and LongRunning
 * for commands that may take a long time.
 */
enum class CommandFlags : std::uint32_t
{
    /**
     * @brief Default: a plain one-shot command.
     */
    None = 0,

    /**
     * @brief Modifies business data; participates in Undo/Redo.
     *
     * The CommandManager asks the document to snapshot before such a command
     * runs, through CommandManager::setSnapshotHandler().
     */
    Undoable = 1 << 0,

    /**
     * @brief Takes over the foreground, cancelling the running command chain.
     *
     * A top-level Exclusive command requests cancellation of the foreground
     * chain and waits, bounded by CommandManager::exclusiveDrainTimeout(), for it
     * to unwind. Only a chain that actually unwound is taken over: a command that
     * ignores its token makes the takeover impossible, and the request is then
     * refused with a Failed result instead of running two chains at once. The
     * cancelled chain kept its own call stack and cancellation source throughout,
     * so it always unwinds cleanly. An Exclusive command also bypasses the
     * serialization gate that rejects other top-level commands while a LongRunning
     * operation is busy.
     */
    Exclusive = 1 << 1,

    /**
     * @brief May run for a long time; reports progress and serializes input.
     *
     * A LongRunning top-level command creates an ambient ProgressHost so the
     * command can report progress through ProgressScope without wiring any
     * UI. While it runs, the application is considered busy: other top-level
     * commands are rejected with a "another operation is in progress" result
     * and the UI may show a progress bar with a cancel button.
     */
    LongRunning = 1 << 2
};

/**
 * @brief Outcome of executing a Command.
 */
enum class CommandStatus : std::uint8_t
{
    /**
     * @brief The command completed successfully.
     */
    Success = 0,

    /**
     * @brief The command failed to complete.
     */
    Failed,

    /**
     * @brief The command was cancelled before completing.
     */
    Cancelled,
};

/**
 * @brief Result returned by Command::execute().
 *
 * Carries the execution status, an optional message describing the outcome
 * (usually set on failure), and optional business data produced by the
 * command. succeeded() reports whether the command completed successfully.
 */
class V_APPFW_API CommandResult
{
  public:
    CommandResult() = default;

    /**
     * @brief Constructs a result with a status and optional message.
     *
     * @param status Execution outcome.
     * @param message Optional description, usually a failure reason.
     */
    explicit CommandResult(CommandStatus status, String message = {});

  public:
    /**
     * @brief Returns the execution outcome.
     *
     * @return The status.
     */
    CommandStatus status() const;

    /**
     * @brief Returns whether the command completed successfully.
     *
     * @return true if the status is Success.
     */
    bool succeeded() const;

    /**
     * @brief Returns the optional message describing the outcome.
     *
     * @return The message.
     */
    const String& message() const;

    /**
     * @brief Returns the business data produced by the command.
     *
     * Empty when the command produced no data. Retrieve the typed value with
     * std::any_cast<T>.
     *
     * @return The data.
     */
    const std::any& data() const;

    /**
     * @brief Sets the business data produced by the command.
     *
     * @param data The data.
     */
    void setData(std::any data);

  private:
    CommandStatus status_ = CommandStatus::Success;
    String message_;
    std::any data_;
};

/**
 * @brief Execution context handed to Command::execute().
 *
 * Created by the CommandManager for each execution and passed to the command.
 * The command reads application resources (services, config, ...) through
 * application(). The CommandManager provides a private implementation; user
 * code never constructs a context directly.
 */
class V_APPFW_API CommandExecutionContext
{
  public:
    virtual ~CommandExecutionContext() = default;

    /**
     * @brief Returns the application executing the command.
     *
     * @return The hosting Application.
     */
    virtual Application* application() const = 0;

    /**
     * @brief Returns the cancellation token for this execution.
     *
     * Pass it to cancellable async operations (e.g. vine::async::sleep with a
     * token); they throw TaskCancelledException once the execution is cancelled.
     *
     * @return The execution's cancellation token.
     */
    virtual std::stop_token stopToken() const = 0;

    /**
     * @brief Whether a cancellation has been requested for this execution.
     *
     * Commands doing non-cooperative work should poll this between steps.
     *
     * @return true when the execution should stop.
     */
    virtual bool isCancelled() const = 0;

    /**
     * @brief Starts a registered child command by name as part of this
     *        execution.
     *
     * The child is created through the command registry exactly like
     * CommandManager::executeCommand(name), joins this execution's chain
     * (sharing its cancellation source, so cancelCurrent() stops parent and
     * child together), and bypasses the serialization gate so it may run
     * inside a LongRunning parent. Every LongRunning command owns its own
     * ProgressHost, so a child takes over the progress bar for its own duration
     * and the parent's host is current again afterwards.
     * Use this — not
     * application()->commandManager()->executeCommand() — to start a nested
     * command: the manager cannot tell a nested call from a new top-level one
     * (both enter the same entry point and commands may hop threads), so
     * nesting must be marked explicitly through the execution context, and a
     * top-level call from inside a command would take the foreground away from it.
     *
     * @param name Registered child command name.
     * @return The child's execution outcome; Failed when the name is not
     *         registered, when the factory fails, or when the chain is already
     *         maxChainDepth() commands deep.
     */
    virtual vine::async::Task<CommandResult> executeChild(const String& name) = 0;
};

/**
 * @brief Base class of all user operations.
 *
 * A Command describes one user operation, runs the concrete business logic in
 * execute(), and accesses application services through the execution context.
 * It does not store long-lived application state and does not manage UI or
 * document lifecycles.
 *
 * Commands run through the CommandManager. A command may start nested commands
 * via context->executeChild(), which joins the calling execution: it shares its
 * chain and cancellation source (every LongRunning command still owns its own
 * ProgressHost) and is bounded by CommandManager::maxChainDepth().
 */
class V_APPFW_API Command : public Object
{
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(Command);

  public:
    /**
     * @brief Default-constructs a command.
     *
     * Required so commands can be created through the registered factory
     * (registerCommand<T> instantiates T with new T).
     */
    Command() = default;

  public:
    /**
     * @brief Unique command name used as identifier and for menus.
     *
     * @return The command name.
     */
    virtual String name() const = 0;

    /**
     * @brief Group the command belongs to (menu/toolbar grouping).
     *
     * May be empty when the command has no group.
     *
     * @return The group name.
     */
    virtual String group() const = 0;

    /**
     * @brief Short human-readable description of what the command does.
     *
     * @return The description, empty when not provided.
     */
    virtual String description() const { return {}; }

    /**
     * @brief Execution characteristics of this command.
     *
     * @return The command flags.
     */
    virtual CommandFlags flags() const = 0;

    /**
     * @brief Runs the command business logic.
     *
     * The command may suspend on asynchronous operations (for example user
     * input or an asynchronous delay) by co_awaiting them.
     *
     * @param context Execution context providing application access and
     *                nested command execution.
     * @return A task yielding the execution outcome.
     */
    virtual vine::async::Task<CommandResult> execute(CommandExecutionContext* context) = 0;
};

V_APPFW_NS_END
