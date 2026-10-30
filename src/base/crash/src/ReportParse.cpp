#include <vine/crash/CrashHandler.hpp>

#include <cstring>

VN_CRASH_NS_BEGIN
namespace {

/// @brief Copies @p text into a fixed field, always terminated and never splitting a UTF-8 sequence.
void copyField(char* out, std::size_t capacity, std::string_view text) noexcept
{
    if (capacity == 0) {
        return;
    }
    std::size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    while (count > 0 && count < text.size() && (static_cast<unsigned char>(text[count]) & 0xC0) == 0x80) {
        --count;
    }
    if (count > 0) {
        std::memcpy(out, text.data(), count);
    }
    out[count] = '\0';
}

/// @brief Removes the separator spaces a key/value line leaves in front of its value.
std::string_view skipSpaces(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

/// @brief Splits off the next space-separated token of @p rest and advances it past that token.
std::string_view takeToken(std::string_view& rest) noexcept
{
    rest                = skipSpaces(rest);
    const std::size_t end = rest.find(' ');
    const std::string_view token = rest.substr(0, end);
    rest.remove_prefix(token.size());
    return token;
}

/// @brief Parses a hexadecimal number, with or without the `0x` prefix.
bool parseHex(std::string_view text, std::uint64_t& value) noexcept
{
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (const char c : text) {
        parsed <<= 4;
        if (c >= '0' && c <= '9') {
            parsed |= static_cast<std::uint64_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f') {
            parsed |= static_cast<std::uint64_t>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F') {
            parsed |= static_cast<std::uint64_t>(c - 'A' + 10);
        }
        else {
            return false;
        }
    }
    value = parsed;
    return true;
}

/// @brief Parses a decimal number, including the leading `-` the image index uses for "no image".
bool parseDecimal(std::string_view text, long long& value) noexcept
{
    if (text.empty()) {
        return false;
    }
    const bool negative = text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    long long parsed = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        parsed = parsed * 10 + (c - '0');
    }
    value = negative ? -parsed : parsed;
    return true;
}

/// @brief Whether @p line is the magic line of a report version this reader understands.
bool hasKnownHeader(std::string_view line) noexcept
{
    std::string_view rest    = line;
    const auto       magic   = takeToken(rest);
    const auto       version = takeToken(rest);
    long long        parsed  = 0;
    return magic == Report::kMagic && parseDecimal(version, parsed) && parsed == Report::kVersion;
}

} // namespace

bool parseReport(std::string_view text, Report& out) noexcept
{
    out = Report{};

    std::size_t position = 0;
    const auto  nextLine = [&text, &position](std::string_view& line) noexcept {
        if (position >= text.size()) {
            return false;
        }
        std::size_t end = text.find('\n', position);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        line = text.substr(position, end - position);
        position = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        return true;
    };

    std::string_view line;
    if (!nextLine(line) || !hasKnownHeader(line)) {
        return false;
    }

    while (nextLine(line)) {
        std::string_view       rest = line;
        const std::string_view key  = takeToken(rest);
        if (key.empty()) {
            continue;
        }

        if (key == "app") {
            copyField(out.app, sizeof out.app, skipSpaces(rest));
        }
        else if (key == "time") {
            copyField(out.time, sizeof out.time, skipSpaces(rest));
        }
        else if (key == "dump") {
            copyField(out.dump, sizeof out.dump, skipSpaces(rest));
        }
        else if (key == "context") {
            if (out.context_count < Report::kMaxContext) {
                copyField(out.context[out.context_count], Report::kContextChars, skipSpaces(rest));
                ++out.context_count;
            }
        }
        else if (key == "exception") {
            const auto    code    = takeToken(rest);
            takeToken(rest); // "at"
            const auto    address = takeToken(rest);
            takeToken(rest); // "thread"
            const auto    id      = takeToken(rest);
            std::uint64_t parsed  = 0;
            if (parseHex(code, parsed)) {
                out.code = static_cast<unsigned long>(parsed);
            }
            if (parseHex(address, parsed)) {
                out.address = static_cast<std::uintptr_t>(parsed);
            }
            long long number = 0;
            if (parseDecimal(id, number)) {
                out.thread_id = static_cast<unsigned long>(number);
            }
        }
        else if (key == "image") {
            const auto base = takeToken(rest);
            const auto name = takeToken(rest);
            const auto path = skipSpaces(rest); // a path may contain spaces, so it is the rest of the line
            if (out.image_count < Report::kMaxImages) {
                ReportImage&  image  = out.images[out.image_count];
                std::uint64_t parsed = 0;
                if (parseHex(base, parsed)) {
                    image.base = static_cast<std::uintptr_t>(parsed);
                }
                copyField(image.name, ReportImage::kNameChars, name);
                copyField(image.path, ReportImage::kPathChars, path);
                ++out.image_count;
            }
        }
        else if (key == "frame") {
            const auto address = takeToken(rest);
            const auto image   = takeToken(rest);
            const auto offset  = takeToken(rest);
            if (out.frame_count < Report::kMaxFrames) {
                ReportFrame&  frame       = out.frames[out.frame_count];
                std::uint64_t parsed      = 0;
                long long     image_index = -1;
                if (parseHex(address, parsed)) {
                    frame.address = static_cast<std::uintptr_t>(parsed);
                }
                if (parseDecimal(image, image_index)) {
                    frame.image = static_cast<int>(image_index);
                }
                if (parseHex(offset, parsed)) {
                    frame.offset = static_cast<std::uintptr_t>(parsed);
                }
                ++out.frame_count;
            }
        }
        // An unknown key is a field a newer writer added: skipping it is what makes the tail of a report forward compatible.
    }

    return true;
}

VN_CRASH_NS_END
