#include <vine/appfw/gui/RibbonAction.hpp>

#include <QAction>
#include <QIcon>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>

#include <vine/appfw/gui/UIElementData.hpp>

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RibbonAction, UIElement)

struct RibbonAction::Impl : public UIElementData {
    void*     user = nullptr;
    EventArgs triggeredArgs;
    String    command;
};

RibbonAction::RibbonAction()
  : UIElement(new Impl(), new QAction(nullptr))
{
    auto* act = impl<QAction>();

    if (act) {
        // Bridge the Qt triggered signal to the framework's `triggered` event.
        QObject::connect(act, &QAction::triggered, [this](bool) { triggered.trigger(*this, dptr()->triggeredArgs); });
    }

    // When a command is configured, execute it when the action is triggered.
    // detach(): this is the widget wiring its own signal, so the subscription should last
    // exactly as long as the widget does.
    triggered.connect([](RibbonAction& self, EventArgs&) {
        const String cmd = self.command();
        if (!cmd.empty()) {
            if (auto* app = Application::current()) {
                app->commandManager()->executeDetached(cmd);
            }
        }
    }).detach();
}

RibbonAction::~RibbonAction()
{
    // d is deleted by UIElement
}

void RibbonAction::setText(const String& t)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    auto utf16 = t.toUtf16();
    act->setText(QString::fromStdU16String(utf16));
}

String RibbonAction::text() const
{
    auto* act = impl<QAction>();
    if (!act)
        return {};
    auto qs = act->text();
    return String::fromUtf16((const char16_t*)qs.utf16(), qs.size());
}

void RibbonAction::setIcon(const Icon& ic)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    act->setIcon(ic.value());
}

Icon RibbonAction::icon() const
{
    auto* act = impl<QAction>();
    if (!act)
        return {};
    return Icon(act->icon());
}

void RibbonAction::setTooltip(const String& t)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    auto utf16 = t.toUtf16();
    act->setToolTip(QString::fromStdU16String(utf16));
}

String RibbonAction::tooltip() const
{
    auto* act = impl<QAction>();
    if (!act)
        return {};
    auto qs = act->toolTip();
    return String::fromUtf16((const char16_t*)qs.utf16(), qs.size());
}

void RibbonAction::setEnabled(bool on)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    act->setEnabled(on);
}

bool RibbonAction::enabled() const
{
    auto* act = impl<QAction>();
    return act && act->isEnabled();
}

void RibbonAction::setCheckable(bool on)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    act->setCheckable(on);
}

bool RibbonAction::checkable() const
{
    auto* act = impl<QAction>();
    return act && act->isCheckable();
}

void RibbonAction::setChecked(bool on)
{
    auto* act = impl<QAction>();
    if (!act)
        return;
    act->setChecked(on);
}

bool RibbonAction::checked() const
{
    auto* act = impl<QAction>();
    return act && act->isChecked();
}

void RibbonAction::setData(void* data)
{
    dptr()->user = data;
}

void* RibbonAction::data() const
{
    return dptr()->user;
}

void RibbonAction::setCommand(const String& command)
{
    dptr()->command = command;
}

String RibbonAction::command() const
{
    return dptr()->command;
}

inline auto RibbonAction::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RibbonAction::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
