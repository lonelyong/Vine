#include <vine/appfw/gui/RibbonGroup.hpp>

#include <QToolButton>
#include <SARibbon.h>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RibbonGroup, Control)

namespace
{

/// Item size -> panel row proportion
SARibbonPanelItem::RowProportion rowProportionFor(RibbonItemSize s)
{
    switch (s) {
    case RibbonItemSize::Large: return SARibbonPanelItem::Large;
    case RibbonItemSize::Medium: return SARibbonPanelItem::Medium;
    default: return SARibbonPanelItem::Small;
    }
}

SARibbonPanel::PanelLayoutMode toSarPanelLayoutMode(RibbonPanelLayoutMode m)
{
    switch (m) {
    case RibbonPanelLayoutMode::TwoRow: return SARibbonPanel::TwoRowMode;
    case RibbonPanelLayoutMode::SingleRow: return SARibbonPanel::SingleRowMode;
    case RibbonPanelLayoutMode::ThreeRow: break;
    }

    return SARibbonPanel::ThreeRowMode;
}

RibbonPanelLayoutMode fromSarPanelLayoutMode(SARibbonPanel::PanelLayoutMode m)
{
    switch (m) {
    case SARibbonPanel::TwoRowMode: return RibbonPanelLayoutMode::TwoRow;
    case SARibbonPanel::SingleRowMode: return RibbonPanelLayoutMode::SingleRow;
    default: break;
    }

    return RibbonPanelLayoutMode::ThreeRow;
}

} // namespace

struct RibbonGroup::Impl : public ControlData {
    String                  title;
    bool                    word_wrap   = false;
    RibbonAction*           option_item = nullptr;
    QMetaObject::Connection option_conn; // option-button QAction destruction callback
};

RibbonGroup::RibbonGroup()
  : Control(new Impl(), new SARibbonPanel())
{}

RibbonGroup::~RibbonGroup()
{
    // Disconnect the option-button QAction destruction callback so the callback
    // cannot access a freed object after the panel is destroyed.
    QObject::disconnect(dptr()->option_conn);
    // d is deleted by UIElement
}

void RibbonGroup::setTitle(const String& t)
{
    dptr()->title = t;
    auto* pnl     = impl<SARibbonPanel>();
    if (pnl) {
        auto utf16 = t.toUtf16();
        pnl->setPanelName(QString::fromStdU16String(utf16));
    }
}

String RibbonGroup::title() const
{
    return dptr()->title;
}

void RibbonGroup::addButton(RibbonButton* b)
{
    if (!b)
        return;
    auto  w   = b->impl<QWidget>();
    auto* pnl = impl<SARibbonPanel>();
    if (w && pnl)
        pnl->addWidget(w, rowProportionFor(b->buttonSize()));
}

void RibbonGroup::removeButton(RibbonButton* b)
{
    if (!b)
        return;
    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return;
    auto* w = b->impl<QWidget>();
    if (w) {
        auto* action = w->findChild<QAction*>(QString(), Qt::FindDirectChildrenOnly);
        if (!action) {
            auto* tb = qobject_cast<QToolButton*>(w);
            if (tb)
                action = tb->defaultAction();
        }
        if (action)
            pnl->removeAction(action);
        w->setParent(nullptr);
        w->deleteLater();
    }
}

void RibbonGroup::addControl(Control* w, RibbonItemSize size)
{
    if (!w)
        return;
    auto  wgt = w->impl<QWidget>();
    auto* pnl = impl<SARibbonPanel>();
    if (wgt && pnl)
        pnl->addWidget(wgt, rowProportionFor(size));
}

void RibbonGroup::removeControl(Control* w)
{
    if (!w)
        return;
    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return;
    auto* wgt = w->impl<QWidget>();
    if (!wgt)
        return;

    auto* action = wgt->findChild<QAction*>(QString(), Qt::FindDirectChildrenOnly);
    if (!action) {
        if (auto* tb = qobject_cast<QToolButton*>(wgt))
            action = tb->defaultAction();
    }
    if (action)
        pnl->removeAction(action);

    wgt->setParent(nullptr);
    wgt->deleteLater();
}

void RibbonGroup::addSeparator()
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->addSeparator();
}

void RibbonGroup::setLayoutMode(RibbonPanelLayoutMode m)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setPanelLayoutMode(toSarPanelLayoutMode(m));
}

RibbonPanelLayoutMode RibbonGroup::layoutMode() const
{
    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return RibbonPanelLayoutMode::ThreeRow;
    return fromSarPanelLayoutMode(pnl->panelLayoutMode());
}

void RibbonGroup::setExpanding(bool on)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setExpanding(on);
}

bool RibbonGroup::expanding() const
{
    auto* pnl = impl<SARibbonPanel>();
    return pnl && pnl->isExpanding();
}

void RibbonGroup::setCanCustomize(bool on)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setCanCustomize(on);
}

bool RibbonGroup::canCustomize() const
{
    auto* pnl = impl<SARibbonPanel>();
    return pnl && pnl->isCanCustomize();
}

void RibbonGroup::setLargeIconSize(const Size& s)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setLargeIconSize(QSize(s.x, s.y));
}

Size RibbonGroup::largeIconSize() const
{
    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return {};
    const QSize qs = pnl->largeIconSize();
    return Size(qs.width(), qs.height());
}

void RibbonGroup::setSmallIconSize(const Size& s)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setSmallIconSize(QSize(s.x, s.y));
}

Size RibbonGroup::smallIconSize() const
{
    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return {};
    const QSize qs = pnl->smallIconSize();
    return Size(qs.width(), qs.height());
}

void RibbonGroup::setIconRightText(bool on)
{
    auto* pnl = impl<SARibbonPanel>();
    if (pnl)
        pnl->setEnableIconRightText(on);
}

bool RibbonGroup::iconRightText() const
{
    auto* pnl = impl<SARibbonPanel>();
    return pnl && pnl->isEnableIconRightText();
}

void RibbonGroup::setWordWrap(bool on)
{
    // SARibbonPanel::setEnableWordWrap is protected (for SARibbonBar/Category
    // synchronization only), so the panel layer cannot call it directly; instead
    // iterate over the buttons and set them one by one.
    dptr()->word_wrap = on;

    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return;

    const auto btns = pnl->ribbonToolButtons();
    for (auto* b : btns) b->setEnableWordWrap(on);
}

bool RibbonGroup::wordWrap() const
{
    return dptr()->word_wrap;
}

void RibbonGroup::setOptionAction(RibbonAction* item)
{
    // Disconnect the old callback to avoid duplicate connections
    if (dptr()->option_conn) {
        QObject::disconnect(dptr()->option_conn);
        dptr()->option_conn = {};
    }
    dptr()->option_item = item;

    auto* pnl = impl<SARibbonPanel>();
    if (!pnl)
        return;

    QAction* act = item ? item->impl<QAction>() : nullptr;
    pnl->setOptionAction(act);

    // When the item is destroyed, clear the panel's option button to avoid a
    // dangling pointer.
    if (act) {
        dptr()->option_conn = QObject::connect(act, &QObject::destroyed, [this, pnl](QObject*) {
            if (dptr()->option_item) {
                dptr()->option_item = nullptr;
                if (pnl)
                    pnl->setOptionAction(nullptr);
            }
        });
    }
}

raw_ptr<RibbonAction> RibbonGroup::optionAction() const
{
    return dptr()->option_item;
}

inline auto RibbonGroup::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RibbonGroup::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
