#pragma once

#include <vector>

#include <vine/Signal.hpp>

#include "ConsoleTheme.hpp"
#include "Control.hpp"

class QWidget;

VN_APPFWGUI_NS_BEGIN

/**
 * @brief A command candidate offered by the console completion popup.
 */
struct ConsoleCommandEntry {
    /// Command name (target of the aliases).
    String name;
    /// Short human-readable description (may be empty).
    String description;
    /// Aliases resolving to this command (may be empty).
    std::vector<String> aliases;
    /// Owning plugin name shown as "source: name" (empty = host commands).
    String source;
};

/**
 * @brief What the input line is asking for while a read waits for an answer.
 *
 * Only the shape of the offer: which dialog to open and how many answers are accepted. What a picked value has to look
 * like (extensions, existence) stays with the caller that asked for it - the panel never validates anything.
 */
enum class ConsoleInputKind
{
    None,       // A plain read: no picker.
    File,       // One file.
    Files,      // One or more files.
    Directory,  // One directory.
    Directories // One or more directories.
};

/**
 * @brief The picker the input line offers while a read waits.
 */
struct ConsoleInputAction {
    /// What to pick; None removes the offer again (which is what a finished read does).
    ConsoleInputKind kind{ ConsoleInputKind::None };
    /// Name filter handed to the dialog as it stands, e.g. "网格文件 (*.stl *.obj);;所有文件 (*)"; empty = no filter.
    /// This string is built by the caller - the panel passes it through without looking inside.
    String filter;
    /// Hover text and accessible name of the button, e.g. "选择文件…（.stl .obj，可多选）".
    String tooltip;
};

/**
 * @brief Console panel: read-only output view plus a command input line.
 *
 * Pure UI: it renders output with semantic coloring, collects a line of input,
 * offers up/down history and Tab completion, and reports Enter/Escape through
 * signals. It knows nothing about commands or business logic.
 *
 * A read that wants a path can also be answered by picking it: setInputAction() puts a small picker button inside the
 * input line, and what it picks is written into that line as text - so the picker only ever saves typing, and whatever
 * the user finally submits goes through the same signal and the same validation as a typed line.
 */
class VN_APPFW_API ConsolePanel : public Control {
    VN_OBJECT_META_DECL

  public:
    explicit ConsolePanel(QWidget* parent = nullptr);
    ~ConsolePanel() override;

  public:
    /**
     * @brief Appends a message of the given type to the output view.
     */
    void append(ConsoleMessageType type, const String& text);

    /**
     * @brief Clears all output.
     */
    void clear();

    /**
     * @brief Shows a prompt, clears the input line and focuses it.
     *
     * @param prompt       Prompt text to show in the output view.
     * @param initial_text Text to put into the input line; a re-prompt passes what the user just typed, so a rejected
     *                     answer does not have to be typed again.
     */
    void beginInput(const String& prompt, const String& initial_text = {});

    /**
     * @brief Clears the input line only.
     */
    void clearInput();

    /**
     * @brief Sets the picker the input line offers, or removes it.
     *
     * The offer is not part of the panel's own state machine: whoever waits for the answer (a UserIO implementation)
     * sets it when the read starts and clears it when the read ends, so a button can never outlive its prompt.
     *
     * @param action Picker to offer; an action with kind == None (the default) removes the button and the highlight.
     */
    void setInputAction(const ConsoleInputAction& action);

    /**
     * @brief Sets the candidate commands used by completion and the popup.
     *
     * @param entries Candidate commands.
     */
    void setCommandEntries(const std::vector<ConsoleCommandEntry>& entries);

    /**
     * @brief Sets the color scheme.
     */
    void setTheme(const ConsoleTheme& theme);

    /**
     * @brief Returns the current color scheme.
     */
    const ConsoleTheme& theme() const;

    /// Fired when the user submits a line (Enter).
    vn::Signal<const String&> lineEntered;

    /// Fired when the user presses Escape.
    vn::Signal<> escapePressed;

  private:
    /// Applies the ConsoleTheme matching the current application theme.
    void applyAppTheme();

    struct Impl;
    Impl*       dptr();
    const Impl* dptr() const;
};

VN_APPFWGUI_NS_END
