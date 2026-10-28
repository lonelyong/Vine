#pragma once
#include "appfw_global.hpp"

#include <filesystem>
#include <optional>
#include <vector>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/async/Task.hpp>
#include <vine/math/Point3.hpp>
#include <vine/raw_ptr.hpp>

VN_APPFW_NS_BEGIN

class CommandManager;

namespace detail
{

/**
 * @brief What a file read accepts.
 *
 * The options are a requirement, not advice a caller applies afterwards: an implementation checks them itself, because
 * only it knows how to ask again - a console repeats the prompt, a headless run reports the reason and gives up.
 */
struct VN_APPFW_API FileReadOptions {
    /// Accepted extensions, compared case-insensitively against the end of the file name: "stl", ".stl" and "*.stl"
    /// all mean the same thing, and a multi-part one ("tar.gz") works too. Empty = no restriction.
    ///
    /// This is the RULE, not a caption: every implementation enforces it (a headless run has no picker at all and
    /// still refuses a wrong suffix), so it stays a structured list instead of a display string. A visual picker
    /// builds its Qt filter from it and captions the group itself.
    std::vector<String> extensions;
    /// Whether the path has to be there already. The negation is the "save as" case - `require_existing = false`
    /// reads as "an existing file is not required", where a name like `must_exist = false` could be misread as "the
    /// path must NOT exist".
    bool require_existing{ true };
    /// Path to start from: a visual implementation pre-fills its input line with it, a headless one only mentions it.
    /// A path (like what these reads return), not pre-formatted text - it is shown, not parsed.
    std::filesystem::path suggested_path;
};

/**
 * @brief What a directory read accepts.
 *
 * A directory has no extension, so the extension list has no counterpart here - which is why this is a type of its own
 * instead of a kind flag on FileReadOptions.
 */
struct VN_APPFW_API DirReadOptions {
    /// Whether the directory has to be there already; false is the "create it there" case.
    bool require_existing{ true };
    /// Path to start from, with the same meaning as FileReadOptions::suggested_path.
    std::filesystem::path suggested_path;
};

} // namespace detail

class VN_APPFW_API UserIO : public Object {
    VN_OBJECT_META_DECL;

  public:
    /**
     * @brief Paths a user handed over in one answer.
     *
     * A read that accepts several paths answers with one of these. The text is split on ';' (a character a Windows file name
     * cannot contain) and every entry has to pass on its own, so the list is never a partial success.
     */
    using PathList = std::vector<std::filesystem::path>;

    /**
     * @brief What a file read accepts; the type of the `options` parameter of the file reads below.
     *
     * Named here rather than free in the namespace: these are the parameter types of this class' reads and of nothing
     * else, which is how the rest of this framework spells a class' own options (MeshLoader::Options, InputBox::Field),
     * and it keeps a name as generic as "extensions" out of `vn::appfw`.
     *
     * Defined in the detail namespace instead of lexically nested, because the `options = {}` defaults below need the
     * type's default member initializers at class scope, and Clang and GCC both refuse to use those of a class nested
     * in the class that declares the default argument (only MSVC accepts the nested form). The alias keeps the
     * `UserIO::FileReadOptions` spelling and the aggregate layout; see detail::FileReadOptions for the fields.
     */
    using FileReadOptions = detail::FileReadOptions;

    /**
     * @brief What a directory read accepts; the type of the `options` parameter of the directory reads below.
     *
     * A sibling of FileReadOptions, defined in detail for the same reason; see detail::DirReadOptions for the fields.
     */
    using DirReadOptions = detail::DirReadOptions;

  public:
    /**
     * @brief Constructs the base; an implementation adds the interaction state it needs.
     *
     * The base itself holds only the command-manager binding, which starts empty (see setCommandManager).
     */
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
    virtual vn::async::Task<std::optional<String>> getStringAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests an integer from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the entered value, or std::nullopt if cancelled or
     *         if the text is not an integer in the range of int.
     */
    virtual vn::async::Task<std::optional<int>> getIntAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests a double from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the entered value, or std::nullopt if cancelled.
     */
    virtual vn::async::Task<std::optional<double>> getDoubleAsync(const String& prompt = {}) = 0;

    /**
     * @brief Asynchronously requests a 3D point from the user.
     *
     * @param prompt Prompt text shown to the user.
     * @return A task yielding the picked point, or std::nullopt if cancelled.
     */
    virtual vn::async::Task<std::optional<math::Point3d>> getPoint3dAsync(const String& prompt = {}) = 0;

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
     * @brief Asynchronously requests one file from the user.
     *
     * @param prompt  Prompt text shown to the user; the implementation may add the accepted formats to it.
     * @param options What to accept: extensions, existence, and the path to start from.
     * @return A task yielding the path, or std::nullopt if cancelled, if the input did not fit the options, or if the
     *         implementation has no way to ask (see the note below).
     *
     * @note Like every read, only one may wait for the user at a time, and the entry points may be called from any
     *       thread. An implementation that cannot offer a picker or a line input gives std::nullopt and logs a warning
     *       instead of pretending the path was read; both implementations in this framework offer it.
     *
     * @note Declared after every other virtual, like cancelPendingInput(): this group only ever grows at the end, so a
     *       plugin compiled against an older header keeps the slots it already used.
     */
    virtual vn::async::Task<std::optional<std::filesystem::path>> getFileAsync(const String& prompt = {}, const FileReadOptions& options = {});

    /**
     * @brief Asynchronously requests one or more files from the user.
     *
     * @param prompt  Prompt text shown to the user.
     * @param options What to accept; applied to every entry.
     * @return A task yielding at least one path, or std::nullopt if cancelled or if any entry was refused.
     *
     * @note Several paths are typed in one line, separated by ';' - see PathList for why that character.
     */
    virtual vn::async::Task<std::optional<PathList>> getFilesAsync(const String& prompt = {}, const FileReadOptions& options = {});

    /**
     * @brief Asynchronously requests one directory from the user.
     *
     * @param prompt  Prompt text shown to the user.
     * @param options What to accept: existence and the path to start from.
     * @return A task yielding the path, or std::nullopt if cancelled or if the input did not fit the options.
     */
    virtual vn::async::Task<std::optional<std::filesystem::path>> getDirAsync(const String& prompt = {}, const DirReadOptions& options = {});

    /**
     * @brief Asynchronously requests one or more directories from the user.
     *
     * @param prompt  Prompt text shown to the user.
     * @param options What to accept; applied to every entry.
     * @return A task yielding at least one path, or std::nullopt if cancelled or if any entry was refused.
     *
     * @note Several directories are typed in one line, separated by ';'.
     */
    virtual vn::async::Task<std::optional<PathList>> getDirsAsync(const String& prompt = {}, const DirReadOptions& options = {});

    /**
     * @brief Returns the bound command manager.
     *
     * @return The command manager, or nullptr if unbound.
     */
    raw_ptr<CommandManager> commandManager() const;

  protected:
    /**
     * @brief Outcome of checking one text as a path.
     *
     * @note The message is the whole interaction contract on the headless side (it is the only thing the user sees) and
     *       the re-prompt text on the visual side, so both backends report the same words for the same refusal.
     */
    struct PathCheck {
        bool                  ok{ false }; // True when the text is a path the options accept.
        std::filesystem::path path;        // The path, valid only when ok.
        String                message;     // Why it was refused; empty when ok.
    };

    /// Outcome of checking one text that may hold several paths (one per ';').
    struct PathLineCheck {
        bool     ok{ false }; // True when every entry passed and at least one was there.
        PathList paths;       // The paths, valid only when ok.
        String   message;     // Why it was refused; empty when ok.
    };

    /**
     * @brief Checks one text as a single path.
     *
     * Leading and trailing whitespace is ignored, and so is one pair of surrounding quotes, because a path copied out of a
     * file manager usually carries them ("C:\a b.stl"). Nothing else is rewritten: a relative path stays relative and is
     * resolved by the caller's working directory, like every other loader in this repository.
     *
     * @param text    Text to check.
     * @param options What the read accepts; extensions are ignored here.
     * @return The path and an empty message, or a reason the user is shown.
     */
    static PathCheck checkPath(const String& text, const FileReadOptions& options);

    /**
     * @brief Checks one text as a single directory.
     *
     * @param text    Text to check.
     * @param options What the read accepts.
     * @return The path and an empty message, or a reason the user is shown.
     */
    static PathCheck checkPath(const String& text, const DirReadOptions& options);

    /**
     * @brief Checks one text as one or more paths separated by ';'.
     *
     * Empty entries are skipped, so a trailing or doubled ';' is harmless; the result keeps the order the user typed and
     * is not de-duplicated. The first entry that fails ends the check - the caller is shown one reason, not all of them.
     *
     * @param text    Text to check.
     * @param options What the read accepts; the extensions apply to every entry.
     * @return The paths in order and an empty message, or a reason the user is shown.
     */
    static PathLineCheck checkPathLine(const String& text, const FileReadOptions& options);

    /**
     * @brief Checks one text as one or more directories separated by ';'.
     *
     * @param text    Text to check.
     * @param options What the read accepts.
     * @return The paths in order and an empty message, or a reason the user is shown.
     */
    static PathLineCheck checkPathLine(const String& text, const DirReadOptions& options);

    /**
     * @brief Turns the accepted extensions into Qt-style patterns.
     *
     * Exists so that the pattern spelling ("*.stl") is normalized in exactly one place: a visual picker hands these to its
     * filter, and checkPath() matches with the same normalization.
     *
     * @param options Options whose extensions are converted.
     * @return One pattern per extension ("stl" and "*.stl" both become "*.stl"), in the order given.
     */
    static std::vector<String> filterPatterns(const FileReadOptions& options);

    /**
     * @brief Summarizes what a read accepts, for the prompt text.
     *
     * @param extensions Accepted extensions, if any.
     * @param multiple   Whether several paths may be given in one answer.
     * @return Text to place in the prompt - "（.stl .obj，多个用 ; 分隔）" - or an empty string when there is nothing to
     *         say.
     */
    static String requirementSummary(const std::vector<String>& extensions, bool multiple);

    /**
     * @brief Weaves a requirement summary into a prompt text.
     *
     * The summary goes before a trailing separator when the prompt has one ("网格文件: " ⇒ "网格文件（.stl）: "), because
     * that is where a reader expects the qualification, and is appended otherwise.
     *
     * @param prompt  Prompt text as the caller wrote it.
     * @param summary Requirement summary from requirementSummary(), possibly empty.
     * @return The prompt to show; the input is returned unchanged when there is no summary to add.
     */
    static String promptWithRequirements(const String& prompt, const String& summary);

    /**
     * @brief Parses text as an integer that has to fit int.
     *
     * String::toInt() used to cast the saturated value, so text beyond the range of int wrapped
     * around (which is how a value typed into the former int8_t read came out negative); it
     * range-checks now, so this helper exists for one difference only: it leaves value untouched
     * when parsing fails, which the re-prompt paths rely on to keep what the user already had.
     *
     * @param text  Text to parse; leading whitespace is skipped and trailing characters after
     *              the number are accepted, exactly like toInt().
     * @param value Receives the parsed value on success, and is left untouched on failure.
     * @return true when text starts with an integer in the range of int.
     */
    static bool parseInt(const String& text, int& value);

  private:
    /**
     * @brief The one check both options types go through.
     *
     * @param text      Text to check.
     * @param directory Whether a directory is wanted (a file read expects the opposite).
     * @param options   What the read accepts; extensions are ignored for a directory.
     * @return The path and an empty message, or a reason the user is shown.
     */
    static PathCheck checkPath(const String& text, bool directory, const FileReadOptions& options);

    CommandManager* command_manager_{ nullptr };
};

using UserIOPtr = raw_ptr<UserIO>;

VN_APPFW_NS_END
