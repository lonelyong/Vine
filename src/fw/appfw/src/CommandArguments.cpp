#include <vine/appfw/CommandArguments.hpp>

#include <utility>

VN_APPFW_NS_BEGIN

namespace
{

/// Whether a character separates tokens.
///
/// Only ASCII whitespace separates: the line comes from a console or a process command line,
/// where any other separator has to be quoted anyway.
///
/// @param ch Character to test.
/// @return true when ch separates tokens outside quotes.
bool isSeparator(char8_t ch) noexcept
{
    return ch == u8' ' || ch == u8'\t' || ch == u8'\r' || ch == u8'\n';
}

/// Whether a token has to be quoted to survive tokenizeCommandLine() unchanged.
///
/// @param token Token to test.
/// @return true for an empty token or one holding whitespace or a quote.
bool needsQuoting(const String& token) noexcept
{
    if (token.empty()) {
        return true;
    }
    for (std::size_t i = 0; i < token.size(); ++i) {
        const char8_t ch = token.data()[i];
        if (isSeparator(ch) || ch == u8'"') {
            return true;
        }
    }
    return false;
}

/// Appends one token to the canonical text, quoting it when it must be.
///
/// The escaping is the inverse of tokenizeCommandLine(): a run of n backslashes before a quote
/// becomes 2n+1 backslashes plus the quote, and backslashes before the closing quote are doubled,
/// so the Windows C runtime rules recover the original token.
///
/// @param out   Text being built.
/// @param token Token to append.
void appendToken(String& out, const String& token)
{
    if (!needsQuoting(token)) {
        out.append(token);
        return;
    }

    out.push_back(u8'"');
    std::size_t backslashes = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        const char8_t ch = token.data()[i];
        if (ch == u8'\\') {
            ++backslashes;
            continue;
        }
        if (ch == u8'"') {
            out.append(backslashes * 2 + 1, u8'\\');
            out.push_back(u8'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, u8'\\');
        backslashes = 0;
        out.push_back(ch);
    }
    // Backslashes directly before the closing quote would otherwise escape it.
    out.append(backslashes * 2, u8'\\');
    out.push_back(u8'"');
}

} // namespace

std::vector<String> tokenizeCommandLine(const String& line)
{
    std::vector<String> tokens;
    String              token;
    bool                token_started = false;
    bool                in_quotes     = false;

    const char8_t*    data = line.data();
    const std::size_t size = line.size();

    for (std::size_t i = 0; i < size; ++i) {
        const char8_t ch = data[i];

        if (ch == u8'\\') {
            // A run of backslashes is an escape only directly before a quote, and then by
            // parity: 2n backslashes plus a quote mean n backslashes and a quote that toggles
            // quoting, while 2n+1 mean n backslashes and a literal quote. Anywhere else the
            // backslashes are ordinary characters, so `C:\dir\file` survives unchanged.
            std::size_t backslashes = 0;
            while (i + backslashes < size && data[i + backslashes] == u8'\\') {
                ++backslashes;
            }

            const bool followed_by_quote = i + backslashes < size && data[i + backslashes] == u8'"';
            if (!followed_by_quote) {
                token.append(backslashes, u8'\\');
                token_started = true;
                i += backslashes - 1;
                continue;
            }

            token.append(backslashes / 2, u8'\\');
            token_started = true;
            if (backslashes % 2 != 0) {
                token.push_back(u8'"');
            }
            else {
                in_quotes = !in_quotes;
            }
            // The loop's ++i steps over the quote the run escaped or toggled.
            i += backslashes;
            continue;
        }

        if (ch == u8'"') {
            in_quotes     = !in_quotes;
            token_started = true; // "" is an empty token, not the absence of one.
            continue;
        }

        if (!in_quotes && isSeparator(ch)) {
            if (token_started) {
                tokens.push_back(std::move(token));
                token         = String{};
                token_started = false;
            }
            continue;
        }

        token.push_back(ch);
        token_started = true;
    }

    if (token_started) {
        tokens.push_back(std::move(token));
    }
    return tokens;
}

CommandArguments::CommandArguments(std::vector<String> tokens)
  : tokens_(std::move(tokens))
{}

std::span<const String> CommandArguments::tokens() const noexcept
{
    return tokens_;
}

std::size_t CommandArguments::size() const noexcept
{
    return tokens_.size();
}

bool CommandArguments::empty() const noexcept
{
    return tokens_.empty();
}

const String& CommandArguments::operator[](std::size_t index) const
{
    return tokens_[index];
}

String CommandArguments::toString() const
{
    String text;
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
        if (i != 0) {
            text.push_back(u8' ');
        }
        appendToken(text, tokens_[i]);
    }
    return text;
}

VN_APPFW_NS_END
