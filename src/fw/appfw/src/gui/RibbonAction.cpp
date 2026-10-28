#include <vine/appfw/gui/RibbonAction.hpp>

#include <QAction>
#include <QIcon>
#include <optional>
#include <utility>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>

#include <vine/appfw/gui/UIElementData.hpp>

#include "CommandBinding.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RibbonAction, UIElement)

struct RibbonAction::Impl : public UIElementData {
    void*     user = nullptr;
    EventArgs triggeredArgs;

    /// Keeps the action in step with the command setCommand() names (enabled/tooltip/checked).
    detail::CommandBinding binding;
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
    triggered
        .connect([](RibbonAction& self, EventArgs&) {
            const String cmd = self.command();
            if (!cmd.empty()) {
                if (auto* app = Application::current()) {
                    // Resolve at dispatch and carry it: a ribbon control belongs to one window, so the window that is
                    // current here is the one whose view the command acts on (single window today; the initiator's own
                    // window once there are several).
                    app->commandManager()->executeDetached(cmd, resolveCommandTarget(MainWindow::current()));
                }
            }
        })
        .detach();

    // A command action is a view of its command (menu entries and Backstage items included): derive its state here.
    dptr()->binding.attach(
        [this](bool enabled, const String& tip, std::optional<bool> checked) {
            auto* act = impl<QAction>();
            if (!act) {
                return;
            }
            act->setEnabled(enabled);

            const QString desired = QString::fromStdU16String(tip.toUtf16());
            if (act->toolTip() != desired) {
                act->setToolTip(desired);
            }

            if (checked.has_value()) {
                act->setCheckable(true);
                act->setChecked(*checked);
            }
        },
        [this] { return tooltip(); });
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

bool RibbonAction::isEnabled() const
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

bool RibbonAction::isCheckable() const
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

bool RibbonAction::isChecked() const
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
    dptr()->binding.setCommand(command);
}

String RibbonAction::command() const
{
    return dptr()->binding.command();
}

void RibbonAction::setCheckedProvider(std::function<bool()> provider)
{
    dptr()->binding.setCheckedProvider(std::move(provider));
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
