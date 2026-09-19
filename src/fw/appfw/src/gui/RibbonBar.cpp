#include <vine/appfw/gui/RibbonBar.hpp>

#include <SARibbon.h>
#include <vine/Exception.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>

#include "ControlData.hpp"

V_APPFWGUI_NS_BEGIN

V_OBJECT_META_IMPL(RibbonBar, Control)

struct RibbonBar::Impl : public ControlData {
    std::vector<RibbonTab*> tabs;
    MainWindow*             wnd;
    QMenu*                  application_menu = nullptr;

    ~Impl()
    {
        delete application_menu;
    }
};

namespace
{

using itype = SARibbonBar;

SARibbonBar::RibbonStyles toSarRibbonStyle(RibbonStyle s)
{
    switch (s) {
    case RibbonStyle::ThreeRowLoose: return SARibbonBar::RibbonStyleLooseThreeRow;
    case RibbonStyle::ThreeRowCompact: return SARibbonBar::RibbonStyleCompactThreeRow;
    case RibbonStyle::TwoRowLoose: return SARibbonBar::RibbonStyleLooseTwoRow;
    case RibbonStyle::TwoRowCompact: return SARibbonBar::RibbonStyleCompactTwoRow;
    case RibbonStyle::SingleRowLoose: return SARibbonBar::RibbonStyleLooseSingleRow;
    case RibbonStyle::SingleRowCompact: return SARibbonBar::RibbonStyleCompactSingleRow;
    }

    return SARibbonBar::RibbonStyleLooseThreeRow;
}

RibbonStyle fromSarRibbonStyle(SARibbonBar::RibbonStyles s)
{
    if (s.testFlag(SARibbonBar::RibbonStyleSingleRow)) {
        return s.testFlag(SARibbonBar::RibbonStyleCompact) ? RibbonStyle::SingleRowCompact : RibbonStyle::SingleRowLoose;
    }
    if (s.testFlag(SARibbonBar::RibbonStyleTwoRow)) {
        return s.testFlag(SARibbonBar::RibbonStyleCompact) ? RibbonStyle::TwoRowCompact : RibbonStyle::TwoRowLoose;
    }
    return s.testFlag(SARibbonBar::RibbonStyleCompact) ? RibbonStyle::ThreeRowCompact : RibbonStyle::ThreeRowLoose;
}

} // namespace

RibbonBar::RibbonBar(MainWindow* wnd)
  : Control(new RibbonBar::Impl(), static_cast<SARibbonMainWindow*>(wnd->impl())->ribbonBar(), /*owns=*/false)
{
    dptr()->wnd              = wnd;
    dptr()->application_menu = new QMenu();
    auto app_btn             = qobject_cast<SARibbonApplicationButton*>(impl<itype>()->applicationButton());
    app_btn->setMenu(dptr()->application_menu);
}

RibbonBar::~RibbonBar()
{
    // d is deleted by UIElement::~UIElement(), do NOT delete here
}

int RibbonBar::numTabs() const
{
    return (int)dptr()->tabs.size();
}

RibbonTab* RibbonBar::tabAt(int idx) const
{
    return (idx >= 0 && idx < (int)dptr()->tabs.size()) ? dptr()->tabs[(size_t)idx] : nullptr;
}

void RibbonBar::addTab(RibbonTab* tab)
{
    V_CHECK_NULL_THROW(tab)
    if (std::any_of(dptr()->tabs.begin(), dptr()->tabs.end(), [tab](RibbonTab* t) { return tab == t; }))
        return;
    auto w = impl<itype>();
    w->addCategoryPage(tab->impl<SARibbonCategory>());
    dptr()->tabs.push_back(tab);
}

void RibbonBar::removeTab(RibbonTab* tab)
{
    V_CHECK_NULL_THROW(tab)
    if (std::none_of(dptr()->tabs.begin(), dptr()->tabs.end(), [tab](RibbonTab* t) { return t == tab; }))
        return;
    auto w = impl<itype>();
    w->removeCategory(tab->impl<SARibbonCategory>());
    dptr()->tabs.erase(std::remove(dptr()->tabs.begin(), dptr()->tabs.end(), tab), dptr()->tabs.end());
}

int RibbonBar::currentIndex() const
{
    auto w = impl<itype>();
    return w ? w->currentIndex() : -1;
}

void RibbonBar::setCurrentIndex(int idx)
{
    auto w = impl<itype>();
    w->setCurrentIndex(idx);
}

void RibbonBar::appendApplicationMenu(RibbonAction* mi)
{
    dptr()->application_menu->addAction(mi->impl<QAction>());
}

void RibbonBar::setApplicationButtonVisible(bool on)
{
    auto* bar = impl<itype>();
    if (bar && bar->applicationButton())
        bar->applicationButton()->setVisible(on);
}

bool RibbonBar::applicationButtonVisible() const
{
    auto* bar = impl<itype>();
    return bar && bar->applicationButton() && bar->applicationButton()->isVisible();
}

void RibbonBar::setApplicationIcon(const Icon& ic)
{
    auto* bar = impl<itype>();
    if (!bar)
        return;
    if (auto* btn = qobject_cast<SARibbonApplicationButton*>(bar->applicationButton()))
        btn->setIcon(ic.value());
}

Icon RibbonBar::applicationIcon() const
{
    auto* bar = impl<itype>();
    if (!bar)
        return {};
    if (auto* btn = qobject_cast<SARibbonApplicationButton*>(bar->applicationButton()))
        return Icon(btn->icon());
    return {};
}

void RibbonBar::setApplicationText(const String& t)
{
    auto* bar = impl<itype>();
    if (!bar)
        return;
    if (auto* btn = qobject_cast<SARibbonApplicationButton*>(bar->applicationButton())) {
        auto utf16 = t.toUtf16();
        btn->setText(QString::fromStdU16String(utf16));
    }
}

String RibbonBar::applicationText() const
{
    auto* bar = impl<itype>();
    if (!bar)
        return {};
    if (auto* btn = qobject_cast<SARibbonApplicationButton*>(bar->applicationButton())) {
        auto qs = btn->text();
        return String::fromUtf16((const char16_t*)qs.utf16(), qs.size());
    }
    return {};
}

void RibbonBar::addQuickAccessItem(RibbonAction* item)
{
    if (!item)
        return;
    auto* bar = impl<itype>();
    if (!bar)
        return;
    if (auto* qab = bar->quickAccessBar())
        qab->addAction(item->impl<QAction>());
}

void RibbonBar::addQuickAccessSeparator()
{
    auto* bar = impl<itype>();
    if (!bar)
        return;
    if (auto* qab = bar->quickAccessBar())
        qab->addSeparator();
}

void RibbonBar::setQuickAccessVisible(bool on)
{
    auto* bar = impl<itype>();
    if (!bar)
        return;
    if (auto* qab = bar->quickAccessBar())
        qab->setVisible(on);
}

bool RibbonBar::quickAccessVisible() const
{
    auto* bar = impl<itype>();
    return bar && bar->quickAccessBar() && bar->quickAccessBar()->isVisible();
}

void RibbonBar::setRibbonStyle(RibbonStyle s)
{
    auto w = impl<itype>();
    if (w)
        w->setRibbonStyle(toSarRibbonStyle(s));
}

RibbonStyle RibbonBar::ribbonStyle() const
{
    auto w = impl<itype>();
    if (!w)
        return RibbonStyle::ThreeRowLoose;
    return fromSarRibbonStyle(w->currentRibbonStyle());
}

void RibbonBar::setMinimumMode(bool on)
{
    auto w = impl<itype>();
    if (w)
        w->setMinimumMode(on);
}

bool RibbonBar::minimumMode() const
{
    auto w = impl<itype>();
    return w && w->isMinimumMode();
}

void RibbonBar::setPanelTitleVisible(bool on)
{
    auto w = impl<itype>();
    if (w)
        w->setEnableShowPanelTitle(on);
}

bool RibbonBar::panelTitleVisible() const
{
    auto w = impl<itype>();
    return w && w->isEnableShowPanelTitle();
}

void RibbonBar::setWordWrap(bool on)
{
    auto w = impl<itype>();
    if (w)
        w->setEnableWordWrap(on);
}

bool RibbonBar::wordWrap() const
{
    auto w = impl<itype>();
    return w && w->isEnableWordWrap();
}

void RibbonBar::setIconRightText(bool on)
{
    auto w = impl<itype>();
    if (w)
        w->setEnableIconRightText(on);
}

bool RibbonBar::iconRightText() const
{
    auto w = impl<itype>();
    return w && w->isEnableIconRightText();
}

inline auto RibbonBar::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RibbonBar::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
