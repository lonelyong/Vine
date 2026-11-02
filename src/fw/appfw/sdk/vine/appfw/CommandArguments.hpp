#pragma once

#include "appfw_global.hpp"

#include <cstddef>
#include <span>
#include <vector>

#include <vine/String.hpp>

VN_APPFW_NS_BEGIN

/**
 * @brief Splits one command line into the tokens a CommandArguments carries.
 *
 * @details
 * - Whitespace separates tokens; double quotes group; backslashes follow the Windows C runtime
 *   rules, so a line and the process argv split identically.
 * - It is LEXICAL only: `--`, `-l10` and `--name=value` carry no meaning here.
 *   The token reaches the command unchanged and the command decides what it means.
 * - An empty quoted token (`""`) is a token; an unterminated quote takes the rest of the line,
 *   which is what the C runtime does too.
 * - Every text source (console, startup arguments, macro replay, tests) calls this one
 *   implementation, so there is exactly one dialect.
 *
 * @param line Text to split.
 * @return The tokens, in input order; empty when the line holds no token.
 */
VN_APPFW_API std::vector<String> tokenizeCommandLine(const String& line);

/**
 * @brief The arguments of one execution: the tokens typed after the command name.
 *
 * @details
 * - The framework only carries the tokens: how they are interpreted (positions, `--name`, a
 *   private grammar) is the command's business. Splitting the line into tokens is the one piece
 *   the framework owns, because quoting must not be re-implemented by every command.
 * - The manager copies the arguments into the execution frame when it dispatches, so they are
 *   immutable afterwards and may be read from whatever thread the command resumes on.
 * - An execution without arguments holds an empty instance, never a null one: commands read
 *   tokens() without guarding for absence.
 * - toString() is the canonical record used by events, the history and macro replay; it is the
 *   exact inverse of tokenizeCommandLine(), so a recorded invocation can be replayed verbatim.
 */
class VN_APPFW_API CommandArguments {
  public:
    CommandArguments() = default;

    /**
     * @brief Builds the arguments from already-split tokens.
     *
     * @param tokens Tokens in input order; taken over by the instance.
     */
    explicit CommandArguments(std::vector<String> tokens);

  public:
    /**
     * @brief Returns the tokens in input order.
     *
     * @return A view over the tokens; empty when the execution has no arguments.
     */
    std::span<const String> tokens() const noexcept;

    /**
     * @brief Returns the number of tokens.
     *
     * @return Token count.
     */
    std::size_t size() const noexcept;

    /**
     * @brief Returns whether the execution has no arguments.
     *
     * @return true when no token was given.
     */
    bool empty() const noexcept;

    /**
     * @brief Returns the token at an index.
     *
     * @param index Token index; must be smaller than size().
     * @return The token.
     */
    const String& operator[](std::size_t index) const;

    /**
     * @brief Returns the canonical text of these arguments.
     *
     * @details
     * - Tokens are separated by single spaces; a token that is empty or holds whitespace or a
     *   double quote is quoted with backslash escapes.
     * - tokenizeCommandLine(toString()) reproduces tokens() exactly, which is what makes the
     *   history and the events replayable.
     *
     * @return The text to record and to hand back to tokenizeCommandLine().
     */
    String toString() const;

  private:
    std::vector<String> tokens_;
};

VN_APPFW_NS_END
