#include <vine/appfw/gui/CentralDocumentHost.hpp>

#include <algorithm>
#include <memory>
#include <utility>

#include <QLabel>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

/// vn::String is UTF-8, so the bytes are what Qt wants.
QString qtText(const String& text)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

/// The page a CentralDocumentHost shows when there is nothing to present: no current document, or a type without a
/// registered view. It exists so that "nothing here" is said rather than shown as an empty area.
class MessagePage final : public Control {
  public:
    explicit MessagePage(QWidget* parent = nullptr) : Control(new QWidget(parent), /*owns=*/false)
    {
        // owns=false on purpose: the container is the widget's real owner (see the class note of CentralDocumentHost).
        auto* root   = impl<QWidget>();
        auto* layout = new QVBoxLayout(root);
        label_       = new QLabel(root);
        label_->setAlignment(Qt::AlignCenter);
        label_->setWordWrap(true);
        layout->addWidget(label_);
    }

    void setText(const String& text)
    {
        if (label_ != nullptr) {
            label_->setText(qtText(text));
        }
    }

  private:
    QLabel* label_ = nullptr;
};

} // namespace

VN_OBJECT_META_IMPL(CentralDocumentHost, Control)

struct CentralDocumentHost::Impl : public ControlData {
    vn::appfw::DocumentManager* documents = nullptr;
    DocumentViewRegistry*       views     = nullptr;
    DockPanelManager*           docks     = nullptr;

    /// Subscriptions to the document manager; RAII, so destroying the host unsubscribes it.
    std::vector<vn::Connection> connections;

    /// One view per document instance, in creation order. Owned here; `closed` destroys the entry.
    std::vector<std::unique_ptr<DocumentView>> owned;

    /// The view currently on screen, or nullptr when a message page is up.
    DocumentView* shown = nullptr;

    /// What is on screen: a view's content(), or the message page. Nullptr before the host took the central area.
    UIElement* shown_element = nullptr;

    /// The page that says "nothing to show"; built on first use.
    std::unique_ptr<MessagePage> message;

    /// Set once the host installed its container into the docking layout.
    bool installed = false;

    /// Whatever occupied the central slot before (a shell's own content). Hidden, not deleted: it is not ours.
    UIElement* previous = nullptr;
};

CentralDocumentHost::CentralDocumentHost(vn::appfw::DocumentManager& documents, DocumentViewRegistry& views,
                                        DockPanelManager& docks)
  : Control(new Impl(), new QStackedWidget())
{
    auto* d    = dptr();
    d->documents = &documents;
    d->views     = &views;
    d->docks     = &docks;

    // Only `currentChanged` drives the screen: `opened` does not change the selection, and when the current document
    // goes away the manager fires currentChanged(nullptr) before `closed`.
    d->connections.push_back(documents.currentChanged.connect(
        [this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { syncToCurrentDocument(); }));

    // `closed` is where the view of that document goes: one view per instance means it dies with the instance.
    d->connections.push_back(documents.closed.connect(
        [this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
            if (args.document() != nullptr) {
                releaseView(*args.document());
            }
        }));

    syncToCurrentDocument();
}

CentralDocumentHost::~CentralDocumentHost()
{
    auto* d = dptr();

    d->connections.clear();

    // Pages are DESTROYED HERE (the objects), never by their widgets: the container below stays alive until the base
    // destructor deletes it, and a page widget dying through Qt's parenting would run UIElement's `destroyed` callback,
    // whose `delete this` would then collide with these own- ers. That is also why every page is handed over with
    // owns=false: the container owns the widgets, this host owns the view objects (see the class note).
    d->owned.clear();
    d->message.reset();

    // Give the central slot back to whoever had it: a host that is thrown away must not take the shell's area with it.
    if (d->installed && d->docks != nullptr) {
        if (auto* previous = d->previous; previous != nullptr) {
            if (auto* widget = qobject_cast<QWidget*>(previous->impl()); widget != nullptr) {
                widget->setVisible(true);
            }
            d->docks->setCentralWidget(previous);
        }
    }
    d->installed = false;
    d->previous  = nullptr;
    d->shown     = nullptr;
}

void CentralDocumentHost::syncToCurrentDocument()
{
    present(dptr()->documents != nullptr ? dptr()->documents->current() : nullptr);
}

raw_ptr<DocumentView> CentralDocumentHost::currentView() const
{
    return dptr()->shown;
}

raw_ptr<UIElement> CentralDocumentHost::shownContent() const
{
    return dptr()->shown_element;
}

raw_ptr<DocumentView> CentralDocumentHost::viewFor(const vn::appfw::Document& document) const
{
    for (const auto& view : dptr()->owned) {
        if (&view->document() == &document) {
            return view.get();
        }
    }
    return nullptr;
}

bool CentralDocumentHost::ownsCentralArea() const
{
    return dptr()->installed;
}

DocumentView* CentralDocumentHost::obtainView(vn::appfw::Document& document)
{
    if (DocumentView* existing = viewFor(document); existing != nullptr) {
        return existing;
    }

    auto* d = dptr();
    if (d->views == nullptr || !d->views->hasView(document.typeId())) {
        // No view registered for this type: do not invent one, do not fall back to another type's view.
        return nullptr;
    }

    DocumentView* created = d->views->create(document);
    if (created == nullptr) {
        return nullptr; // The factory refused this document.
    }

    // The container owns the page's widget, the host owns the view object: a view whose widget dies with the container
    // must not delete itself (UIElement's destroyed callback does exactly that when it owns the widget), because the
    // host still holds the view until the document closes.
    created->setOwnsImpl(false);

    d->owned.emplace_back(created);
    return created;
}

void CentralDocumentHost::present(vn::appfw::Document* document)
{
    if (document == nullptr) {
        setShown(nullptr);
        showMessage(u8"没有当前文档", false);
        return;
    }

    DocumentView* view = obtainView(*document);
    if (view == nullptr) {
        setShown(nullptr);

        auto* d = dptr();

        // Two different reasons, two different sentences: "nothing is registered for this type" is a registration gap,
        // "the factory refused this document" is the view's own judgement (a document it cannot present).
        const bool registered = d->views != nullptr && d->views->hasView(document->typeId());

        String text = u8"“";
        text.append(document->typeId());
        text.append(registered ? u8"” 类型的视图工厂拒绝了这份文档" : u8"” 类型没有注册视图");
        showMessage(text, true);
        return;
    }

    setShown(view);
}

void CentralDocumentHost::setShown(DocumentView* view)
{
    auto* d = dptr();
    if (d->shown == view) {
        return;
    }

    if (d->shown != nullptr) {
        d->shown->deactivate();
        d->shown = nullptr;
    }

    if (view == nullptr) {
        return;
    }

    installIntoCentralArea();

    auto* stack = impl<QStackedWidget>();
    auto* page  = view->content()->impl<QWidget>();
    if (stack != nullptr && page != nullptr) {
        if (stack->indexOf(page) < 0) {
            // First time this view is shown: take it out of whatever parent it had, so the container can own the page.
            page->setParent(nullptr);
            stack->addWidget(page);
        }
        stack->setCurrentWidget(page);
    }

    view->activate();
    d->shown         = view;
    d->shown_element = view->content();
}

void CentralDocumentHost::showMessage(const String& text, bool take_over)
{
    auto* d = dptr();

    if (!d->installed && !take_over) {
        // Nothing to show and nothing to say about a document: leave the central area to the shell.
        d->shown_element = nullptr;
        return;
    }

    installIntoCentralArea();

    if (d->message == nullptr) {
        d->message = std::make_unique<MessagePage>();
    }
    d->message->setText(text);

    auto* stack = impl<QStackedWidget>();
    auto* page  = static_cast<QWidget*>(d->message->impl());
    if (stack != nullptr && page != nullptr) {
        if (stack->indexOf(page) < 0) {
            page->setParent(nullptr);
            stack->addWidget(page);
        }
        stack->setCurrentWidget(page);
    }
    d->shown_element = d->message.get();
}

void CentralDocumentHost::releaseView(vn::appfw::Document& document)
{
    auto* d = dptr();

    const auto it = std::find_if(d->owned.begin(), d->owned.end(), [&document](const std::unique_ptr<DocumentView>& view) {
        return &view->document() == &document;
    });
    if (it == d->owned.end()) {
        return;
    }

    if (d->shown == it->get()) {
        // The manager fires currentChanged(nullptr) before `closed`, so this is the defensive path.
        d->shown->deactivate();
        d->shown         = nullptr;
        d->shown_element = nullptr;
    }

    // Out of the container first, then the view object, then its widget (which the container does not own - see
    // obtainView()): removing the widget from the stack is not enough, it would stay a hidden child of the container.
    auto* page = (*it)->content()->impl<QWidget>();
    if (auto* stack = impl<QStackedWidget>(); stack != nullptr && page != nullptr) {
        stack->removeWidget(page);
    }

    // Erasing destroys the view (and with it its camera, selection and scroll position).
    d->owned.erase(it);

    delete page;
}

void CentralDocumentHost::installIntoCentralArea()
{
    auto* d = dptr();
    if (d->installed || d->docks == nullptr) {
        return;
    }

    // Hide whatever the shell put in the central slot before installing ours. Hidden, not deleted (it is not ours), and
    // hidden rather than merely replaced: DockPanelManager::setCentralWidget() does not clear the old widget out of the
    // pane, and a native child window (a 3D view) would paint over a non-native sibling.
    if (auto* previous = d->docks->centralWidget(); previous != nullptr && previous != this) {
        if (auto* widget = qobject_cast<QWidget*>(previous->impl()); widget != nullptr) {
            widget->setVisible(false);
        }
        d->previous = previous;
    }

    d->docks->setCentralWidget(this);
    d->installed = true;
}

inline auto CentralDocumentHost::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto CentralDocumentHost::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
