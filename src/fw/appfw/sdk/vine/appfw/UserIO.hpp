#pragma once
#include "appfw_global.hpp"

#include <optional>

#include <vine/Object.hpp>
#include <vine/raw_ptr.hpp>
#include <vine/String.hpp>
#include <vine/async/Task.hpp>
#include <vine/math/Point3.hpp>

V_APPFW_NS_BEGIN

class CommandManager;

class V_APPFW_API UserIO : public Object {
    V_OBJECT_META_DECL;

  public:
    UserIO();

  public:
    /**
     * @brief Writes a line of output to the user.
     *
     * This is the output path of a running command, so it may be called from any
     * thread: a command resumes on the thread that completed whatever it awaited
     * (a timer, an IO read), not necessarily the thread that started it. An
     * implementation that owns a UI object has to marshal to its own thread;
     * ConsoleUserIO and VisualUserIO both do.
     *
     * @param str Text to append as one line.
     */
    virtual void putString(const String& str) = 0;

    /**
     * @brief Clears all previously written output.
     *
     * The base implementation does nothing; visual implementations clear their
     * console panel and headless implementations clear the terminal.
     *
     * @note Like putString(), this may be called from any thread.
     */
    virtual void clear();

    /**
     * @brief Asynchronously requests a string from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the entered string, or std::nullopt if cancelled.
     *
     * @note Only one read may wait for the user at a time: while one is pending, the
     *       other getXxxAsync() entries complete immediately with std::nullopt and
     *       log a warning. The console shows a single prompt, and two waiting reads
     *       would otherwise share one completion signal and each other's result.
     */
    virtual vine::async::Task<std::optional<String>> getStringAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests an integer from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the entered value, or std::nullopt if cancelled or
     *         if the text is not an integer in the range of int.
     */
    virtual vine::async::Task<std::optional<int>> getIntAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests a double from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the entered value, or std::nullopt if cancelled.
     */
    virtual vine::async::Task<std::optional<double>> getDoubleAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests a 3D point from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the picked point, or std::nullopt if cancelled.
     */
    virtual vine::async::Task<std::optional<math::Point3d>> getPoint3dAsync(const String& prompt = {}) = 0;

    /**
     * @brief Sets the command manager that idle input is dispatched to.
     *
     * Called once while the application starts up, before any worker thread can
     * reach the IO; an implementation that caches command metadata is expected to
     * refresh it here and whenever CommandManager::commandsChanged() fires.
     *
     * @param manager Command manager, or nullptr to unbind.
     */
    virtual void setCommandManager(CommandManager* manager);

    /**
     * @brief Cancels the read that is currently waiting for user input, if any.
     *
     * A command awaiting one of the getXxxAsync() reads stays parked until the user
     * answers; cancelling the command chain cannot reach it, because the read owns no
     * cancellation token. Hosts that are going down therefore have to unblock the
     * interaction explicitly (Application::shutdown() does, right before it drains
     * the command chains), and the pending read completes with std::nullopt.
     *
     * May be called from any thread, including one whose event loop has already
     * stopped: an implementation must make the waiting read return without touching
     * any UI it may own.
     *
     * The base implementation does nothing: an implementation whose read cannot be
     * unblocked must not pretend otherwise.
     *
     * @note Declared after every other virtual on purpose: an implementation compiled
     *       against an older header keeps the index of the slots it already used, so
     *       adding this entry only affects the new slot.
     */
    virtual void cancelPendingInput();

    /**
     * @brief Returns the bound command manager.
     *
     * @return The command manager, or nullptr if unbound.
     */
    raw_ptr<CommandManager> commandManager() const;

  protected:
    /**
     * @brief Parses text as an integer that has to fit int.
     *
     * String::toInt() is not usable for user input: it runs through strtol and casts
     * the result to int, so text beyond the range of int silently wraps (which is how
     * a value typed into the former int8_t read turned into a negative number).
     *
     * @param text  Text to parse; surrounding whitespace is ignored, and trailing
     *              characters after the number are accepted like toInt() does.
     * @param value Receives the parsed value on success.
     * @return true when text starts with an integer in the range of int.
     */
    static bool parseInt(const String& text, int& value);

  private:
    CommandManager* command_manager_{ nullptr };
};

using UserIOPtr = raw_ptr<UserIO>;

V_APPFW_NS_END
