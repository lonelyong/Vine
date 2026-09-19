#include <vine/appfw/gui/RibbonTab.hpp>

#include <SARibbon.h>
#include <algorithm>
#include <vector>
#include <vine/appfw/gui/RibbonGroup.hpp>

#include "ControlData.hpp"

V_APPFWGUI_NS_BEGIN

V_OBJECT_META_IMPL(RibbonTab, Control)

namespace
{

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

struct RibbonTab::Impl : public ControlData {
    String                    title;
    std::vector<RibbonGroup*> groups; // added groups (framework bookkeeping, for queries)
};

RibbonTab::RibbonTab()
  : Control(new Impl(), new SARibbonCategory(QString()))
{}

RibbonTab::~RibbonTab()
{
    // d is deleted by UIElement
}

void RibbonTab::setTitle(const String& t)
{
    dptr()->title = t;
    auto* cat     = impl<SARibbonCategory>();
    if (cat) {
        auto utf16 = t.toUtf16();
        cat->setCategoryName(QString::fromStdU16String(utf16));
    }
}

String RibbonTab::title() const
{
    return dptr()->title;
}

void RibbonTab::addGroup(RibbonGroup* g)
{
    if (!g)
        return;
    auto  p   = g->impl<SARibbonPanel>();
    auto* cat = impl<SARibbonCategory>();
    if (p && cat)
        cat->addPanel(p);
    // Bookkeeping (dedupe)
    if (std::find(dptr()->groups.begin(), dptr()->groups.end(), g) == dptr()->groups.end())
        dptr()->groups.push_back(g);
}

void RibbonTab::removeGroup(RibbonGroup* g)
{
    if (!g)
        return;
    auto  p   = g->impl<SARibbonPanel>();
    auto* cat = impl<SARibbonCategory>();
    if (p && cat)
        cat->removePanel(p);
    dptr()->groups.erase(std::remove(dptr()->groups.begin(), dptr()->groups.end(), g), dptr()->groups.end());
}

int RibbonTab::numGroups() const
{
    return (int)dptr()->groups.size();
}

RibbonGroup* RibbonTab::groupAt(int i) const
{
    return (i >= 0 && i < (int)dptr()->groups.size()) ? dptr()->groups[(size_t)i] : nullptr;
}

void RibbonTab::setPanelLayoutMode(RibbonPanelLayoutMode m)
{
    auto* cat = impl<SARibbonCategory>();
    if (cat)
        cat->setPanelLayoutMode(toSarPanelLayoutMode(m));
}

RibbonPanelLayoutMode RibbonTab::panelLayoutMode() const
{
    auto* cat = impl<SARibbonCategory>();
    if (!cat)
        return RibbonPanelLayoutMode::ThreeRow;
    return fromSarPanelLayoutMode(cat->panelLayoutMode());
}

void RibbonTab::setPanelTitleVisible(bool on)
{
    auto* cat = impl<SARibbonCategory>();
    if (cat)
        cat->setEnableShowPanelTitle(on);
}

bool RibbonTab::panelTitleVisible() const
{
    auto* cat = impl<SARibbonCategory>();
    return cat && cat->isEnableShowPanelTitle();
}

void RibbonTab::setPanelSpacing(int n)
{
    auto* cat = impl<SARibbonCategory>();
    if (cat)
        cat->setPanelSpacing(n);
}

int RibbonTab::panelSpacing() const
{
    auto* cat = impl<SARibbonCategory>();
    return cat ? cat->panelSpacing() : 0;
}

inline auto RibbonTab::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto RibbonTab::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
