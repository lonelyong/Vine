#include <vine/appfw/gui/RibbonButton.hpp>

#include <QAction>
#include <QIcon>
#include <QSize>
#include <QToolButton>
#include <SARibbon.h>
#include <algorithm>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>

#include "ControlData.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

Qt::ToolButtonStyle toQtStyle(RibbonButtonStyle s)
{
    switch (s) {
    case RibbonButtonStyle::IconOnly: return Qt::ToolButtonIconOnly;
    case RibbonButtonStyle::TextOnly: return Qt::ToolButtonTextOnly;
    case RibbonButtonStyle::TextBesideIcon: return Qt::ToolButtonTextBesideIcon;
    case RibbonButtonStyle::TextUnderIcon: return Qt::ToolButtonTextUnderIcon;
    }

    return Qt::ToolButtonIconOnly;
}

RibbonButtonStyle fromQtStyle(Qt::ToolButtonStyle s)
{
    switch (s) {
    case Qt::ToolButtonTextOnly: return RibbonButtonStyle::TextOnly;
    case Qt::ToolButtonTextBesideIcon: return RibbonButtonStyle::TextBesideIcon;
    case Qt::ToolButtonTextUnderIcon: return RibbonButtonStyle::TextUnderIcon;
    default: break;
    }

    return RibbonButtonStyle::IconOnly;
}

} // namespace

V_OBJECT_META_IMPL(RibbonButton, Control)

struct RibbonButton::Impl : public ControlData {
    void*          user       = nullptr;
    RibbonItemSize buttonSize = RibbonItemSize::Small;
    String         command;

    /// Drop-down menu entry (ordered): item == nullptr means a separator.
    struct Entry {
        RibbonAction* item = nullptr;
    };

    std::vector<Entry>    entries;    // ordered entries (items/separators)
    std::vector<QAction*> separators; // separator QActions created by this button (freed on rebuild)
    SARibbonMenu*         menu = nullptr;
    EventArgs             clickedArgs;
};

RibbonButton::RibbonButton()
  : Control(new Impl(), new SARibbonToolButton(static_cast<QWidget*>(nullptr)))
{
    auto* btn = impl<SARibbonToolButton>();

    if (btn) {
        // Bridge the Qt clicked signal to the framework's `clicked` event.
        QObject::connect(btn, &QToolButton::clicked, [this](bool) { clicked.trigger(*this, dptr()->clickedArgs); });
    }

    // When a command is configured, execute it on click so simple command
    // buttons do not need a manually bound callback. release() because this is the
    // widget wiring its own signal: the subscription lasts as long as the widget.
    clicked.subscribe([](RibbonButton& self, EventArgs&) {
        const String cmd = self.command();
        if (!cmd.empty()) {
            if (auto* app = Application::current()) {
                app->commandManager()->executeDetached(cmd);
            }
        }
    }).release();
}

RibbonButton::~RibbonButton()
{
    // d is deleted by UIElement
}

void RibbonButton::setText(const String& t)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    auto utf16 = t.toUtf16();
    btn->setText(QString::fromStdU16String(utf16));
}

String RibbonButton::text() const
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return {};
    auto qs = btn->text();
    return String::fromUtf16((const char16_t*)qs.utf16(), qs.size());
}

void RibbonButton::setIcon(const Icon& ic)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setIcon(ic.value());
}

Icon RibbonButton::icon() const
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return {};
    return Icon(btn->icon());
}

void RibbonButton::setCheckable(bool on)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setCheckable(on);
}

bool RibbonButton::checkable() const
{
    auto* btn = impl<SARibbonToolButton>();
    return btn && btn->isCheckable();
}

void RibbonButton::setChecked(bool on)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setChecked(on);
}

bool RibbonButton::checked() const
{
    auto* btn = impl<SARibbonToolButton>();
    return btn && btn->isChecked();
}

void RibbonButton::setButtonSize(RibbonItemSize s)
{
    dptr()->buttonSize = s;

    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;

    // SARibbon button types only have Large/Small: Medium uses SmallButton,
    // and the medium placeholder is decided by the panel row proportion
    // (RowProportion::Medium).
    btn->setButtonType(s == RibbonItemSize::Large ? SARibbonToolButton::LargeButton : SARibbonToolButton::SmallButton);
}

RibbonItemSize RibbonButton::buttonSize() const
{
    return dptr()->buttonSize;
}

void RibbonButton::setStyle(RibbonButtonStyle s)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setToolButtonStyle(toQtStyle(s));
}

RibbonButtonStyle RibbonButton::style() const
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return RibbonButtonStyle::IconOnly;
    return fromQtStyle(btn->toolButtonStyle());
}

void RibbonButton::setLargeIconSize(const Size& s)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setLargeIconSize(QSize(s.x, s.y));
}

Size RibbonButton::largeIconSize() const
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return {};
    const QSize qs = btn->largeIconSize();
    return Size(qs.width(), qs.height());
}

void RibbonButton::setSmallIconSize(const Size& s)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setSmallIconSize(QSize(s.x, s.y));
}

Size RibbonButton::smallIconSize() const
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return {};
    const QSize qs = btn->smallIconSize();
    return Size(qs.width(), qs.height());
}

void RibbonButton::setWordWrap(bool on)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setEnableWordWrap(on);
}

bool RibbonButton::wordWrap() const
{
    auto* btn = impl<SARibbonToolButton>();
    return btn && btn->isEnableWordWrap();
}

void RibbonButton::setIconRightText(bool on)
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn)
        return;
    btn->setEnableIconRightText(on);
}

bool RibbonButton::iconRightText() const
{
    auto* btn = impl<SARibbonToolButton>();
    return btn && btn->isEnableIconRightText();
}

void RibbonButton::addDropDownItem(RibbonAction* item)
{
    if (!item)
        return;

    auto& entries = dptr()->entries;

    for (const auto& e : entries) {
        if (e.item == item) {
            return;
        } // already exists, ignore
    }

    entries.push_back({ item });

    rebuildMenu();
}

void RibbonButton::removeDropDownItem(RibbonAction* item)
{
    if (!item)
        return;

    auto& entries = dptr()->entries;
    auto  it      = std::find_if(entries.begin(), entries.end(), [item](const Impl::Entry& e) { return e.item == item; });

    if (it == entries.end()) {
        return;
    }

    entries.erase(it);

    // Return the item's QAction ownership back to the item (while attached to
    // the menu, the menu holds it); rebuildMenu() removes it from the menu.
    item->setOwnsImpl(true);

    rebuildMenu();
}

void RibbonButton::clearDropDownItems()
{
    auto& entries = dptr()->entries;

    if (entries.empty()) {
        return;
    }

    // Return each item's QAction ownership before clearing.
    for (const auto& e : entries) {
        if (e.item) {
            e.item->setOwnsImpl(true);
        }
    }

    entries.clear();

    rebuildMenu();
}

void RibbonButton::addSeparator()
{
    dptr()->entries.push_back({ nullptr });

    rebuildMenu();
}

size_t RibbonButton::dropDownItemCount() const
{
    // Count only real items; separators are not included
    size_t n = 0;
    for (const auto& e : dptr()->entries) {
        if (e.item) {
            ++n;
        }
    }
    return n;
}

RibbonAction* RibbonButton::dropDownItemAt(size_t i) const
{
    const auto& entries = dptr()->entries;

    size_t idx = 0;
    for (const auto& e : entries) {
        if (!e.item) {
            continue;
        }
        if (idx == i) {
            return e.item;
        }
        ++idx;
    }

    return nullptr;
}

size_t RibbonButton::dropDownEntryCount() const
{
    // Total entries: real items + separators
    return dptr()->entries.size();
}

void RibbonButton::removeDropDownEntryAt(size_t i)
{
    auto& entries = dptr()->entries;

    if (i >= entries.size()) {
        return;
    } // out-of-range guard

    auto it = entries.begin() + i;

    // Return QAction ownership when removing a real item; separators need no
    // handling (owned by this button)
    if (it->item) {
        it->item->setOwnsImpl(true);
    }

    entries.erase(it);

    rebuildMenu();
}

void RibbonButton::setData(void* data)
{
    dptr()->user = data;
}

void* RibbonButton::data() const
{
    return dptr()->user;
}

void RibbonButton::setCommand(const String& command)
{
    dptr()->command = command;
}

String RibbonButton::command() const
{
    return dptr()->command;
}

void RibbonButton::rebuildMenu()
{
    auto* btn = impl<SARibbonToolButton>();
    if (!btn) {
        return;
    }

    auto& entries = dptr()->entries;

    // Free the separator QActions built last time (created by this button, so
    // we must manage them). Though they are children of the menu, after a
    // rebuild they no longer appear in the menu's action list.
    for (QAction* s : dptr()->separators) {
        delete s;
    }
    dptr()->separators.clear();

    if (entries.empty()) {
        if (dptr()->menu) {
            btn->setMenu(nullptr);
            btn->setPopupMode(QToolButton::DelayedPopup);
        }

        return;
    }

    if (!dptr()->menu) {
        dptr()->menu = new SARibbonMenu(btn);
    }

    // Detach all current actions, then rebuild in entry order. Never use
    // QMenu::clear(): it would delete the item's QAction (the item's UIElement
    // also references it).
    for (QAction* a : dptr()->menu->actions()) {
        dptr()->menu->removeAction(a);
    }

    for (const auto& e : entries) {
        if (!e.item) {
            dptr()->separators.push_back(dptr()->menu->addSeparator());
            continue;
        }
        QAction* act = e.item->impl<QAction>();
        if (!act) {
            continue;
        }
        e.item->setOwnsImpl(false); // the menu now owns the action
        dptr()->menu->addAction(act);
    }

    btn->setMenu(dptr()->menu);
    btn->setPopupMode(QToolButton::InstantPopup);
}

inline auto RibbonButton::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RibbonButton::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
