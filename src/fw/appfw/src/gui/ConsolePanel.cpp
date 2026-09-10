#include <vine/appfw/gui/ConsolePanel.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScreen>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/UIElementData.hpp>

#include "CommandCompleter.hpp"
#include "CommandHistory.hpp"
#include "Convert.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

QColor toQColor(const Color& c)
{
    return QColor(c.r, c.g, c.b, c.a);
}

/**
 * @brief Single-line input routing Up/Down/Tab/Esc to the panel.
 */
class InputLine : public QLineEdit {
  public:
    using QLineEdit::QLineEdit;

    std::function<void()> onEscape;
    std::function<void()> onHistoryUp;
    std::function<void()> onHistoryDown;
    std::function<void()> onTab;

  protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        switch (e->key()) {
        case Qt::Key_Escape:
            if (onEscape) {
                onEscape();
            }
            return;
        case Qt::Key_Up:
            if (onHistoryUp) {
                onHistoryUp();
            }
            return;
        case Qt::Key_Down:
            if (onHistoryDown) {
                onHistoryDown();
            }
            return;
        case Qt::Key_Tab:
            if (onTab) {
                onTab();
            }
            return;
        default: break;
        }
        QLineEdit::keyPressEvent(e);
    }
};

} // namespace

namespace
{

/**
 * @brief Hides the suggestion popup when the user clicks outside it.
 *
 * A Qt::Tool window (unlike Qt::Popup) does not grab the mouse, so outside
 * clicks are detected with an application-wide event filter instead.
 */
class SuggestCloseFilter : public QObject {
  public:
    QListWidget* popup = nullptr;

  protected:
    bool eventFilter(QObject* watched, QEvent* e) override
    {
        if (popup == nullptr || !popup->isVisible()) {
            return QObject::eventFilter(watched, e);
        }

        if (e->type() == QEvent::MouseButtonPress) {
            auto* me = static_cast<QMouseEvent*>(e);
            if (!popup->geometry().contains(me->globalPosition().toPoint())) {
                popup->hide();
            }
        }
        else if (e->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(e);
            if (ke->key() == Qt::Key_Escape) {
                // Consume the first Escape: it only closes the popup.
                popup->hide();
                return true;
            }
        }
        return QObject::eventFilter(watched, e);
    }
};

} // namespace

V_OBJECT_META_IMPL(ConsolePanel, Control)

struct ConsolePanel::Impl : public UIElementData {
    /// Owning panel, used by the input handlers to trigger signals.
    ConsolePanel*   panel  = nullptr;
    QPlainTextEdit* output = nullptr;
    InputLine*      input  = nullptr;

    /// VS Code-style completion popup (frameless Qt::Tool window, owned here).
    std::unique_ptr<QListWidget> suggest;
    /// Event filter closing the popup on outside clicks.
    std::unique_ptr<SuggestCloseFilter> suggest_filter;

    ConsoleTheme theme = ConsoleTheme::dark();

    /// Handler id of the GuiApplication::theme_changed subscription (0 = unset).
    std::size_t theme_handler_id_{ 0 };

    CommandHistory   history;
    CommandCompleter completer;

    /// Current prefix matches shown in the popup.
    std::vector<ConsoleCommandEntry> matches;

    /**
     * @brief Resolves the display color for a message type from the theme.
     *
     * @param type Message type to resolve.
     * @return The color used to render that message type.
     */
    QColor colorFor(ConsoleMessageType type) const;

    void appendFormatted(ConsoleMessageType type, const String& text);
    void onTextChanged();
    void updateSuggest();
    void onEscape();
    void onReturnPressed();
    void onHistoryUp();
    void onHistoryDown();
    void onTab();

    ~Impl();
};

ConsolePanel::Impl::~Impl() = default;

ConsolePanel::ConsolePanel(QWidget* parent)
  : Control(new Impl(), new QWidget(parent))
{
    auto* data = dptr();
    auto* root = impl<QWidget>();

    data->panel = this;

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    data->output = new QPlainTextEdit(root);
    data->output->setReadOnly(true);

    data->input = new InputLine(root);

    // VS Code-style completion popup: a frameless Qt::Tool window under the input
    // line. Unlike Qt::Popup it does not grab the mouse (the main window stays
    // draggable and focused); outside clicks are closed by an event filter.
    data->suggest = std::make_unique<QListWidget>();
    data->suggest->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus);
    data->suggest->setAttribute(Qt::WA_ShowWithoutActivating);
    data->suggest->setFocusPolicy(Qt::NoFocus);
    data->suggest->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    data->suggest->hide();

    data->suggest_filter        = std::make_unique<SuggestCloseFilter>();
    data->suggest_filter->popup = data->suggest.get();
    if (auto* app = QCoreApplication::instance()) {
        app->installEventFilter(data->suggest_filter.get());
    }

    layout->addWidget(data->output);
    layout->addWidget(data->input);

    QObject::connect(data->input, &QLineEdit::returnPressed, root, [data] { data->onReturnPressed(); });
    QObject::connect(data->input, &QLineEdit::textChanged, root, [data] { data->onTextChanged(); });
    QObject::connect(data->suggest.get(), &QListWidget::itemClicked, root, [data] { data->onReturnPressed(); });
    data->input->onEscape      = [data] { data->onEscape(); };
    data->input->onHistoryUp   = [data] { data->onHistoryUp(); };
    data->input->onHistoryDown = [data] { data->onHistoryDown(); };
    data->input->onTab         = [data] { data->onTab(); };

    // Follow the application theme for the semantic color scheme so the text
    // stays readable in both light and dark themes.
    if (auto* app = obj_cast<GuiApplication>(Application::current())) {
        data->theme_handler_id_ = app->theme_changed.addHandler([this](Theme) { applyAppTheme(); });
    }
    applyAppTheme();
}

ConsolePanel::~ConsolePanel()
{
    if (auto* app = obj_cast<GuiApplication>(Application::current())) {
        app->theme_changed.removeHandler(dptr()->theme_handler_id_);
    }
    if (auto* core_app = QCoreApplication::instance()) {
        core_app->removeEventFilter(dptr()->suggest_filter.get());
    }
    // The popup and its filter are parentless (single owner), so they are
    // released automatically when d (Impl) is deleted by UIElement.
}

void ConsolePanel::append(ConsoleMessageType type, const String& text)
{
    dptr()->appendFormatted(type, text);
}

void ConsolePanel::clear()
{
    dptr()->output->clear();
}

void ConsolePanel::beginInput(const String& prompt)
{
    auto* data = dptr();
    data->appendFormatted(ConsoleMessageType::Prompt, prompt);
    data->input->clear();
    data->input->setFocus();
}

void ConsolePanel::clearInput()
{
    dptr()->input->clear();
}

void ConsolePanel::setCommandEntries(const std::vector<ConsoleCommandEntry>& entries)
{
    auto* data = dptr();
    data->completer.setEntries(entries);
    data->matches.clear();
    data->suggest->hide();
}

void ConsolePanel::setTheme(const ConsoleTheme& theme)
{
    dptr()->theme = theme;
}

const ConsoleTheme& ConsolePanel::theme() const
{
    return dptr()->theme;
}

void ConsolePanel::applyAppTheme()
{
    const auto* app = obj_cast<GuiApplication>(Application::current());
    if (app == nullptr) {
        return;
    }
    dptr()->theme = app->theme() == Theme::Dark ? ConsoleTheme::dark() : ConsoleTheme::light();
}

QColor ConsolePanel::Impl::colorFor(ConsoleMessageType type) const
{
    switch (type) {
    case ConsoleMessageType::Command: return toQColor(theme.command);
    case ConsoleMessageType::Prompt: return toQColor(theme.prompt);
    case ConsoleMessageType::Warning: return toQColor(theme.warning);
    case ConsoleMessageType::Error: return toQColor(theme.error);
    default: return toQColor(theme.normal);
    }
}

void ConsolePanel::Impl::appendFormatted(ConsoleMessageType type, const String& text)
{
    QTextCharFormat fmt;
    fmt.setForeground(colorFor(type));
    if (type == ConsoleMessageType::Command || type == ConsoleMessageType::Prompt) {
        fmt.setFontWeight(QFont::Bold);
    }

    QTextCursor cursor(output->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(Convert::toQString(text) + QStringLiteral("\n"), fmt);
    output->setTextCursor(cursor);
    output->ensureCursorVisible();
}

void ConsolePanel::Impl::onTextChanged()
{
    const String current = Convert::fromQString(input->text());
    if (current.empty()) {
        // An empty prompt hides the popup; otherwise clearing the input after
        // running a command would match every command and re-show the popup.
        matches.clear();
        suggest->hide();
        return;
    }
    matches = completer.complete(current);
    updateSuggest();
}

void ConsolePanel::Impl::updateSuggest()
{
    if (matches.empty()) {
        suggest->hide();
        return;
    }

    suggest->clear();
    for (const auto& entry : matches) {
        // VS Code-style "source: name", e.g. "app_shell: show_plugins".
        QString text = Convert::toQString(entry.name);
        if (!entry.source.empty()) {
            text = Convert::toQString(entry.source) + QStringLiteral(": ") + text;
        }
        if (!entry.aliases.empty()) {
            QString aliases;
            bool    first = true;
            for (const auto& alias : entry.aliases) {
                if (!first) {
                    aliases += QStringLiteral(", ");
                }
                aliases += Convert::toQString(alias);
                first = false;
            }
            text += QStringLiteral(" (") + aliases + QStringLiteral(")");
        }
        if (!entry.description.empty()) {
            text += QStringLiteral("  ") + Convert::toQString(entry.description);
        }
        suggest->addItem(text);
    }

    suggest->setCurrentRow(0);
    suggest->setFixedWidth(std::max(input->width(), 360));
    const int height = static_cast<int>(matches.size()) * 22 + 4;
    suggest->setFixedHeight(std::min(height, 220));

    if (!suggest->isVisible()) {
        suggest->show();
        suggest->raise();
    }

    // Position the popup right below the input line; flip it above when it
    // would run off the bottom of the screen, and clamp it to the screen.
    QPoint pos = input->mapToGlobal(QPoint(0, input->height() + 2));
    if (QScreen* scr = input->window() ? input->window()->screen() : nullptr) {
        const QRect avail = scr->availableGeometry();
        if (pos.y() + suggest->height() > avail.bottom()) {
            pos.setY(input->mapToGlobal(QPoint(0, 0)).y() - suggest->height() - 2);
        }
        pos.setX(std::clamp(pos.x(), avail.left(), avail.right() - suggest->width()));
    }
    suggest->move(pos);

    // Keep keyboard input on the command line while the popup is open.
    input->setFocus();
}

void ConsolePanel::Impl::onEscape()
{
    if (suggest->isVisible()) {
        suggest->hide();
        return;
    }
    panel->escapePressed.trigger();
}

void ConsolePanel::Impl::onReturnPressed()
{
    String text = Convert::fromQString(input->text());
    if (suggest->isVisible() && suggest->currentRow() >= 0 && suggest->currentRow() < static_cast<int>(matches.size())) {
        // Keep the typed text when it is already a complete command or alias;
        // otherwise run the highlighted suggestion's canonical name.
        bool exact = false;
        for (const auto& m : matches) {
            if (m.name == text) {
                exact = true;
                break;
            }
            for (const auto& a : m.aliases) {
                if (a == text) {
                    exact = true;
                    break;
                }
            }
            if (exact) {
                break;
            }
        }
        if (!exact) {
            text = matches[suggest->currentRow()].name;
        }
    }
    suggest->hide();
    input->clear();
    history.add(text);
    panel->lineEntered.trigger(text);
}

void ConsolePanel::Impl::onHistoryUp()
{
    if (suggest->isVisible()) {
        const int row = suggest->currentRow();
        suggest->setCurrentRow(row > 0 ? row - 1 : 0);
        return;
    }
    const String current = Convert::fromQString(input->text());
    const String cmd     = history.previous(current);
    if (!cmd.empty()) {
        input->setText(Convert::toQString(cmd));
    }
}

void ConsolePanel::Impl::onHistoryDown()
{
    if (suggest->isVisible()) {
        const int row  = suggest->currentRow();
        const int last = suggest->count() - 1;
        suggest->setCurrentRow(row >= 0 && row < last ? row + 1 : row);
        return;
    }
    const String cmd = history.next();
    input->setText(Convert::toQString(cmd));
}

void ConsolePanel::Impl::onTab()
{
    if (matches.empty()) {
        return;
    }
    const int row = suggest->currentRow() >= 0 ? suggest->currentRow() : 0;
    if (row >= 0 && row < static_cast<int>(matches.size())) {
        input->setText(Convert::toQString(matches[row].name));
        input->setCursorPosition(input->text().size());
    }
}

inline auto ConsolePanel::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto ConsolePanel::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
