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
 * @brief Console panel: read-only output view plus a command input line.
 *
 * Pure UI: it renders output with semantic coloring, collects a line of input,
 * offers up/down history and Tab completion, and reports Enter/Escape through
 * signals. It knows nothing about commands or business logic.
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
     */
    void beginInput(const String& prompt);

    /**
     * @brief Clears the input line only.
     */
    void clearInput();

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
