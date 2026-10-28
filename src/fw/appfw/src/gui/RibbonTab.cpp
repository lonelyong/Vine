#include <vine/appfw/gui/RibbonTab.hpp>

#include <SARibbon.h>
#include <algorithm>
#include <vector>

#include <vine/Exception.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/logging/Log.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RibbonTab, Control)

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
    String                    id;
    RibbonTabRole             role   = RibbonTabRole::None;
    RibbonTabPolicy           policy = RibbonTabPolicy::None;
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

void RibbonTab::setId(const String& id)
{
    dptr()->id = id;
}

String RibbonTab::id() const
{
    return dptr()->id;
}

void RibbonTab::setRole(RibbonTabRole role)
{
    dptr()->role = role;
}

RibbonTabRole RibbonTab::role() const
{
    return dptr()->role;
}

void RibbonTab::setPolicy(RibbonTabPolicy policy)
{
    dptr()->policy = policy;
}

RibbonTabPolicy RibbonTab::policy() const
{
    return dptr()->policy;
}

void RibbonTab::addGroup(RibbonGroup* g)
{
    if (g == nullptr) {
        return; // legacy callers may pass null; the placement overload rejects it
    }

    static_cast<void>(addGroup(g, RibbonPlacement::append()));
}

RibbonPlaceResult RibbonTab::addGroup(RibbonGroup* g, const RibbonPlacement& placement)
{
    VN_CHECK_NULL_THROW(g)

    auto& groups = dptr()->groups;

    for (int i = 0; i < (int)groups.size(); ++i) {
        if (groups[(size_t)i] == g) {
            return { RibbonPlaceStatus::AlreadyAdded, i };
        }
    }

    const String id = g->id();

    if (!id.empty()) {
        for (const RibbonGroup* other : groups) {
            if (other->id() == id) {
                VN_LOGW("RibbonTab::addGroup: duplicate group id '{}', group not added", id.std_str_view());
                return { RibbonPlaceStatus::DuplicateId, -1 };
            }
        }
    }

    if (placement.where != RibbonPlacement::Where::Append) {
        // Groups anchor by id only: a role names a tab slot, not a group.
        if (placement.anchor_role != RibbonTabRole::None || placement.anchor_id.empty()) {
            VN_LOGW("RibbonTab::addGroup: a relative placement needs a group id anchor, group not added");
            return { RibbonPlaceStatus::InvalidAnchor, -1 };
        }
    }

    int               index  = (int)groups.size();
    RibbonPlaceStatus status = RibbonPlaceStatus::Placed;

    if (placement.where != RibbonPlacement::Where::Append) {
        int anchor = -1;

        for (int i = 0; i < (int)groups.size(); ++i) {
            if (groups[(size_t)i]->id() == placement.anchor_id) {
                anchor = i;
                break;
            }
        }

        if (anchor < 0) {
            status = RibbonPlaceStatus::PlacedByFallback;
        }
        else {
            index = (placement.where == RibbonPlacement::Where::Before) ? anchor : anchor + 1;
        }
    }

    auto* panel = g->impl<SARibbonPanel>();
    auto* cat   = impl<SARibbonCategory>();

    if (panel != nullptr && cat != nullptr) {
        if (index >= (int)groups.size()) {
            cat->addPanel(panel);
        }
        else {
            cat->insertPanel(panel, index);
        }
    }

    groups.insert(groups.begin() + index, g);

    return { status, index };
}

RibbonGroup* RibbonTab::findGroup(const String& id) const
{
    if (id.empty()) {
        return nullptr;
    }

    for (RibbonGroup* group : dptr()->groups) {
        if (group->id() == id) {
            return group;
        }
    }

    return nullptr;
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

VN_APPFWGUI_NS_END
