#include <vine/appfw/UserIO.hpp>

#include <system_error>

#include <vine/logging/Log.hpp>

VN_APPFW_NS_BEGIN

namespace
{

/// "stl" / ".stl" / "*.stl" ⇒ "stl": one spelling to match with, and one to build patterns from.
String normalizeExtension(const String& text)
{
    String value = text.trimmed().toLower();
    while (value.startsWith(u8'*')) {
        value = value.substr(1);
    }
    while (value.startsWith(u8'.')) {
        value = value.substr(1);
    }
    return value;
}

/// The accepted extensions as ".stl .obj" (whitespace separated, empties dropped); empty when there are none.
String extensionBody(const std::vector<String>& extensions)
{
    String body;
    for (const String& extension : extensions) {
        const String normalized = normalizeExtension(extension);
        if (normalized.empty()) {
            continue;
        }
        if (!body.empty()) {
            body += u8" ";
        }
        body += String(u8".") + normalized;
    }
    return body;
}

} // namespace

VN_OBJECT_META_IMPL(UserIO, Object);

UserIO::UserIO()
{}

void UserIO::cancelPendingInput()
{
    // Nothing to do by default: only an implementation that parks the read on
    // something it can signal (an event a UI thread sets) can unblock it.
}

void UserIO::setCommandManager(CommandManager* manager)
{
    command_manager_ = manager;
}

void UserIO::clear()
{}

raw_ptr<CommandManager> UserIO::commandManager() const
{
    return command_manager_;
}

// 默认实现不做假承诺：这一层读不出来的实现（比如将来的远端/脚本 IO）必须自己 override，
// 否则一次"读到了路径"的假象会让调用方拿一个 std::nullopt 之外的错误。两个内置实现都 override。
vn::async::Task<std::optional<std::filesystem::path>> UserIO::getFileAsync(const String&, const FileReadOptions&)
{
    VN_LOGW("getFileAsync() is not implemented by this UserIO; returning nullopt");
    co_return std::nullopt;
}

vn::async::Task<std::optional<UserIO::PathList>> UserIO::getFilesAsync(const String&, const FileReadOptions&)
{
    VN_LOGW("getFilesAsync() is not implemented by this UserIO; returning nullopt");
    co_return std::nullopt;
}

vn::async::Task<std::optional<std::filesystem::path>> UserIO::getDirAsync(const String&, const DirReadOptions&)
{
    VN_LOGW("getDirAsync() is not implemented by this UserIO; returning nullopt");
    co_return std::nullopt;
}

vn::async::Task<std::optional<UserIO::PathList>> UserIO::getDirsAsync(const String&, const DirReadOptions&)
{
    VN_LOGW("getDirsAsync() is not implemented by this UserIO; returning nullopt");
    co_return std::nullopt;
}

UserIO::PathCheck UserIO::checkPath(const String& text, const FileReadOptions& options)
{
    return checkPath(text, /*directory=*/false, options);
}

UserIO::PathCheck UserIO::checkPath(const String& text, const DirReadOptions& options)
{
    FileReadOptions file_options;
    file_options.require_existing = options.require_existing;
    file_options.suggested_path = options.suggested_path;
    return checkPath(text, /*directory=*/true, file_options);
}

UserIO::PathCheck UserIO::checkPath(const String& text, bool directory, const FileReadOptions& options)
{
    const auto* noun = directory ? u8"目录" : u8"文件";

    // 路径从前端复制过来常带一层成对的引号；空白无所谓，不剥就是"不存在"。
    String path_text = text.trimmed();
    if (path_text.size() >= 2 && path_text.startsWith(u8'"') && path_text.endsWith(u8'"')) {
        path_text = path_text.substr(1, path_text.size() - 2).trimmed();
    }
    if (path_text.empty()) {
        return PathCheck{ false, {}, String(u8"请输入路径") };
    }

    const std::filesystem::path path(path_text.as_std_u8str());
    std::error_code             error;
    const bool                  exists = std::filesystem::exists(path, error);

    if (options.require_existing && !exists) {
        return PathCheck{ false, {}, String(noun) + u8"不存在: " + path_text };
    }

    // 存在就核对类型："另存为"给了一个已存在的目录同样算错，而尚不存在的路径只看名字（后缀那一步）。
    if (exists && directory != std::filesystem::is_directory(path, error)) {
        return PathCheck{ false, {}, path_text + u8"不是" + noun };
    }

    if (!directory && !options.extensions.empty()) {
        const String name = String(path.filename().u8string()).toLower();
        bool         matched = false;
        for (const String& extension : options.extensions) {
            const String normalized = normalizeExtension(extension);
            if (normalized.empty()) {
                continue;
            }
            // 比尾部的 ".后缀" 而不是 extension()：多段后缀（"tar.gz"）才写得出来，且 ".stl" 这样的点文件不算命中。
            if (name.size() > normalized.size() + 1 && name.endsWith(String(u8".") + normalized)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return PathCheck{ false, {}, String(u8"后缀不支持（仅 ") + extensionBody(options.extensions) + u8"）: " + path_text };
        }
    }

    return PathCheck{ true, path, {} };
}

UserIO::PathLineCheck UserIO::checkPathLine(const String& text, const FileReadOptions& options)
{
    PathLineCheck result;

    // split(…, false) 丢掉空段，于是尾随/重复的 ';' 无害；只剩空白的段同理（"a; ;b" 里的中间那段）。
    for (const String& entry : text.split(u8';', false)) {
        const String trimmed = entry.trimmed();
        if (trimmed.empty()) {
            continue;
        }

        const PathCheck one = checkPath(trimmed, options);
        if (!one.ok) {
            result.message = one.message; // 第一条不合法的就够用户改了
            return result;
        }
        result.paths.push_back(one.path);
    }

    if (result.paths.empty()) {
        result.message = String(u8"请输入路径");
        return result;
    }

    result.ok = true;
    return result;
}

UserIO::PathLineCheck UserIO::checkPathLine(const String& text, const DirReadOptions& options)
{
    FileReadOptions file_options;
    file_options.require_existing = options.require_existing;
    file_options.suggested_path = options.suggested_path;
    return checkPathLine(text, file_options);
}

std::vector<String> UserIO::filterPatterns(const FileReadOptions& options)
{
    std::vector<String> patterns;
    patterns.reserve(options.extensions.size());
    for (const String& extension : options.extensions) {
        const String normalized = normalizeExtension(extension);
        if (normalized.empty()) {
            continue;
        }
        patterns.push_back(String(u8"*.") + normalized);
    }
    return patterns;
}

String UserIO::requirementSummary(const std::vector<String>& extensions, bool multiple)
{
    String body = extensionBody(extensions);
    if (multiple) {
        if (!body.empty()) {
            body += u8"，";
        }
        body += u8"多个用 ; 分隔";
    }
    if (body.empty()) {
        return {};
    }
    return String(u8"（") + body + u8"）";
}

String UserIO::promptWithRequirements(const String& prompt, const String& summary)
{
    if (prompt.empty() || summary.empty()) {
        return prompt;
    }

    // 提示末尾若是分隔符（"网格文件: " / "输入名称> "），摘要在它**前面** —— 读起来才是
    // "网格文件（.stl .obj）: " 而不是 "网格文件: （.stl .obj）"。
    for (const char8_t* tail : { u8": ", u8"：", u8":", u8"> ", u8">" }) {
        const String separator(tail);
        if (prompt.endsWith(separator)) {
            return prompt.substr(0, prompt.size() - separator.size()) + summary + separator;
        }
    }
    return prompt + String(u8" ") + summary;
}

bool UserIO::parseInt(const String& text, int& value)
{
    // toInt() is the single implementation of the "has to fit int" rule (it reports an
    // out-of-range text as a failure instead of wrapping it); the extra contract this helper
    // adds is that value stays untouched when parsing fails.
    bool      ok     = false;
    const int parsed = text.toInt(&ok);
    if (!ok) {
        return false;
    }

    value = parsed;
    return true;
}

VN_APPFW_NS_END
