#include <vine/imageio/ImageCodec.hpp>

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>

#include <vine/String.hpp>

VN_IMAGEIO_NS_BEGIN

namespace
{

/**
 * @brief One row of the format table: the format, its diagnostic name and the facts callers may query.
 *
 * The table is the only place a format is stated. formatName(), formatFromPath(), formatInfo(),
 * canRead() and canWrite() all read it, so a format added here is named, recognised by its extension and
 * offered to a file picker in one edit - the alternative was one switch per question, which is how a
 * format ends up readable but not writable by accident.
 */
struct Entry {
    ImageFileFormat format;
    const char*     name;
    ImageFormatInfo info;
};

// JPEG spells itself two ways; every other container has one extension. The arrays live outside the table
// so ImageFormatInfo can be a view over them rather than a fixed-capacity list.
constexpr const char* const kPngExtensions[]  = { ".png" };
constexpr const char* const kJpegExtensions[] = { ".jpg", ".jpeg" };
constexpr const char* const kBmpExtensions[]  = { ".bmp" };
constexpr const char* const kTgaExtensions[]  = { ".tga" };

constexpr Entry kEntries[] = {
    { ImageFileFormat::Png,  "PNG",  { kPngExtensions, true, true }   },
    // JPEG writing is lossy and needs a quality parameter this API deliberately has no place for, so the
    // table says "readable, not writable" instead of leaving the asymmetry to the encoder.
    { ImageFileFormat::Jpeg, "JPEG", { kJpegExtensions, true, false } },
    { ImageFileFormat::Bmp,  "BMP",  { kBmpExtensions, true, true }   },
    { ImageFileFormat::Tga,  "TGA",  { kTgaExtensions, true, true }   },
};

constexpr ImageFormatInfo kUnknownInfo{ {}, false, false };

/**
 * @brief Finds the row of a format.
 *
 * @param format Format to look up.
 * @return The row, or null for Unknown and out-of-range values.
 */
const Entry* findEntry(ImageFileFormat format) noexcept
{
    for (const Entry& entry : kEntries) {
        if (entry.format == format) {
            return &entry;
        }
    }
    return nullptr;
}

/**
 * @brief Compares ASCII text without regard to case.
 *
 * The text is UTF-8 rather than plain `char` because it comes straight from `std::filesystem::path::u8string()`,
 * the only path conversion that is lossless on Windows (where the native form is UTF-16, so `native()` cannot
 * be compared against an ASCII suffix at all).
 *
 * @param text    Text to compare.
 * @param literal Lowercase ASCII literal to compare against.
 * @return true when they are equal ignoring ASCII case.
 */
bool equalsIgnoreCaseAscii(std::u8string_view text, std::string_view literal) noexcept
{
    if (text.size() != literal.size()) {
        return false;
    }

    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c     = static_cast<unsigned char>(text[i]);
        const char          lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        if (lower != literal[i]) {
            return false;
        }
    }

    return true;
}

} // namespace

const ImageFormatInfo& formatInfo(ImageFileFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    return entry != nullptr ? entry->info : kUnknownInfo;
}

const char* formatName(ImageFileFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    return entry != nullptr ? entry->name : "Unknown";
}

ImageFileFormat formatFromPath(const std::filesystem::path& path)
{
    // Materialised rather than viewed: extension() returns a temporary path, and u8string() a temporary
    // string, so a view of either would dangle at the end of the full expression.
    const std::u8string extension = path.extension().u8string();

    if (extension.empty()) {
        return ImageFileFormat::Unknown;
    }

    for (const Entry& entry : kEntries) {
        for (const char* const candidate : entry.info.extensions) {
            if (equalsIgnoreCaseAscii(extension, candidate)) {
                return entry.format;
            }
        }
    }

    return ImageFileFormat::Unknown;
}

bool canRead(ImageFileFormat format) noexcept
{
    return formatInfo(format).can_read;
}

bool canWrite(ImageFileFormat format) noexcept
{
    return formatInfo(format).can_write;
}

const std::set<vn::String>& supportedExtensions()
{
    // Generated from the format table: the set a file picker filters on and the extensions the decoder
    // relies on are one list, so a format added to the table shows up here without a second edit.
    static const std::set<vn::String> extensions = [] {
        std::set<vn::String> parsed;
        for (const Entry& entry : kEntries) {
            if (!entry.info.can_read) {
                continue;
            }
            for (const char* const extension : entry.info.extensions) {
                parsed.insert(vn::String::fromUtf8(extension));
            }
        }
        return parsed;
    }();
    return extensions;
}

VN_IMAGEIO_NS_END
