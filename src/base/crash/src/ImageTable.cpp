#include "CrashDetail.hpp"

#include <cstring>

VN_CRASH_NS_BEGIN
namespace detail {
namespace {

/**
 * @brief Copies UTF-8 into a fixed field, always terminated and never splitting a sequence.
 *
 * @param out Destination field.
 * @param capacity Size of @p out in bytes, including the terminator.
 * @param text Text to copy, truncated when it does not fit.
 */
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

/**
 * @brief Reads a hexadecimal number at @p pos and advances past it.
 *
 * @param text Line being parsed.
 * @param pos Cursor into @p text; left on the first character that is not a hex digit.
 * @param value Receives the parsed number.
 * @return true when at least one digit was there.
 */
bool readHex(std::string_view text, std::size_t& pos, std::uintptr_t& value) noexcept
{
    std::uintptr_t parsed = 0;
    std::size_t    digits = 0;
    std::size_t    cursor = pos;
    while (cursor < text.size()) {
        const char     c     = text[cursor];
        unsigned       digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        }
        else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        }
        else {
            break;
        }
        parsed = (parsed << 4) | digit;
        ++cursor;
        ++digits;
    }

    if (digits == 0 || digits > sizeof(std::uintptr_t) * 2) {
        return false;
    }
    pos   = cursor;
    value = parsed;
    return true;
}

/// @brief Skips spaces and tabs at @p pos.
void skipSpaces(std::string_view text, std::size_t& pos) noexcept
{
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
}

/**
 * @brief Skips one space-separated field (permissions, file offset, device, inode).
 *
 * @param text Line being parsed.
 * @param pos Cursor into @p text.
 * @return false when the line ends before a field starts.
 */
bool skipField(std::string_view text, std::size_t& pos) noexcept
{
    skipSpaces(text, pos);
    const std::size_t start = pos;
    while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t') {
        ++pos;
    }
    return pos > start;
}

/// @brief The text after the last `/` of @p path, i.e. the file name.
std::string_view fileName(std::string_view path) noexcept
{
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/**
 * @brief Whether a stored path field names the same file as @p path.
 *
 * @details
 * - Compares as much as the field holds: a path longer than @ref ReportImage::kPathChars is stored truncated, and two such
 *   paths sharing the stored prefix are treated as one file. That is the price of comparing without allocating, and a
 *   wrong merge costs a base address, not a report.
 *
 * @param stored Path as stored in an image entry, terminated.
 * @param path Candidate path from the mapping line.
 * @return true when both name the same mapped file.
 */
bool samePath(const char* stored, std::string_view path) noexcept
{
    constexpr std::size_t kFieldChars{ ReportImage::kPathChars - 1 };
    const std::size_t     length   = std::strlen(stored);
    const std::size_t     compared = path.size() < kFieldChars ? path.size() : kFieldChars;
    return length == compared && std::memcmp(stored, path.data(), compared) == 0;
}

} // namespace

std::size_t resolveImageTable(std::string_view maps, Report& report) noexcept
{
    // Start and end of every image must be kept apart from ReportImage, which has a base but no size: a frame is inside an
    // image when it is in any of that file's segments, which only the ranges can answer.
    std::uintptr_t starts[Report::kMaxImages]{};
    std::uintptr_t ends[Report::kMaxImages]{};
    std::size_t    count = 0;

    report.image_count = 0;

    std::size_t pos = 0;
    while (pos < maps.size()) {
        const std::size_t newline = maps.find('\n', pos);
        if (newline == std::string_view::npos) {
            break; // Unterminated last line: the buffer ran out inside it, and half a mapping is not a mapping.
        }
        const std::string_view line = maps.substr(pos, newline - pos);
        pos                         = newline + 1;
        if (line.empty()) {
            continue;
        }

        std::size_t    cursor = 0;
        std::uintptr_t start  = 0;
        std::uintptr_t end    = 0;
        if (!readHex(line, cursor, start) || cursor >= line.size() || line[cursor] != '-') {
            continue;
        }
        ++cursor;
        if (!readHex(line, cursor, end) || end <= start) {
            continue;
        }
        // Permissions, file offset, device and inode: parsed past, not needed to name an image.
        if (!skipField(line, cursor) || !skipField(line, cursor) || !skipField(line, cursor) || !skipField(line, cursor)) {
            continue;
        }
        skipSpaces(line, cursor);

        const std::string_view path = line.substr(cursor);
        if (path.empty() || path.front() != '/') {
            continue; // Anonymous range, [heap], [stack], [vdso]: an address there belongs to no image.
        }

        std::size_t index = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (samePath(report.images[i].path, path)) {
                index = i;
                break;
            }
        }

        if (index == count) {
            if (count >= Report::kMaxImages) {
                continue;
            }
            ReportImage& image = report.images[count];
            std::memset(&image, 0, sizeof image);
            copyField(image.path, ReportImage::kPathChars, path);
            copyField(image.name, ReportImage::kNameChars, fileName(path));
            starts[count] = start;
            ends[count]   = end;
            ++count;
        }
        else {
            if (start < starts[index]) {
                starts[index] = start;
            }
            if (end > ends[index]) {
                ends[index] = end;
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        report.images[i].base = starts[i];
    }
    report.image_count = count;

    for (std::size_t i = 0; i < report.frame_count && i < Report::kMaxFrames; ++i) {
        ReportFrame& frame = report.frames[i];
        frame.image        = -1;
        frame.offset       = 0;
        for (std::size_t image = 0; image < count; ++image) {
            if (frame.address >= starts[image] && frame.address < ends[image]) {
                frame.image  = static_cast<int>(image);
                frame.offset = frame.address - starts[image];
                break;
            }
        }
    }
    return count;
}

} // namespace detail
VN_CRASH_NS_END
