#pragma once

#include <string>

namespace handler {

/**
 * @brief Reads a whole file as bytes.
 *
 * @details
 * - No transcoding: a report is UTF-8 by construction, and reading it binary keeps every byte the crashed process wrote.
 *
 * @param path File to read.
 * @return The file's bytes, or an empty string when it cannot be opened.
 */
std::string readFile(const std::wstring& path);

/**
 * @brief Replaces a file with @p text, creating it if needed.
 *
 * @param path File to write.
 * @param text Bytes to write.
 * @return true when the whole text was written.
 */
bool writeFile(const std::wstring& path, const std::string& text);

/**
 * @brief Converts UTF-8 to UTF-16.
 *
 * @param text UTF-8 text.
 * @return The UTF-16 text; empty for empty input.
 */
std::wstring toWide(const std::string& text);

/**
 * @brief Converts UTF-16 to UTF-8.
 *
 * @param text UTF-16 text.
 * @return The UTF-8 text; empty for empty input.
 */
std::string toUtf8(const std::wstring& text);

/**
 * @brief Replaces the extension of @p path.
 *
 * @param path Source path, whose last component may or may not have an extension.
 * @param extension Replacement extension including its dot, e.g. `L".txt"`.
 * @return The path with its extension replaced.
 */
std::wstring replaceExtension(const std::wstring& path, const wchar_t* extension);

/**
 * @brief Whether @p path names an existing file.
 *
 * @param path Path to test.
 * @return true when a regular file is there.
 */
bool fileExists(const std::wstring& path);

} // namespace handler
