#include <vine/appfw/gui/RibbonBar.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <SARibbon.h>
#include <QToolButton>
#include <QPointer>

#include <vine/Exception.hpp>
#include <vine/appfw/gui/Backstage.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/logging/Log.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(RibbonBar, Control)

namespace
{

/// One registered tab: the logical model the physical SARibbon order is derived from.
struct TabEntry
{
    RibbonTab*        tab       = nullptr;
    RibbonPlacement   placement;
    std::uint64_t     seq       = 0; ///< Registration order: the deterministic tie-break.
    RibbonPlaceStatus status    = RibbonPlaceStatus::Placed;
};

/// @brief Whether the tab must stay in the terminal suffix.
bool isTerminal(const RibbonTab* tab)
{
    return tab != nullptr && testFlag(tab->policy(), RibbonTabPolicy::Terminal);
}

/// @brief Index of the first terminal tab, or the list size when there is none.
int terminalBegin(const std::vector<TabEntry>& placed)
{
    for (int i = 0; i < (int)placed.size(); ++i) {
        if (isTerminal(placed[i].tab)) {
            return i;
        }
    }
    return (int)placed.size();
}

/// @brief Append index for a role: role-bearing tabs sort by role value among themselves.
///
/// Role-less tabs are transparent for this scan: a role only orders against other roles.
///
/// @param placed         Already placed entries.
/// @param role           Role of the tab being placed, or None.
/// @param terminal_begin Index of the first terminal tab.
/// @return Insertion index in [0, terminal_begin].
int appendIndexForRole(const std::vector<TabEntry>& placed, RibbonTabRole role, int terminal_begin)
{
    if (role == RibbonTabRole::None) {
        return terminal_begin;
    }

    const int rank = static_cast<int>(role);

    for (int i = 0; i < terminal_begin; ++i) {
        const RibbonTab* tab = placed[(size_t)i].tab;
        if (tab->role() != RibbonTabRole::None && static_cast<int>(tab->role()) > rank) {
            return i;
        }
    }

    return terminal_begin;
}

/// @brief Where a tab goes in the current logical order, and why.
struct Resolved
{
    int               index  = -1;
    RibbonPlaceStatus status = RibbonPlaceStatus::Placed;
};

/// @brief Resolves one tab against the tabs placed before it (registration order).
///
/// Anchors only look backwards: a tab registered later cannot be an anchor, so the resolution
/// is a single pass and cannot form a cycle.
///
/// @param placed    Entries placed so far, in logical order.
/// @param tab       Tab being placed.
/// @param placement Placement request of that tab.
/// @return The insertion index and the status to report.
Resolved resolveTab(const std::vector<TabEntry>& placed, const RibbonTab* tab, const RibbonPlacement& placement)
{
    if (isTerminal(tab)) {
        // Terminal tabs form the suffix, in registration order; a relative placement is not used.
        return { (int)placed.size(), RibbonPlaceStatus::Placed };
    }

    const int terminal_begin = terminalBegin(placed);

    int               index  = terminal_begin;
    RibbonPlaceStatus status = RibbonPlaceStatus::Placed;

    if (placement.where == RibbonPlacement::Where::Append) {
        index = appendIndexForRole(placed, tab->role(), terminal_begin);
    } else if (!placement.anchor_id.empty() && placement.anchor_role != RibbonTabRole::None) {
        return { -1, RibbonPlaceStatus::InvalidAnchor };
    } else if (!placement.anchor_id.empty()) {
        int anchor = -1;

        for (int i = 0; i < (int)placed.size(); ++i) {
            if (placed[(size_t)i].tab->id() == placement.anchor_id) {
                anchor = i;
                break;
            }
        }

        if (anchor < 0) {
            status = RibbonPlaceStatus::PlacedByFallback;
            index  = terminal_begin;
        }
        else {
            index = (placement.where == RibbonPlacement::Where::Before) ? anchor : anchor + 1;
        }
    } else if (placement.anchor_role != RibbonTabRole::None) {
        int anchor = -1;

        for (int i = 0; i < (int)placed.size(); ++i) {
            if (placed[(size_t)i].tab->role() == placement.anchor_role) {
                anchor = i;
                break;
            }
        }

        if (anchor < 0) {
            status = RibbonPlaceStatus::PlacedByFallback;
            index  = appendIndexForRole(placed, placement.anchor_role, terminal_begin);
        }
        else {
            index = (placement.where == RibbonPlacement::Where::Before) ? anchor : anchor + 1;
        }
    } else {
        return { -1, RibbonPlaceStatus::InvalidAnchor };
    }

    if (index > terminal_begin) {
        index  = terminal_begin;
        status = RibbonPlaceStatus::ClampedBeforeTerminal;
    }

    return { index, status };
}

} // namespace

struct RibbonBar::Impl : public ControlData {
    std::vector<TabEntry> entries; // logical order, kept in sync with the SARibbon categories
    std::uint64_t         next_seq = 0;
    MainWindow*           wnd      = nullptr;
    QMenu*                application_menu = nullptr;
    QPointer<QToolButton> application_button;
    Backstage*            backstage            = nullptr;
    vn::Connection        backstage_connection;
    Event<RibbonBar, EventArgs> application_button_clicked;
    EventArgs                   application_button_args;

    ~Impl()
    {
        delete application_menu;
    }

    /// @brief Index of a tab in the logical order, or -1.
    int indexOf(const RibbonTab* tab) const
    {
        for (int i = 0; i < (int)entries.size(); ++i) {
            if (entries[(size_t)i].tab == tab) {
                return i;
            }
        }
        return -1;
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

    auto* bar     = impl<itype>();
    auto* app_btn = bar != nullptr ? qobject_cast<QToolButton*>(bar->applicationButton()) : nullptr;

    dptr()->application_button = app_btn;

    if (app_btn != nullptr) {
        app_btn->setMenu(dptr()->application_menu);

        // The button is the one trigger; the menu/Backstage switch is a subscriber of this event.
        QObject::connect(app_btn, &QAbstractButton::clicked, [this] { dptr()->application_button_clicked.trigger(*this, dptr()->application_button_args); });
    }
}

RibbonBar::~RibbonBar()
{
    // d is deleted by UIElement::~UIElement(), do NOT delete here
}

int RibbonBar::numTabs() const
{
    return (int)dptr()->entries.size();
}

RibbonTab* RibbonBar::tabAt(int idx) const
{
    return (idx >= 0 && idx < (int)dptr()->entries.size()) ? dptr()->entries[(size_t)idx].tab : nullptr;
}

void RibbonBar::addTab(RibbonTab* tab)
{
    VN_CHECK_NULL_THROW(tab)

    static_cast<void>(addTab(tab, RibbonPlacement::append()));
}

RibbonPlaceResult RibbonBar::addTab(RibbonTab* tab, const RibbonPlacement& placement)
{
    VN_CHECK_NULL_THROW(tab)

    const int existing = dptr()->indexOf(tab);

    if (existing >= 0) {
        return { RibbonPlaceStatus::AlreadyAdded, existing };
    }

    const String id = tab->id();

    if (!id.empty()) {
        for (const TabEntry& entry : dptr()->entries) {
            if (entry.tab->id() == id) {
                VN_LOGW("RibbonBar::addTab: duplicate tab id '{}', tab not added", id.std_str_view());
                return { RibbonPlaceStatus::DuplicateId, -1 };
            }
        }
    }

    if (placement.where != RibbonPlacement::Where::Append) {
        const bool has_id   = !placement.anchor_id.empty();
        const bool has_role = placement.anchor_role != RibbonTabRole::None;

        // Exactly one anchor identifies the sibling; both (or neither) is a caller bug.
        if (has_id == has_role) {
            VN_LOGW("RibbonBar::addTab: a relative placement needs exactly one anchor, tab not added");
            return { RibbonPlaceStatus::InvalidAnchor, -1 };
        }
    }

    auto* bar = impl<itype>();

    // The physical list still matches the logical one here, so the current index names the tab
    // to keep selected across the reorder. Capture it before the new category is appended.
    RibbonTab* keep = nullptr;

    if (bar) {
        const int current = bar->currentIndex();
        if (current >= 0 && current < (int)dptr()->entries.size()) {
            keep = dptr()->entries[(size_t)current].tab;
        }
        bar->addCategoryPage(tab->impl<SARibbonCategory>());
    }

    dptr()->entries.push_back(TabEntry{ tab, placement, dptr()->next_seq++, RibbonPlaceStatus::Placed });

    layoutTabs(keep);

    const int index = dptr()->indexOf(tab);
    return { index >= 0 ? dptr()->entries[(size_t)index].status : RibbonPlaceStatus::InvalidAnchor, index };
}

void RibbonBar::layoutTabs(RibbonTab* keep_current)
{
    auto* bar = impl<itype>();

    if (!bar) {
        return;
    }

    // Rebuild from the registration order: placement decides, registration order breaks ties.
    std::vector<TabEntry> all = dptr()->entries;

    std::sort(all.begin(), all.end(), [](const TabEntry& a, const TabEntry& b) { return a.seq < b.seq; });

    std::vector<TabEntry> ordered;
    ordered.reserve(all.size());

    for (TabEntry& entry : all) {
        const Resolved resolved = resolveTab(ordered, entry.tab, entry.placement);

        entry.status = resolved.status;

        const int index = resolved.index < 0 ? (int)ordered.size() : resolved.index;
        ordered.insert(ordered.begin() + index, entry);
    }

    dptr()->entries = std::move(ordered);

    // The logical order is the truth; SARibbon only mirrors it.
    for (int i = 0; i < (int)dptr()->entries.size(); ++i) {
        auto* category = dptr()->entries[(size_t)i].tab->impl<SARibbonCategory>();

        if (category == nullptr) {
            continue;
        }

        const int current = bar->categoryIndex(category);
        if (current >= 0 && current != i) {
            bar->moveCategory(current, i);
        }
    }

    if (keep_current != nullptr) {
        const int index = dptr()->indexOf(keep_current);
        if (index >= 0) {
            bar->setCurrentIndex(index);
        }
    }
}

void RibbonBar::removeTab(RibbonTab* tab)
{
    VN_CHECK_NULL_THROW(tab)

    const int index = dptr()->indexOf(tab);

    if (index < 0) {
        return;
    }

    auto* bar = impl<itype>();

    RibbonTab* keep = nullptr;

    if (bar) {
        const int current = bar->currentIndex();
        if (current >= 0 && current < (int)dptr()->entries.size() && dptr()->entries[(size_t)current].tab != tab) {
            keep = dptr()->entries[(size_t)current].tab;
        }
        bar->removeCategory(tab->impl<SARibbonCategory>());
    }

    dptr()->entries.erase(dptr()->entries.begin() + index);

    layoutTabs(keep);
}

RibbonTab* RibbonBar::findTab(const String& id) const
{
    if (id.empty()) {
        return nullptr;
    }

    for (const TabEntry& entry : dptr()->entries) {
        if (entry.tab->id() == id) {
            return entry.tab;
        }
    }

    return nullptr;
}

RibbonTab* RibbonBar::findTab(RibbonTabRole role) const
{
    if (role == RibbonTabRole::None) {
        return nullptr;
    }

    for (const TabEntry& entry : dptr()->entries) {
        if (entry.tab->role() == role) {
            return entry.tab;
        }
    }

    return nullptr;
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

void RibbonBar::clearApplicationMenu()
{
    if (dptr()->application_menu != nullptr) {
        dptr()->application_menu->clear();
    }
}

void RibbonBar::setBackstage(Backstage* backstage)
{
    if (dptr()->backstage == backstage) {
        return;
    }

    dptr()->backstage_connection.disconnect();
    dptr()->backstage = backstage;

    if (dptr()->application_button != nullptr) {
        // The two entry points are mutually exclusive: a Backstage opens, or the menu opens.
        dptr()->application_button->setMenu(backstage != nullptr ? nullptr : dptr()->application_menu);
    }

    if (backstage != nullptr) {
        dptr()->backstage_connection = applicationButtonClicked().connect([backstage](RibbonBar&, EventArgs&) { backstage->open(); });
    }
}

Backstage* RibbonBar::backstage() const
{
    return dptr()->backstage;
}

Event<RibbonBar, EventArgs>& RibbonBar::applicationButtonClicked()
{
    return dptr()->application_button_clicked;
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

VN_APPFWGUI_NS_END
