#include "Text.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace handler {
namespace {

#if defined(_WIN32)

/// @brief Converts a UTF-8 path to the UTF-16 spelling the Win32 file APIs take.
std::wstring toWidePath(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), wide.data(), length);
    return wide;
}

#else

/// @brief Converts a UTF-16 path to the UTF-8 spelling the POSIX file APIs take.
std::string toNarrowPath(const std::wstring& path)
{
    return path.empty() ? std::string{} : std::filesystem::path{ path }.string();
}

#endif

} // namespace

#if defined(_WIN32)

// Windows: the UTF-16 overloads are the implementation, because that is the path type the platform's APIs take. The
// UTF-8 overloads convert once and delegate, so a caller that has UTF-8 (a command line built in the crash path) still
// reaches the same code.

std::string readFile(const std::wstring& path)
{
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return {};
    }

    std::string text;
    char        chunk[8192];
    std::size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof chunk, file)) > 0) {
        text.append(chunk, read);
    }
    std::fclose(file);
    return text;
}

std::string readFile(const std::string& path)
{
    return readFile(toWidePath(path));
}

bool writeFile(const std::wstring& path, const std::string& text)
{
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return false;
    }
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
    return written == text.size();
}

bool writeFile(const std::string& path, const std::string& text)
{
    return writeFile(toWidePath(path), text);
}

std::wstring replaceExtension(const std::wstring& path, const wchar_t* extension)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::size_t dot   = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return path + extension;
    }
    return path.substr(0, dot) + extension;
}

std::string replaceExtension(const std::string& path, const char* extension)
{
    // The narrow overload keeps the header's contract (UTF-8 in, UTF-8 out); only the work happens in UTF-16.
    return toUtf8(replaceExtension(toWidePath(path), toWidePath(extension).c_str()));
}

bool fileExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool fileExists(const std::string& path)
{
    return fileExists(toWidePath(path));
}

std::wstring toWide(const std::string& text)
{
    return toWidePath(text);
}

std::string toUtf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

#else

// POSIX: the UTF-8 overloads are the implementation, because that is what `argv`, `/proc/self/maps` and the report's own
// paths are. The UTF-16 overloads convert once and delegate, so the two spellings stay one behaviour.

std::string readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string{ std::istreambuf_iterator<char>{ file }, std::istreambuf_iterator<char>{} };
}

std::string readFile(const std::wstring& path)
{
    return readFile(toNarrowPath(path));
}

bool writeFile(const std::string& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

bool writeFile(const std::wstring& path, const std::string& text)
{
    return writeFile(toNarrowPath(path), text);
}

std::string replaceExtension(const std::string& path, const char* extension)
{
    std::filesystem::path result{ path };
    result.replace_extension(extension);
    return result.string();
}

std::wstring replaceExtension(const std::wstring& path, const wchar_t* extension)
{
    std::filesystem::path result{ path };
    result.replace_extension(std::filesystem::path{ extension });
    return result.wstring();
}

bool fileExists(const std::string& path)
{
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool fileExists(const std::wstring& path)
{
    return fileExists(toNarrowPath(path));
}

#endif

} // namespace handler
