#include <vine/brepio/BrepFormat.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <string>

#include "BrepFormatTable.hpp"

VN_BREPIO_NS_BEGIN

namespace
{

/**
 * @brief One row of the format table: the format, its diagnostic name and the extensions that select it.
 *
 * A STEP file is named .stp or .step and an IGES file .igs or .iges, so a row carries both spellings of its own
 * extension; formatFromPath() matches any of them and supportedExtensions() lists them all.
 */
struct Entry
{
    BrepFormat  format;
    const char* name;
    const char* extensions[2];
};

constexpr Entry kEntries[] = {
    { BrepFormat::Step, "step", { ".stp", ".step" } },
    { BrepFormat::Iges, "iges", { ".igs", ".iges" } },
    { BrepFormat::Brep, "brep", { ".brep", nullptr } },
};

/**
 * @brief Finds the row of a format.
 *
 * @param format Format to look up.
 * @return The row, or null for Unknown and out-of-range values.
 */
const Entry* findEntry(BrepFormat format) noexcept
{
    for (const Entry& entry : kEntries) {
        if (entry.format == format) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

namespace detail
{

std::span<const BrepFormat> allFormats() noexcept
{
    static const std::array<BrepFormat, std::size(kEntries)> formats = [] {
        std::array<BrepFormat, std::size(kEntries)> values{};
        for (std::size_t i = 0; i < std::size(kEntries); ++i) {
            values[i] = kEntries[i].format;
        }
        return values;
    }();
    return formats;
}

std::span<const char* const> extensionsOf(BrepFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    if (entry == nullptr) {
        return {};
    }
    return { entry->extensions, entry->extensions[1] != nullptr ? std::size_t{ 2 } : std::size_t{ 1 } };
}

} // namespace detail

const char* formatName(BrepFormat format) noexcept
{
    const Entry* const entry = findEntry(format);
    return entry != nullptr ? entry->name : "Unknown";
}

BrepFormat formatFromPath(const std::filesystem::path& file_path) noexcept
{
    try {
        std::string extension = file_path.extension().string();
        for (char& c : extension) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        for (const Entry& entry : kEntries) {
            for (const char* candidate : entry.extensions) {
                if (candidate != nullptr && extension == candidate) {
                    return entry.format;
                }
            }
        }
    } catch (...) {
        return BrepFormat::Unknown;
    }
    return BrepFormat::Unknown;
}

VN_BREPIO_NS_END
