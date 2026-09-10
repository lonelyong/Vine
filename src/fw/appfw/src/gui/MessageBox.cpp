#include <vine/appfw/gui/MessageBox.hpp>

#include <QCoreApplication>
#include <QMessageBox>
#include <QThread>

#include <functional>

#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

/**
 * @brief Maps a framework icon to the Qt message box icon.
 *
 * @param icon Framework icon to map.
 * @return The equivalent Qt message box icon.
 */
QMessageBox::Icon toQtIcon(MessageBoxIcon icon)
{
    switch (icon) {
    case MessageBoxIcon::Information: return QMessageBox::Information;
    case MessageBoxIcon::Warning: return QMessageBox::Warning;
    case MessageBoxIcon::Critical: return QMessageBox::Critical;
    case MessageBoxIcon::Question: return QMessageBox::Question;
    case MessageBoxIcon::None: break;
    }
    return QMessageBox::NoIcon;
}

/**
 * @brief Maps a framework button preset to the Qt standard buttons.
 *
 * @param buttons Framework button preset to map.
 * @return The equivalent Qt standard buttons.
 */
QMessageBox::StandardButtons toQtButtons(MessageBoxButton buttons)
{
    QMessageBox::StandardButtons qbuttons = QMessageBox::NoButton;
    if (!!(buttons & MessageBoxButton::Ok)) {
        qbuttons |= QMessageBox::Ok;
    }
    if (!!(buttons & MessageBoxButton::Cancel)) {
        qbuttons |= QMessageBox::Cancel;
    }
    if (!!(buttons & MessageBoxButton::Yes)) {
        qbuttons |= QMessageBox::Yes;
    }
    if (!!(buttons & MessageBoxButton::No)) {
        qbuttons |= QMessageBox::No;
    }
    return qbuttons;
}

/**
 * @brief Maps a single framework button to the Qt default button.
 *
 * Preset combinations are not valid default buttons and map to NoButton.
 *
 * @param button Framework button to map.
 * @return The equivalent Qt standard button, or NoButton for combinations.
 */
QMessageBox::StandardButton toQtDefaultButton(MessageBoxButton button)
{
    switch (button) {
    case MessageBoxButton::Ok: return QMessageBox::Ok;
    case MessageBoxButton::Cancel: return QMessageBox::Cancel;
    case MessageBoxButton::Yes: return QMessageBox::Yes;
    case MessageBoxButton::No: return QMessageBox::No;
    default: break;
    }
    return QMessageBox::NoButton;
}

/**
 * @brief Maps a clicked Qt standard button back to the framework button.
 *
 * A box dismissed without clicking a button (or an unknown result) maps to
 * Cancel so callers never mistake it for an affirmative answer.
 *
 * @param qbutton Qt standard button that was clicked.
 * @return The equivalent framework button.
 */
MessageBoxButton toVineButton(QMessageBox::StandardButton qbutton)
{
    switch (qbutton) {
    case QMessageBox::Ok: return MessageBoxButton::Ok;
    case QMessageBox::Cancel: return MessageBoxButton::Cancel;
    case QMessageBox::Yes: return MessageBoxButton::Yes;
    case QMessageBox::No: return MessageBoxButton::No;
    default: break;
    }
    return MessageBoxButton::Cancel;
}

/**
 * @brief Runs a function on the GUI thread and blocks until it has finished.
 *
 * When the caller is already on the GUI thread the function runs directly;
 * otherwise it is queued to the GUI thread and the caller blocks until the
 * function has been handled.
 *
 * @param fn Function to run on the GUI thread.
 * @return The value produced by the function; Cancel when no application
 * exists.
 */
MessageBoxButton runOnGuiThread(const std::function<MessageBoxButton()>& fn)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) {
        return MessageBoxButton::Cancel;
    }

    if (QThread::currentThread() == app->thread()) {
        return fn();
    }

    MessageBoxButton result = MessageBoxButton::Cancel;

    QMetaObject::invokeMethod(app, [&result, &fn]() { result = fn(); }, Qt::BlockingQueuedConnection);

    return result;
}

/**
 * @brief Builds and runs a message box on the GUI thread.
 *
 * @param parent        Parent widget; may be null.
 * @param title         Window title of the box.
 * @param text          Message text shown in the box.
 * @param icon          Icon displayed in the box.
 * @param buttons       Buttons offered by the box.
 * @param default_button Default button; only applied when has_default is true.
 * @param has_default   Whether an explicit default button was requested.
 * @return The button the user clicked.
 */
MessageBoxButton
runBox(Window* owner, const String& title, const String& text, MessageBoxIcon icon, MessageBoxButton buttons, MessageBoxButton default_button, bool has_default)
{
    return runOnGuiThread([=]() -> MessageBoxButton {
        MessageBox box(owner);
        box.setWindowTitle(title);
        box.setText(text);
        box.setIcon(icon);
        box.setButtons(buttons);
        if (has_default) {
            box.setDefaultButton(default_button);
        }
        return box.exec();
    });
}

} // namespace

V_OBJECT_META_IMPL(MessageBox, Window)

struct MessageBox::Impl : public UIElementData {
    String           text;
    MessageBoxIcon   icon            = MessageBoxIcon::None;
    MessageBoxButton buttons         = MessageBoxButton::Ok;
    bool             has_default_btn = false;
    MessageBoxButton default_button  = MessageBoxButton::No;
};

MessageBox::MessageBox(Window* owner)
  : Window(new Impl(), new QMessageBox(owner ? owner->impl<QWidget>() : nullptr))
{
    apply();
}

MessageBox::~MessageBox()
{
    // d is released by UIElement.
}

void MessageBox::setText(const String& text)
{
    dptr()->text = text;
    apply();
}

String MessageBox::text() const
{
    return dptr()->text;
}

void MessageBox::setIcon(MessageBoxIcon icon)
{
    dptr()->icon = icon;
    apply();
}

MessageBoxIcon MessageBox::icon() const
{
    return dptr()->icon;
}

void MessageBox::setButtons(MessageBoxButton buttons)
{
    dptr()->buttons = buttons;
    apply();
}

MessageBoxButton MessageBox::buttons() const
{
    return dptr()->buttons;
}

void MessageBox::setDefaultButton(MessageBoxButton button)
{
    auto* data            = dptr();
    data->has_default_btn = true;
    data->default_button  = button;
    apply();
}

bool MessageBox::hasDefaultButton() const
{
    return dptr()->has_default_btn;
}

MessageBoxButton MessageBox::exec()
{
    apply();
    auto* native = impl<QMessageBox>();
    return toVineButton(static_cast<QMessageBox::StandardButton>(native->exec()));
}

void MessageBox::apply()
{
    auto* data   = dptr();
    auto* native = impl<QMessageBox>();
    native->setText(Convert::toQString(data->text));
    native->setIcon(toQtIcon(data->icon));
    native->setStandardButtons(toQtButtons(data->buttons));
    native->setDefaultButton(data->has_default_btn ? toQtDefaultButton(data->default_button) : QMessageBox::NoButton);
}

MessageBoxButton MessageBox::information(Window* owner, const String& title, const String& text, MessageBoxButton buttons)
{
    return runBox(owner, title.empty() ? String(u8"提示") : title, text, MessageBoxIcon::Information, buttons, MessageBoxButton::No, false);
}

MessageBoxButton MessageBox::warning(Window* owner, const String& title, const String& text, MessageBoxButton buttons)
{
    return runBox(owner, title.empty() ? String(u8"警告") : title, text, MessageBoxIcon::Warning, buttons, MessageBoxButton::No, false);
}

MessageBoxButton MessageBox::critical(Window* owner, const String& title, const String& text, MessageBoxButton buttons)
{
    return runBox(owner, title.empty() ? String(u8"错误") : title, text, MessageBoxIcon::Critical, buttons, MessageBoxButton::No, false);
}

MessageBoxButton MessageBox::question(Window* owner, const String& title, const String& text, MessageBoxButton buttons, MessageBoxButton default_button)
{
    return runBox(owner, title.empty() ? String(u8"请确认") : title, text, MessageBoxIcon::Question, buttons, default_button, true);
}

inline auto MessageBox::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto MessageBox::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
