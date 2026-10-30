#include "CrashWindow.hpp"

#include <cstring>
#include <vector>

// clang-format off
#ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
// clang-format on

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
// The themed controls (v6) are what makes the dialog look like the operating system's own and not like Windows 95. The
// dialog is the whole UI of this executable, so the dependency is worth its one line - and it needs no .rc file.
#pragma comment(linker,                                                                                                        \
                "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "               \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "Text.hpp"

namespace handler {
namespace {

constexpr int kCopyReport{ 101 };
constexpr int kOpenFolder{ 102 };
constexpr int kSendMail{ 103 };

/**
 * @brief Copies text to the clipboard as UTF-16.
 *
 * @param text Text to place on the clipboard.
 * @return true when the clipboard accepted it.
 */
bool copyToClipboard(const std::wstring& text)
{
    if (OpenClipboard(nullptr) == FALSE) {
        return false;
    }
    EmptyClipboard();

    const std::size_t bytes  = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL           memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool              copied = false;
    if (memory != nullptr) {
        if (void* target = GlobalLock(memory); target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

/**
 * @brief Shows @p file in its own folder, selecting it when it still exists.
 *
 * @param file File to reveal; when it is gone, its folder is opened instead.
 */
void revealFile(const std::wstring& file)
{
    if (fileExists(file)) {
        const std::wstring arguments = L"/select,\"" + file + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }

    const std::size_t slash = file.find_last_of(L"\\/");
    const std::wstring folder = slash == std::wstring::npos ? std::wstring{} : file.substr(0, slash);
    if (!folder.empty()) {
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

/**
 * @brief Percent-encodes text for a `mailto:` query value.
 *
 * @param text UTF-8 text.
 * @return The encoded text; every reserved character becomes `%XX`, so newlines survive as `%0D%0A`.
 */
std::string percentEncode(const std::string& text)
{
    static const char* kDigits = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(text.size());
    for (const unsigned char byte : text) {
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')
                           || byte == '-' || byte == '_' || byte == '.' || byte == '~';
        if (plain) {
            encoded += static_cast<char>(byte);
        }
        else {
            encoded += '%';
            encoded += kDigits[byte >> 4];
            encoded += kDigits[byte & 0x0F];
        }
    }
    return encoded;
}

/**
 * @brief Starts a mail draft with the recipient, subject and body pre-filled.
 *
 * @warning
 * - `mailto:` cannot carry an attachment, so the body tells the user to attach the dump themselves. Anything more would be
 *   a promise this action cannot keep.
 *
 * @param mailto Recipient address.
 * @param app_name Application name, used in the subject.
 * @param report_path Path of the report, quoted in the body.
 * @param dump_path Path of the dump, quoted in the body; may be empty.
 */
void startMailDraft(const std::wstring& mailto,
                    const std::wstring& app_name,
                    const std::wstring& report_path,
                    const std::wstring& dump_path)
{
    std::wstring body = app_name + L" 崩溃报告位于：\r\n" + report_path + L"\r\n";
    if (!dump_path.empty()) {
        body += L"\r\n转储文件（请手动作为附件添加）：\r\n" + dump_path + L"\r\n";
    }
    body += L"\r\n（报告正文见附件旁的 .txt 文件；本邮件由崩溃窗口预填。）";

    const std::string uri = "mailto:" + toUtf8(mailto) + "?subject=" + percentEncode(toUtf8(app_name + L" 崩溃报告"))
                            + "&body=" + percentEncode(toUtf8(body));
    ShellExecuteW(nullptr, L"open", toWide(uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/**
 * @brief The fallback for a machine without the themed dialog: paths only, no actions.
 *
 * @param title Window title.
 * @param content Message text.
 */
void showMessageBox(const std::wstring& title, const std::wstring& content)
{
    MessageBoxW(nullptr, content.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

void showWindow(const std::wstring& app_name,
                const std::wstring& report_path,
                const std::wstring& dump_path,
                const std::string&  report_text,
                const std::wstring& mailto)
{
    const std::wstring title       = app_name + L" 已崩溃";
    const std::wstring instruction = app_name + L" 遇到了问题，已经退出。";
    const std::wstring expanded    = toWide(report_text);
    const bool         can_mail    = !mailto.empty();

    std::wstring content = L"报告：\r\n" + report_path + L"\r\n\r\n转储：\r\n"
                           + (dump_path.empty() ? L"（未生成）" : dump_path)
                           + L"\r\n\r\n请把这两个文件交给开发者；转储是定位问题的关键。";

    // The buttons and their labels must outlive the dialog call, so they are built once per showWindow.
    constexpr int                   kButtonIds[]{ kCopyReport, kOpenFolder, kSendMail };
    std::vector<std::wstring>       labels{ L"复制报告", L"打开文件夹" };
    std::vector<TASKDIALOG_BUTTON>  buttons;
    if (can_mail) {
        labels.emplace_back(L"发送邮件");
    }
    buttons.reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        buttons.push_back(TASKDIALOG_BUTTON{ kButtonIds[i], labels[i].c_str() });
    }

    const std::wstring clipboard_text = expanded + L"\r\ndump: " + (dump_path.empty() ? std::wstring{ L"(none)" } : dump_path);

    bool copied = false;
    for (;;) {
        std::wstring shown = content;
        if (copied) {
            shown = L"报告已复制到剪贴板，可以直接粘贴。\r\n\r\n" + shown;
        }

        TASKDIALOGCONFIG configuration{};
        configuration.cbSize             = sizeof configuration;
        configuration.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        configuration.dwCommonButtons    = TDCBF_CLOSE_BUTTON;
        configuration.pszWindowTitle     = title.c_str();
        configuration.pszMainIcon        = TD_ERROR_ICON;
        configuration.pszMainInstruction = instruction.c_str();
        configuration.pszContent         = shown.c_str();
        configuration.pszExpandedInformation   = expanded.c_str();
        configuration.pszExpandedControlText   = L"详细信息";
        configuration.pszCollapsedControlText  = L"详细信息";
        configuration.pButtons           = buttons.data();
        configuration.cButtons           = static_cast<UINT>(buttons.size());

        int     pressed = 0;
        HRESULT result  = TaskDialogIndirect(&configuration, &pressed, nullptr, nullptr);
        if (FAILED(result)) {
            showMessageBox(title, shown);
            return;
        }

        if (pressed == kCopyReport) {
            copied = copyToClipboard(clipboard_text);
            continue;
        }
        if (pressed == kOpenFolder) {
            revealFile(dump_path.empty() ? report_path : dump_path);
            continue;
        }
        if (pressed == kSendMail) {
            startMailDraft(mailto, app_name, report_path, dump_path);
            continue;
        }
        return;
    }
}

} // namespace handler
