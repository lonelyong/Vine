#pragma once

#include <string>

namespace handler {

// The handler touches files through whatever path spelling its platform hands it: UTF-16 on Windows (`wWinMain`'s
// arguments, `GetModuleFileNameW`), UTF-8 on POSIX (`argv`, `/proc/self/maps`). Every helper below is overloaded on both,
// so a caller never converts a path just to open it - and neither spelling is a second implementation of the same idea.

/**
 * @brief Reads a whole file as bytes.
 *
 * @details
 * - No transcoding: a report is UTF-8 by construction, and reading it binary keeps every byte the crashed process wrote.
 *
 * @param path File to read.
 * @return The file's bytes, or an empty string when it cannot be opened.
 */
std::string readFile(const std::string& path);

/// @brief @ref readFile for a UTF-16 path; @p path is UTF-8 on POSIX and the native path type on Windows.
std::string readFile(const std::wstring& path);

/**
 * @brief Replaces a file with @p text, creating it if needed.
 *
 * @param path File to write.
 * @param text Bytes to write.
 * @return true when the whole text was written.
 */
bool writeFile(const std::string& path, const std::string& text);

/// @brief @ref writeFile for a UTF-16 path; @p path is UTF-8 on POSIX and the native path type on Windows.
bool writeFile(const std::wstring& path, const std::string& text);

/**
 * @brief Replaces the extension of @p path.
 *
 * @param path Source path, whose last component may or may not have an extension.
 * @param extension Replacement extension including its dot, e.g. `".txt"`.
 * @return The path with its extension replaced.
 */
std::string replaceExtension(const std::string& path, const char* extension);

/// @brief @ref replaceExtension for a UTF-16 path.
std::wstring replaceExtension(const std::wstring& path, const wchar_t* extension);

/**
 * @brief Whether @p path names an existing file.
 *
 * @param path Path to test; empty is never a file, and a directory is not a file.
 * @return true when a regular file is there.
 */
bool fileExists(const std::string& path);

/// @brief @ref fileExists for a UTF-16 path.
bool fileExists(const std::wstring& path);

#if defined(_WIN32)
/**
 * @brief Converts UTF-8 to UTF-16.
 *
 * @note
 * - Windows only: the POSIX side takes UTF-8 from end to end and has nothing to convert. The dialog and the symbolizer are
 *   the callers, because `windows.h` and DbgHelp speak UTF-16.
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
#endif

} // namespace handler
