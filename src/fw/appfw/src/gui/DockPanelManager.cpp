#include <vine/appfw/gui/DockPanelManager.hpp>

#include <algorithm>

#include <DockingPaneBase.h>
#include <DockingPaneContainer.h>
#include <DockingPaneManager.h>
#include <QDockWidget>
#include <QMainWindow>
#include <QSize>
#include <QUuid>

#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/UIElement.hpp>

#include "Convert.hpp"


V_APPFWGUI_NS_BEGIN

namespace
{

/// Minimal UIElement wrapper around the docking manager's root QWidget.
class DockRootWidget final : public UIElement {
  public:
    explicit DockRootWidget(UIObject* impl)
      : UIElement(impl)
    {}
};

} // namespace

struct DockPanelManager::Impl {
    std::unique_ptr<DockingPaneManager> dockingMgr;
    std::unique_ptr<UIElement>          rootElement;
};

DockPanelManager::DockPanelManager()
  : d(new Impl)
{
    d->dockingMgr = std::make_unique<DockingPaneManager>();
}

DockPanelManager::~DockPanelManager() = default;

void DockPanelManager::setWindow(UIElement* wnd)
{
    // NOTE: d->dockingMgr is created in the ctor, so the old condition
    // "if (!wnd || d->dockingMgr) return;" always returned early and the
    // main window was never registered on the DockingPaneManager.
    if (!wnd || !d->dockingMgr)
        return;

    // qobject_cast, not static_cast: a UIElement whose impl is not a QWidget must
    // not be reinterpreted as one.
    if (auto* widget = qobject_cast<QWidget*>(wnd->impl())) {
        d->dockingMgr->setMainWindow(widget);
    }
}

void DockPanelManager::setCentralWidget(UIElement* widget)
{
    if (!d->dockingMgr || !widget)
        return;

    if (auto* impl = qobject_cast<QWidget*>(widget->impl())) {
        d->dockingMgr->setClientWidget(impl);
    }
}

raw_ptr<UIElement> DockPanelManager::root() const
{
    if (!d->dockingMgr)
        return nullptr;
    if (!d->rootElement) {
        d->rootElement = std::make_unique<DockRootWidget>(d->dockingMgr->widget());
        // DockingPanes/QMainWindow owns the central widget, not the wrapper.
        d->rootElement->setOwnsImpl(false);
    }
    return d->rootElement.get();
}

DockPanel* DockPanelManager::createDockPanel(const String& title, DockAreas area)
{
    auto* panel = new DockPanel();
    if (!title.empty())
        panel->setTitle(title);
    addDockPanel(panel, area);
    return panel;
}

DockPanel* DockPanelManager::createDockPanel(const String& title, UIElement* content, DockAreas area)
{
    auto* panel = new DockPanel();
    if (!title.empty())
        panel->setTitle(title);
    if (content)
        panel->setContent(content);
    addDockPanel(panel, area);
    return panel;
}

void DockPanelManager::addDockPanel(DockPanel* panel, DockAreas area)
{
    if (!panel)
        return;

    if (!d->dockingMgr)
        return;

    auto* mgr = d->dockingMgr.get();

    auto* dp = panel->impl<DockingPaneContainer>();
    if (!dp) {
        // Keep the caller-set id; only generate a random one when unset to
        // guarantee uniqueness.
        QString panelId;
        if (panel->id().empty()) {
            panelId = QUuid::createUuid().toString();
            auto q8 = panelId.toUtf8();
            panel->setId(String(reinterpret_cast<const String::value_type*>(q8.constData()), q8.size()));
        }
        else {
            panelId = QString::fromUtf8(panel->id().data(), static_cast<int>(panel->id().size()));
        }
        auto    title        = panel->title();
        QString qtitle       = QString::fromUtf8(title.data(), static_cast<int>(title.size()));
        // Qt Debug asserts on addWidget(nullptr), so pass a temporary placeholder
        QWidget* placeholder = new QWidget();
        auto*    newPane     = mgr->createPane(panelId, qtitle, placeholder, QSize(200, 200), DockingPaneManager::dockFloat, nullptr);
        panel->attach(reinterpret_cast<UIObject*>(newPane));
        // Tag for later lookup via DockPanelManager::panels()
        auto* dpc = qobject_cast<DockingPaneContainer*>(newPane);
        if (dpc) {
            dpc->setUserData(static_cast<void*>(panel));
            // Replace placeholder with actual content if set
            if (auto* content = panel->content()) {
                auto* w = static_cast<QWidget*>(content->impl());
                if (w) {
                    w->setParent(nullptr);
                    dpc->setClientWidget(w);
                    // The temporary placeholder widget was removed from the
                    // pane layout by setClientWidget(); drop it so we do not
                    // leak one QWidget per dock panel.
                    placeholder->deleteLater();
                }
            }
        }
        dp = dpc;
    }
    if (!dp)
        return;

    DockingPaneManager::DockPosition pos;
    DockingPaneBase*                 neighbor = nullptr;

    // Convert DockAreas → DockingPaneManager::DockPosition
    if (testFlag(area, DockAreas::Right))
        pos = DockingPaneManager::dockRight;
    else if (testFlag(area, DockAreas::Top))
        pos = DockingPaneManager::dockTop;
    else if (testFlag(area, DockAreas::Bottom))
        pos = DockingPaneManager::dockBottom;
    else
        pos = DockingPaneManager::dockLeft;

    // Count existing panels in the same area
    int sameAreaCount = 0;
    for (int i = 0; i < mgr->paneCount(); ++i) {
        auto* other = mgr->pane(i);
        if (other == dp)
            continue;
        auto* oc = qobject_cast<DockingPaneContainer*>(other);
        if (!oc)
            continue;
        auto st = oc->state();
        if (st == DockingPaneBase::Docked || st == DockingPaneBase::Tabbed) {
            // Check if this panel is in the same target area via stored property
            auto areaVar = oc->property("_vine_dockarea");
            if (areaVar.isValid() && areaVar.toInt() == static_cast<int>(area)) {
                ++sameAreaCount;
                if (!neighbor)
                    neighbor = other; // first match = tab neighbour
            }
        }
    }

    if (neighbor)
        pos = DockingPaneManager::dockTab;

    mgr->dockPane(dp, pos, neighbor);

    // Remember which area this panel belongs to
    if (auto* dpc = qobject_cast<DockingPaneContainer*>(dp))
        dpc->setProperty("_vine_dockarea", static_cast<int>(area));
}

void DockPanelManager::removeDockPanel(DockPanel* panel)
{
    if (!panel)
        return;

    if (d->dockingMgr) {
        auto* dpc = panel->impl<DockingPaneContainer>();
        if (dpc) {
            d->dockingMgr->closePane(dpc);

            // If the close actually went through, the pane is now hidden and
            // no longer referenced by any tabbed container, so the library may
            // dispose of it: deletePane() schedules that. Ownership has to move with
            // it - the wrapper must stop deleting the same container, otherwise
            // UIElement::~UIElement() and the queued delete both free it.
            //
            // If the close was vetoed (onClosing() returned false), the pane is
            // still alive and visible: keep the wrapper (and the pane) untouched so
            // nothing dangles.
            if (dpc->state() != DockingPaneBase::Hidden)
                return;

            // The pane's close callback dereferences userData() as a DockPanel, and
            // closing a tab or a flyout runs it again: it must not outlive the
            // wrapper.
            dpc->setUserData(nullptr);
            panel->setOwnsImpl(false);

            d->dockingMgr->deletePane(dpc);
        }
    }
    delete panel;
}

DockPanel* DockPanelManager::findById(const String& id) const
{
    if (!d->dockingMgr)
        return nullptr;
    for (int i = 0; i < d->dockingMgr->paneCount(); ++i) {
        auto* pane = d->dockingMgr->pane(i);
        auto* dpc  = qobject_cast<DockingPaneContainer*>(pane);
        if (!dpc)
            continue;
        auto* wrapper = static_cast<DockPanel*>(dpc->userData());
        if (wrapper && wrapper->id() == id)
            return wrapper;
    }
    return nullptr;
}

DockPanel* DockPanelManager::findByTitle(const String& title) const
{
    if (!d->dockingMgr)
        return nullptr;
    for (int i = 0; i < d->dockingMgr->paneCount(); ++i) {
        auto* pane = d->dockingMgr->pane(i);
        auto* dpc  = qobject_cast<DockingPaneContainer*>(pane);
        if (!dpc)
            continue;
        auto* wrapper = static_cast<DockPanel*>(dpc->userData());
        if (wrapper && wrapper->title() == title)
            return wrapper;
    }
    return nullptr;
}

int DockPanelManager::count() const
{
    // Only panes that correspond to a real DockPanel wrapper are counted;
    // internal tabbed containers (and any pane without userData) are excluded.
    return static_cast<int>(panels().size());
}

std::vector<DockPanel*> DockPanelManager::panels() const
{
    std::vector<DockPanel*> result;
    if (!d->dockingMgr)
        return result;
    for (int i = 0; i < d->dockingMgr->paneCount(); ++i) {
        auto* pane = d->dockingMgr->pane(i);
        auto* dpc  = qobject_cast<DockingPaneContainer*>(pane);
        if (!dpc)
            continue;
        auto* wrapper = static_cast<DockPanel*>(dpc->userData());
        if (wrapper)
            result.push_back(wrapper);
    }
    return result;
}

V_APPFWGUI_NS_END
