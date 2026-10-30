#include "CrashWindowX11.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace handler {
namespace {

constexpr int kPadding{ 16 };
constexpr int kButtonHeight{ 30 };
constexpr int kButtonGap{ 8 };
constexpr int kScrollBarWidth{ 8 };
constexpr int kWindowWidth{ 900 };
constexpr int kWindowHeight{ 640 };

/// @brief Whether the byte at @p index starts a UTF-8 sequence rather than continuing one.
bool isSequenceStart(char byte) noexcept
{
    return (static_cast<unsigned char>(byte) & 0xC0) != 0x80;
}

/**
 * @brief Whether @p program resolves to an executable on `PATH`.
 *
 * @details
 * - The action has to know *before* forking: a child that fails in `exec` exits silently, and a button that does nothing
 *   is exactly the failure this window can least afford.
 *
 * @param program Bare program name, no directory.
 * @return true when some `PATH` entry holds it and it is executable.
 */
bool isOnPath(const char* program) noexcept
{
    const char* path = std::getenv("PATH");
    if (path == nullptr || program == nullptr || program[0] == '\0') {
        return false;
    }

    const std::string_view entries{ path };
    std::size_t            begin = 0;
    while (begin <= entries.size()) {
        const std::size_t colon     = entries.find(':', begin);
        const std::size_t end       = colon == std::string_view::npos ? entries.size() : colon;
        const std::string directory = std::string{ entries.substr(begin, end - begin) };
        if (!directory.empty() && ::access((directory + "/" + program).c_str(), X_OK) == 0) {
            return true;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        begin = colon + 1;
    }
    return false;
}

/**
 * @brief One way of opening a path or URI: the command line, and whether its exit status means anything.
 *
 * @details
 * - `explorer.exe` answers 1 even when it worked, so only its start can be checked; everything else is asked whether it
 *   exited with a failure (that is how "gio open" reports "this machine has no file manager").
 */
struct Opener
{
    std::vector<std::string> command;
    bool                     trust_spawn{ false };
};

/// @brief The UNC path Windows uses to reach a WSL path, e.g. `\\wsl.localhost\Ubuntu\tmp\x`.
std::string wslUncPath(const std::string& path)
{
    const char* distro = std::getenv("WSL_DISTRO_NAME");
    if (distro == nullptr || distro[0] == '\0') {
        return {};
    }

    std::string unc = std::string{ "\\\\wsl.localhost\\" } + distro;
    for (const char character : path) {
        unc += character == '/' ? '\\' : character;
    }
    return unc;
}

/**
 * @brief Every installed way to open @p target, best first.
 *
 * @details
 * - Under WSL there is usually no Linux file manager at all, but Windows Explorer reaches the same directory through the
 *   `\\wsl.localhost` share, so it is tried first and the plain Linux openers after it.
 * - On a normal desktop `/mnt/c` does not exist and the list is just the usual `xdg-open` / `gio` / file managers.
 *
 * @param target Directory or `mailto:` URI.
 * @return Candidates in the order they should be tried; empty when nothing is installed.
 */
std::vector<Opener> openerCandidates(const std::string& target)
{
    std::vector<Opener> openers;

    const std::string explorer{ "/mnt/c/Windows/explorer.exe" };
    if (::access(explorer.c_str(), X_OK) == 0) {
        const bool is_path = target.rfind("mailto:", 0) != 0;
        std::string argument = target;
        if (is_path) {
            argument = wslUncPath(target);
        }
        if (!argument.empty()) {
            openers.push_back(Opener{ { explorer, argument }, true });
        }
    }

    if (isOnPath("xdg-open")) {
        openers.push_back(Opener{ { "xdg-open", target } });
    }
    if (isOnPath("gio")) {
        openers.push_back(Opener{ { "gio", "open", target } });
    }
    for (const char* manager : { "nautilus", "thunar", "dolphin", "nemo", "pcmanfm", "caja" }) {
        if (isOnPath(manager)) {
            openers.push_back(Opener{ { manager, target } });
        }
    }
    return openers;
}

/**
 * @brief Starts @p command with its output discarded and reports whether it refused the request.
 *
 * @details
 * - A short wait (250 ms) is enough to catch "the program ran but there is nothing to open it with", which is exactly the
 *   answer `gio open` gives on a machine without a file manager.
 * - A program still running after that wait took the request: a file manager lives until the user closes it.
 *
 * @param command Program and arguments; the first entry is what `PATH` is searched for.
 * @param trust_spawn true when the exit status carries no meaning.
 * @return true when the request was accepted.
 */
bool launch(const std::vector<std::string>& command, bool trust_spawn) noexcept
{
    if (command.empty()) {
        return false;
    }

    // Built before the fork: the child only ever reaches `exec`, so it inherits these buffers instead of allocating.
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (const std::string& part : command) {
        argv.push_back(const_cast<char*>(part.c_str()));
    }
    argv.push_back(nullptr);

    // Earlier children are reaped here: the dialog outlives them, and a refusal is recognised by the exit status, so SIGCHLD
    // cannot simply be ignored. Reaping before the fork keeps this child's status this call's own.
    int stale = 0;
    while (::waitpid(-1, &stale, WNOHANG) > 0) {
    }

    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        const int null_device = ::open("/dev/null", O_RDWR);
        if (null_device >= 0) {
            ::dup2(null_device, STDOUT_FILENO);
            ::dup2(null_device, STDERR_FILENO);
        }
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    for (int attempt = 0; attempt < 25; ++attempt) {
        int         status = 0;
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            // `explorer.exe` answers 1 even when it worked, so for it only the start counts.
            return trust_spawn || (WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        ::usleep(10 * 1000);
    }
    return true;
}

/// @brief Percent-encodes @p text for a `mailto:` URL, keeping CRLF readable as a line break.
std::string percentEncode(const std::string& text)
{
    std::string encoded;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-'
            || byte == '_' || byte == '.' || byte == '~')
        {
            encoded += character;
        }
        else if (byte == '\n') {
            encoded += "%0D%0A";
        }
        else {
            char buffer[5]{};
            std::snprintf(buffer, sizeof buffer, "%%%02X", byte);
            encoded += buffer;
        }
    }
    return encoded;
}

/// @brief Splits text into lines, accepting both line endings.
std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string              current;
    for (const char character : text) {
        if (character == '\n') {
            lines.push_back(current);
            current.clear();
        }
        else if (character != '\r') {
            current += character;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

/// @brief The directory part of @p path, without the trailing slash; empty when @p path has none.
std::string directoryOf(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

/**
 * @brief One window: the whole UI of this executable, drawn with Xlib and Xft.
 *
 * @details
 * - No toolkit: the dialog is a handful of text runs, one scrollable report area and a row of buttons, so Xlib events plus
 *   Xft text are enough and the handler keeps the dependency surface of a base-layer process.
 * - Text is UTF-8 pushed through Xft, which resolves the font through fontconfig: `:lang=zh-cn` is asked for first so the
 *   interface can be Chinese like the Windows dialog.
 * - The clipboard is served the X11 way: this process owns the CLIPBOARD selection until another client takes it, and the
 *   event loop answers the requests for as long as the window lives.
 */
class Dialog
{
  public:
    explicit Dialog(Display* display);

    ~Dialog();

    Dialog(const Dialog&)            = delete;
    Dialog& operator=(const Dialog&) = delete;

    /**
     * @brief Creates the window, the drawing context and the fonts.
     *
     * @param app_name Application name, shown in the title.
     * @param report_path Path of the report, shown and used by the folder action.
     * @param dump_path Path of the dump, may be empty.
     * @param report_text Text the report area shows.
     * @param mailto Mail recipient, may be empty.
     * @return false when the window could not be created (no display, no usable font, no Xft target).
     */
    bool create(const std::string& app_name,
                const std::string& report_path,
                const std::string& dump_path,
                const std::string& report_text,
                const std::string& mailto);

    /// @brief Serves events until the user closes the window.
    void run();

  private:
    enum class Action
    {
        Copy,
        Folder,
        Mail,
        Close
    };

    struct ButtonDef
    {
        std::string label;
        Action      action{ Action::Close };
        int         x{ 0 };
        int         width{ 0 };
    };

    bool        allocateColors();
    XftFont*    openFont(const char* pattern) const;
    int         textWidth(XftFont* font, const std::string& text) const;
    void        drawText(XftFont* font, int x, int baseline, const std::string& text, const XftColor& color) const;
    std::vector<std::string> wrapUtf8(XftFont* font, int max_width, const std::string& text) const;

    void layout();
    void draw();
    void scrollBy(int lines);
    void activate(Action action);
    void copyReport();
    void openFolder();
    void sendMail();
    void openTarget(const std::string& target, const std::string& what);
    void serveSelection(const XSelectionRequestEvent& request);
    void handleEvent(XEvent& event);

    Display*  display_{ nullptr };
    int       screen_{ 0 };
    Visual*   visual_{ nullptr };
    Colormap  colormap_{ 0 };
    Window    root_{ 0 };
    Window    window_{ 0 };
    GC        gc_{ 0 };
    XftDraw*  draw_{ nullptr };
    XftFont*  body_font_{ nullptr };
    XftFont*  ui_font_{ nullptr };
    XftColor  text_{};
    XftColor  muted_{};
    XftColor  background_{};
    XftColor  button_{};
    XftColor  button_border_{};
    bool      colors_allocated_{ false };

    Atom wm_delete_{ 0 };
    Atom clipboard_{ 0 };
    Atom utf8_{ 0 };
    Atom targets_{ 0 };
    Atom text_atom_{ 0 };
    Atom net_wm_name_{ 0 };

    std::string app_name_;
    std::string report_path_;
    std::string dump_path_;
    std::string mailto_;
    std::string report_text_;
    std::string clipboard_text_;
    std::string status_;

    std::vector<std::string> title_lines_;
    std::vector<std::string> path_lines_;
    std::vector<std::string> body_lines_;
    std::vector<ButtonDef>   buttons_;
    std::size_t              first_visible_{ 0 };
    int                      width_{ kWindowWidth };
    int                      height_{ kWindowHeight };
    bool                     running_{ true };
};

Dialog::Dialog(Display* display)
    : display_(display)
    , screen_(DefaultScreen(display))
    , visual_(DefaultVisual(display, screen_))
    , colormap_(DefaultColormap(display, screen_))
{
}

Dialog::~Dialog()
{
    if (draw_ != nullptr) {
        XftDrawDestroy(draw_);
    }
    if (body_font_ != nullptr) {
        XftFontClose(display_, body_font_);
    }
    if (ui_font_ != nullptr) {
        XftFontClose(display_, ui_font_);
    }
    if (colors_allocated_) {
        XftColorFree(display_, visual_, colormap_, &text_);
        XftColorFree(display_, visual_, colormap_, &muted_);
        XftColorFree(display_, visual_, colormap_, &background_);
        XftColorFree(display_, visual_, colormap_, &button_);
        XftColorFree(display_, visual_, colormap_, &button_border_);
    }
    if (window_ != 0) {
        XDestroyWindow(display_, window_);
    }
    if (gc_ != 0) {
        XFreeGC(display_, gc_);
    }
}

bool Dialog::allocateColors()
{
    const bool ok = XftColorAllocName(display_, visual_, colormap_, "#202020", &text_) != 0
                    && XftColorAllocName(display_, visual_, colormap_, "#5a5a5a", &muted_) != 0
                    && XftColorAllocName(display_, visual_, colormap_, "#f4f4f4", &background_) != 0
                    && XftColorAllocName(display_, visual_, colormap_, "#e6e6e6", &button_) != 0
                    && XftColorAllocName(display_, visual_, colormap_, "#9a9a9a", &button_border_) != 0;
    colors_allocated_ = ok;
    return ok;
}

XftFont* Dialog::openFont(const char* pattern) const
{
    // The Chinese first: fontconfig resolves a family that carries both Latin and CJK, so one font can draw the whole
    // dialog. The plain pattern is the fallback for a machine without a CJK font installed.
    XftFont* font = XftFontOpenName(display_, screen_, pattern);
    return font;
}

int Dialog::textWidth(XftFont* font, const std::string& text) const
{
    XGlyphInfo info{};
    XftTextExtentsUtf8(display_, font, reinterpret_cast<const FcChar8*>(text.data()), static_cast<int>(text.size()), &info);
    return static_cast<int>(info.xOff);
}

void Dialog::drawText(XftFont* font, int x, int baseline, const std::string& text, const XftColor& color) const
{
    if (text.empty()) {
        return;
    }
    XftDrawStringUtf8(draw_,
                      &color,
                      font,
                      x,
                      baseline,
                      reinterpret_cast<const FcChar8*>(text.data()),
                      static_cast<int>(text.size()));
}

std::vector<std::string> Dialog::wrapUtf8(XftFont* font, int max_width, const std::string& text) const
{
    std::vector<std::string> lines;
    std::string              current;
    std::size_t              index = 0;
    while (index < text.size()) {
        std::size_t next = index + 1;
        while (next < text.size() && !isSequenceStart(text[next])) {
            ++next;
        }
        const std::string piece = text.substr(index, next - index);
        if (!current.empty() && textWidth(font, current + piece) > max_width) {
            lines.push_back(current);
            current = piece;
        }
        else {
            current += piece;
        }
        index = next;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

bool Dialog::create(const std::string& app_name,
                    const std::string& report_path,
                    const std::string& dump_path,
                    const std::string& report_text,
                    const std::string& mailto)
{
    app_name_    = app_name;
    report_path_ = report_path;
    dump_path_   = dump_path;
    report_text_ = report_text;
    mailto_      = mailto;

    body_font_ = openFont("monospace-10:lang=zh-cn");
    if (body_font_ == nullptr) {
        body_font_ = openFont("monospace-10");
    }
    ui_font_ = openFont("sans-11:lang=zh-cn");
    if (ui_font_ == nullptr) {
        ui_font_ = openFont("sans-11");
    }
    if (body_font_ == nullptr || ui_font_ == nullptr) {
        return false;
    }
    if (!allocateColors()) {
        return false;
    }

    wm_delete_   = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    clipboard_    = XInternAtom(display_, "CLIPBOARD", False);
    utf8_         = XInternAtom(display_, "UTF8_STRING", False);
    targets_      = XInternAtom(display_, "TARGETS", False);
    text_atom_    = XInternAtom(display_, "TEXT", False);
    net_wm_name_  = XInternAtom(display_, "_NET_WM_NAME", False);

    root_ = DefaultRootWindow(display_);
    XWindowAttributes root_attributes{};
    XGetWindowAttributes(display_, root_, &root_attributes);
    const int x = std::max(0, (root_attributes.width - kWindowWidth) / 2);
    const int y = std::max(0, (root_attributes.height - kWindowHeight) / 2);

    XSetWindowAttributes attributes{};
    attributes.background_pixel = background_.pixel;
    attributes.border_pixel     = button_border_.pixel;
    attributes.event_mask       = ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | StructureNotifyMask;
    window_                     = XCreateWindow(display_,
                                                root_,
                                                x,
                                                y,
                                                static_cast<unsigned int>(kWindowWidth),
                                                static_cast<unsigned int>(kWindowHeight),
                                                1,
                                                CopyFromParent,
                                                InputOutput,
                                                CopyFromParent,
                                                CWBackPixel | CWBorderPixel | CWEventMask,
                                                &attributes);
    if (window_ == 0) {
        return false;
    }

    const std::string title = app_name_ + " 已崩溃";
    XStoreName(display_, window_, title.c_str());
    XChangeProperty(display_,
                    window_,
                    net_wm_name_,
                    utf8_,
                    8,
                    PropModeReplace,
                    reinterpret_cast<const unsigned char*>(title.data()),
                    static_cast<int>(title.size()));

    XSizeHints hints{};
    hints.flags     = PMinSize;
    hints.min_width  = 560;
    hints.min_height = 360;
    XSetWMNormalHints(display_, window_, &hints);
    XSetWMProtocols(display_, window_, &wm_delete_, 1);

    gc_ = XCreateGC(display_, window_, 0, nullptr);

    draw_ = XftDrawCreate(display_, window_, visual_, colormap_);
    if (draw_ == nullptr) {
        return false;
    }

    layout();
    XMapWindow(display_, window_);
    return true;
}

void Dialog::layout()
{
    const int content_width = width_ - 2 * kPadding;

    title_lines_ = wrapUtf8(ui_font_, content_width, app_name_ + " 遇到了问题，已经退出。");

    path_lines_.clear();
    for (const std::string& line : wrapUtf8(ui_font_, content_width, "报告：" + report_path_)) {
        path_lines_.push_back(line);
    }
    for (const std::string& line :
         wrapUtf8(ui_font_, content_width, "转储：" + (dump_path_.empty() ? std::string{ "（未生成）" } : dump_path_)))
    {
        path_lines_.push_back(line);
    }
    for (const std::string& line : wrapUtf8(ui_font_, content_width, "请把这些文件交给开发者。")) {
        path_lines_.push_back(line);
    }

    body_lines_ = splitLines(report_text_);

    // The buttons are laid out right to left: close is always in the same place, the others follow when they apply.
    buttons_.clear();
    buttons_.push_back(ButtonDef{ "关闭", Action::Close, 0, 0 });
    if (!mailto_.empty()) {
        buttons_.push_back(ButtonDef{ "发送邮件", Action::Mail, 0, 0 });
    }
    buttons_.push_back(ButtonDef{ "打开文件夹", Action::Folder, 0, 0 });
    buttons_.push_back(ButtonDef{ "复制报告", Action::Copy, 0, 0 });

    int right = width_ - kPadding;
    for (ButtonDef& button : buttons_) {
        button.width = textWidth(ui_font_, button.label) + 2 * kPadding;
        right -= button.width;
        button.x = right;
        right -= kButtonGap;
    }
    std::reverse(buttons_.begin(), buttons_.end()); // Left to right, the order they are read in.
}

void Dialog::scrollBy(int lines)
{
    const int visible = std::max(0, (height_ - kPadding - 2 * kButtonHeight - 3 * kPadding)
                                         / (body_font_->ascent + body_font_->descent + 2));
    const int last = std::max(0, static_cast<int>(body_lines_.size()) - visible);
    int       target = static_cast<int>(first_visible_) + lines;
    target           = std::clamp(target, 0, last);
    if (static_cast<std::size_t>(target) != first_visible_) {
        first_visible_ = static_cast<std::size_t>(target);
        draw();
    }
}

void Dialog::draw()
{
    XClearWindow(display_, window_);

    int y = kPadding;
    y += ui_font_->ascent;
    for (const std::string& line : title_lines_) {
        drawText(ui_font_, kPadding, y, line, text_);
        y += ui_font_->ascent + ui_font_->descent + 2;
    }

    y += 6;
    for (const std::string& line : path_lines_) {
        y += ui_font_->ascent;
        drawText(ui_font_, kPadding, y, line, muted_);
        y += ui_font_->descent + 2;
    }

    y += 8;
    XSetForeground(display_, gc_, button_border_.pixel);
    XDrawLine(display_, window_, gc_, kPadding, y, width_ - kPadding, y);
    y += kPadding;

    const int line_height = body_font_->ascent + body_font_->descent + 2;
    const int buttons_top = height_ - kPadding - kButtonHeight;
    const int body_top    = y;
    const int body_height = buttons_top - kPadding - body_top;
    const int visible     = std::max(0, body_height / line_height);

    for (int row = 0; row < visible; ++row) {
        const std::size_t index = first_visible_ + static_cast<std::size_t>(row);
        if (index >= body_lines_.size()) {
            break;
        }
        drawText(body_font_, kPadding, body_top + row * line_height + body_font_->ascent, body_lines_[index], text_);
    }

    if (body_lines_.size() > static_cast<std::size_t>(visible) && visible > 0) {
        // A plain thumb, not a themed scrollbar: it only has to say "there is more above or below".
        const int track_x = width_ - kPadding - kScrollBarWidth;
        XSetForeground(display_, gc_, button_.pixel);
        XFillRectangle(display_, window_, gc_, track_x, body_top, kScrollBarWidth, body_height);
        const int thumb_height = std::max(line_height,
                                          body_height * visible / static_cast<int>(body_lines_.size()));
        const int travel       = body_height - thumb_height;
        const int last         = std::max(1, static_cast<int>(body_lines_.size()) - visible);
        const int thumb_y      = body_top + travel * static_cast<int>(first_visible_) / last;
        XSetForeground(display_, gc_, button_border_.pixel);
        XFillRectangle(display_, window_, gc_, track_x, thumb_y, kScrollBarWidth, thumb_height);
    }

    if (!status_.empty()) {
        drawText(ui_font_, kPadding, buttons_top - kPadding / 2, status_, muted_);
    }

    for (const ButtonDef& button : buttons_) {
        XSetForeground(display_, gc_, button_.pixel);
        XFillRectangle(display_,
                       window_,
                       gc_,
                       button.x,
                       buttons_top,
                       static_cast<unsigned int>(button.width),
                       static_cast<unsigned int>(kButtonHeight));
        XSetForeground(display_, gc_, button_border_.pixel);
        XDrawRectangle(display_,
                       window_,
                       gc_,
                       button.x,
                       buttons_top,
                       static_cast<unsigned int>(button.width - 1),
                       static_cast<unsigned int>(kButtonHeight - 1));
        const int label_x = button.x + (button.width - textWidth(ui_font_, button.label)) / 2;
        drawText(ui_font_, label_x, buttons_top + (kButtonHeight + ui_font_->ascent - ui_font_->descent) / 2, button.label, text_);
    }

    XFlush(display_);
}

void Dialog::activate(Action action)
{
    switch (action) {
    case Action::Copy:
        copyReport();
        break;
    case Action::Folder:
        openFolder();
        break;
    case Action::Mail:
        sendMail();
        break;
    case Action::Close:
        running_ = false;
        return;
    }
    draw();
}

void Dialog::copyReport()
{
    clipboard_text_ = report_text_ + "\r\ndump: " + (dump_path_.empty() ? std::string{ "(none)" } : dump_path_);
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    status_ = clipboard_ == None ? std::string{} : std::string{ "报告已复制到剪贴板，可以直接粘贴。" };
}

void Dialog::openFolder()
{
    const std::string directory = directoryOf(report_path_);
    openTarget(directory.empty() ? report_path_ : directory, "文件夹");
}

void Dialog::openTarget(const std::string& target, const std::string& what)
{
    for (const Opener& opener : openerCandidates(target)) {
        if (launch(opener.command, opener.trust_spawn)) {
            status_ = "已打开" + what + "。";
            return;
        }
    }

    // Nothing on this machine can show it. Doing nothing is the one answer a crash dialog cannot give, so the path goes to
    // the clipboard and the status line says so.
    clipboard_text_ = target;
    XSetSelectionOwner(display_, clipboard_, window_, CurrentTime);
    status_ = "没有能打开" + what + "的程序；路径已复制到剪贴板。";
}

void Dialog::sendMail()
{
    const std::string subject = percentEncode(app_name_ + " 崩溃报告");
    std::string       body    = app_name_ + " 崩溃报告位于：\n" + report_path_ + "\n";
    if (!dump_path_.empty()) {
        body += "\n转储文件（请手动作为附件添加）：\n" + dump_path_ + "\n";
    }
    body += "\n（报告正文见报告旁的 .txt 文件；本邮件由崩溃窗口预填。）";

    openTarget("mailto:" + mailto_ + "?subject=" + subject + "&body=" + percentEncode(body), "邮件草稿");
}

void Dialog::serveSelection(const XSelectionRequestEvent& request)
{
    XSelectionEvent reply{};
    reply.type      = SelectionNotify;
    reply.display   = request.display;
    reply.requestor = request.requestor;
    reply.selection = request.selection;
    reply.target    = request.target;
    reply.time      = request.time;
    reply.property  = None;

    Atom property = request.property != None ? request.property : request.target;
    if (request.target == targets_) {
        const Atom supported[]{ utf8_, XA_STRING, text_atom_ };
        XChangeProperty(display_,
                        request.requestor,
                        property,
                        XA_ATOM,
                        32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(supported),
                        3);
        reply.property = property;
    }
    else if (request.target == utf8_ || request.target == text_atom_ || request.target == XA_STRING) {
        const Atom type = request.target == utf8_ ? utf8_ : XA_STRING;
        XChangeProperty(display_,
                        request.requestor,
                        property,
                        type,
                        8,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char*>(clipboard_text_.data()),
                        static_cast<int>(clipboard_text_.size()));
        reply.property = property;
    }

    XSendEvent(display_, request.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&reply));
    XFlush(display_);
}

void Dialog::handleEvent(XEvent& event)
{
    switch (event.type) {
    case Expose:
        if (event.xexpose.count == 0) {
            draw();
        }
        break;
    case ConfigureNotify:
        if (event.xconfigure.width != width_ || event.xconfigure.height != height_) {
            width_  = event.xconfigure.width;
            height_ = event.xconfigure.height;
            layout();
            draw();
        }
        break;
    case ButtonPress:
        if (event.xbutton.button == Button4) {
            scrollBy(-3);
        }
        else if (event.xbutton.button == Button5) {
            scrollBy(3);
        }
        break;
    case ButtonRelease: {
        if (event.xbutton.button != Button1) {
            break;
        }
        const int buttons_top = height_ - kPadding - kButtonHeight;
        if (event.xbutton.y < buttons_top || event.xbutton.y > buttons_top + kButtonHeight) {
            break;
        }
        for (const ButtonDef& button : buttons_) {
            if (event.xbutton.x >= button.x && event.xbutton.x <= button.x + button.width) {
                activate(button.action);
                break;
            }
        }
    } break;
    case KeyPress: {
        const KeySym key = XLookupKeysym(&event.xkey, 0);
        if (key == XK_Up) {
            scrollBy(-1);
        }
        else if (key == XK_Down) {
            scrollBy(1);
        }
        else if (key == XK_Prior) {
            scrollBy(-10);
        }
        else if (key == XK_Next) {
            scrollBy(10);
        }
        else if (key == XK_Home) {
            first_visible_ = 0;
            draw();
        }
        else if (key == XK_End) {
            first_visible_ = body_lines_.size();
            scrollBy(0);
        }
    } break;
    default:
        break;
    }
}

void Dialog::run()
{
    while (running_) {
        XEvent event{};
        XNextEvent(display_, &event);

        if (event.type == ClientMessage && static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_) {
            running_ = false;
            continue;
        }
        if (event.type == SelectionRequest) {
            serveSelection(event.xselectionrequest);
            continue;
        }
        if (event.type == SelectionClear) {
            clipboard_text_.clear();
            continue;
        }
        if (event.type == KeyPress) {
            const KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_Escape || key == XK_Return || key == XK_q) {
                running_ = false;
                continue;
            }
        }
        handleEvent(event);
    }
}

} // namespace

bool showWindow(const std::string& app_name,
                const std::string& report_path,
                const std::string& dump_path,
                const std::string& report_text,
                const std::string& mailto)
{
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        return false;
    }

    bool shown = false;
    {
        Dialog dialog{ display };
        if (dialog.create(app_name, report_path, dump_path, report_text, mailto)) {
            dialog.run();
            shown = true;
        }
    }
    XCloseDisplay(display);
    return shown;
}

} // namespace handler
