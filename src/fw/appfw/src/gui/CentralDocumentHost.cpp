#include <vine/appfw/gui/CentralDocumentHost.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
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

/**
 * @brief The window a floated document lives in: a title bar, a dock button and the view's page.
 *
 * A plain top-level QWidget, NOT a DockingPane: the docking manager never sees it, which is what keeps a floated
 * document from being dropped on an edge, an auto-hide strip or another pane (see the class note of
 * CentralDocumentHost).
 *
 * OWNERSHIP: the window owns the page while it is inside. takePage() detaches it first, so a caller that destroys the
 * window (document closed, host destroyed) still owns - and deletes - the page itself.
 */
class FloatingDocumentWindow final : public QWidget {
  public:
    /**
     * @brief Builds the window around an empty page slot.
     *
     * @param title    Text for the title bar.
     * @param on_dock  Called when the dock button is pressed.
     * @param on_close Called when the user asks to close the window; the callback decides, the window does not.
     */
    FloatingDocumentWindow(const QString& title, std::function<void()> on_dock, std::function<void()> on_close)
      : QWidget(nullptr, Qt::Window)
      , on_dock_(std::move(on_dock))
      , on_close_(std::move(on_close))
    {
        auto* root   = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto* bar        = new QWidget(this);
        auto* bar_layout = new QHBoxLayout(bar);
        bar_layout->setContentsMargins(4, 2, 4, 2);

        title_ = new QLabel(title, bar);
        bar_layout->addWidget(title_, 1);

        auto* dock = new QToolButton(bar);
        dock->setText(QStringLiteral("Dock"));
        dock->setToolTip(QStringLiteral("Dock this document back into the central area"));
        bar_layout->addWidget(dock);

        auto* close_button = new QToolButton(bar);
        close_button->setText(QStringLiteral("Close"));
        close_button->setToolTip(QStringLiteral("Close this document"));
        bar_layout->addWidget(close_button);

        root->addWidget(bar);

        page_host_   = new QWidget(this);
        page_layout_ = new QVBoxLayout(page_host_);
        page_layout_->setContentsMargins(0, 0, 0, 0);
        root->addWidget(page_host_, 1);

        connect(dock, &QToolButton::clicked, this, [this] {
            if (on_dock_ != nullptr) {
                on_dock_();
            }
        });
        connect(close_button, &QToolButton::clicked, this, [this] { close(); });
    }

    /**
     * @brief Takes a page into the window.
     *
     * @param page Page widget; the caller detaches it from its previous parent first.
     */
    void setPage(QWidget* page)
    {
        page_layout_->addWidget(page);
        page_ = page;
    }

    /**
     * @brief Detaches the page and leaves it parentless.
     *
     * @return The page that was inside, or nullptr when there was none.
     */
    QWidget* takePage()
    {
        if (page_ == nullptr) {
            return nullptr;
        }

        page_layout_->removeWidget(page_);
        page_->setParent(nullptr);

        QWidget* const page = page_;
        page_               = nullptr;
        return page;
    }

    /**
     * @brief Replaces the title bar text (a document's title may change).
     *
     * @param title New title.
     */
    void setTitle(const QString& title)
    {
        title_->setText(title);
    }

    /**
     * @brief Sets what happens when the window becomes the active one (focus follows the document).
     *
     * @param on_activate Callback; may be empty.
     */
    void setOnActivate(std::function<void()> on_activate)
    {
        on_activate_ = std::move(on_activate);
    }

  protected:
    void closeEvent(QCloseEvent* event) override
    {
        // The DOCUMENT decides whether it may close (canClose), not the window: route the X through the manager and
        // cancel the widget close, so a veto leaves the window where it is and `closed` tears it down afterwards.
        if (on_close_ != nullptr) {
            event->ignore();

            // Deferred on purpose: a document that closes destroys this window, and deleting a widget inside its own
            // closeEvent is not safe (Qt keeps using it after the handler returns). The context object cancels the
            // timer if the window goes away first.
            QTimer::singleShot(0, this, [this] {
                if (on_close_ != nullptr) {
                    on_close_();
                }
            });
            return;
        }
        QWidget::closeEvent(event);
    }

    void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);

        if (event->type() == QEvent::ActivationChange && isActiveWindow() && on_activate_ != nullptr) {
            on_activate_();
        }
    }

  private:
    QLabel*               title_       = nullptr;
    QWidget*              page_host_   = nullptr;
    QVBoxLayout*          page_layout_ = nullptr;
    QWidget*              page_        = nullptr;
    std::function<void()> on_dock_;
    std::function<void()> on_close_;
    std::function<void()> on_activate_;
};

} // namespace

VN_OBJECT_META_IMPL(CentralDocumentHost, Control)

struct CentralDocumentHost::Impl : public ControlData {
    /// One open document: its tab, its view (built lazily) and, while floated, the window holding its page.
    struct Entry
    {
        vn::appfw::Document*                   document = nullptr;
        std::unique_ptr<DocumentView>          view;
        std::unique_ptr<FloatingDocumentWindow> window;

        /// @return The page widget of the view, or nullptr when the view has none / was never built.
        QWidget* page() const
        {
            if (view == nullptr) {
                return nullptr;
            }
            return view->content() != nullptr ? view->content()->impl<QWidget>() : nullptr;
        }
    };

    vn::appfw::DocumentManager* documents = nullptr;
    DocumentViewRegistry*       views     = nullptr;
    DockPanelManager*           docks     = nullptr;

    /// Subscriptions to the document manager; RAII, so destroying the host unsubscribes it.
    std::vector<vn::Connection> connections;

    /// One entry per open document, in tab order. Owns the view objects and the floating windows.
    std::vector<Entry> entries;

    /// The tab strip and the page container it switches; both children of the host's root widget.
    QTabBar*        tabs  = nullptr;
    QStackedWidget* stack = nullptr;

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

    /**
     * @brief Finds the entry of a document.
     *
     * @param document Document to look up.
     * @return The entry, or nullptr when that document is not open.
     */
    Entry* findEntry(const vn::appfw::Document& document) noexcept
    {
        for (Entry& entry : entries) {
            if (entry.document == &document) {
                return &entry;
            }
        }
        return nullptr;
    }

    /**
     * @brief Finds the tab position of a document.
     *
     * @param document Document to look up; may be null ("no document").
     * @return The index, or -1 when it has no tab.
     */
    int indexOf(const vn::appfw::Document* document) const noexcept
    {
        if (document == nullptr) {
            return -1;
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].document == document) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /// Adds a tab for a document that just entered the open set (no view is built here: views stay lazy).
    void addTab(vn::appfw::Document& document)
    {
        if (tabs == nullptr || findEntry(document) != nullptr) {
            return;
        }

        entries.push_back(Entry{ &document, nullptr, nullptr });

        // Blocked: QTabBar emits currentChanged for its own bookkeeping, and a tab added to an empty strip becomes
        // selected - which must not silently change the manager's current document (create() does not select).
        const QSignalBlocker blocker(tabs);
        tabs->addTab(qtText(document.title()));
        tabs->setVisible(entries.size() > 1);

        // Keep the strip showing the manager's current document rather than whatever QTabBar picked.
        syncTabSelection(documents != nullptr ? documents->current() : nullptr);
    }

    /// Removes a document's tab, its view and its floating window.
    void removeTab(vn::appfw::Document& document)
    {
        const int index = indexOf(&document);
        if (index < 0) {
            return;
        }

        Entry& entry = entries[static_cast<std::size_t>(index)];

        if (shown != nullptr && shown == entry.view.get()) {
            // The manager fires currentChanged(nullptr) before `closed`; this is the defensive path.
            shown->deactivate();
            shown         = nullptr;
            shown_element = nullptr;
        }

        if (entry.window != nullptr) {
            // Take the page out first: the window owns its children, and the page is deleted by releaseEntry().
            entry.window->takePage();
            entry.window.reset();
        }

        releaseEntry(entry);

        {
            const QSignalBlocker blocker(tabs);
            if (tabs != nullptr) {
                tabs->removeTab(index);
                tabs->setVisible(entries.size() > 1);
            }
        }

        entries.erase(entries.begin() + index);
    }

    /// Destroys the view and the page of an entry (the page is a widget, so it is deleted explicitly).
    void releaseEntry(Entry& entry)
    {
        QWidget* const page = entry.page();

        if (stack != nullptr && page != nullptr && stack->indexOf(page) >= 0) {
            stack->removeWidget(page);
        }

        // Erasing destroys the view (and with it its camera, selection and scroll position).
        entry.view.reset();

        delete page;
    }

    /// Selects the tab of a document (or clears the selection when it has none) without feeding the signal back.
    void syncTabSelection(const vn::appfw::Document* document)
    {
        if (tabs == nullptr) {
            return;
        }

        const int index = indexOf(document);
        if (tabs->currentIndex() == index) {
            return;
        }

        const QSignalBlocker blocker(tabs);
        tabs->setCurrentIndex(index);
    }
};

CentralDocumentHost::CentralDocumentHost(vn::appfw::DocumentManager& documents, DocumentViewRegistry& views,
                                        DockPanelManager& docks)
  : Control(new Impl(), new QWidget())
{
    auto* d      = dptr();
    d->documents = &documents;
    d->views     = &views;
    d->docks     = &docks;

    // The host's root is a container, not the page stack itself: the tab strip sits above it, and both are created
    // once (switching pages inside the stack never reparents the host - see contract 1).
    auto* root   = impl<QWidget>();
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    d->tabs = new QTabBar(root);
    d->tabs->setDocumentMode(true);
    d->tabs->setExpanding(false);
    d->tabs->setMovable(true);
    d->tabs->setTabsClosable(true);
    d->tabs->setVisible(false);
    layout->addWidget(d->tabs);

    d->stack = new QStackedWidget(root);
    layout->addWidget(d->stack, 1);

    // A tab selection IS a document selection: the manager stays the single owner of "which document". The
    // connection lives as long as the host's root widget (the host itself is not a QObject).
    QObject::connect(d->tabs, &QTabBar::currentChanged, root, [this](int index) {
        auto* d = dptr();
        if (d->documents == nullptr || index < 0 || static_cast<std::size_t>(index) >= d->entries.size()) {
            return;
        }

        vn::appfw::Document* document = d->entries[static_cast<std::size_t>(index)].document;
        if (document != nullptr && d->documents->current() != document) {
            d->documents->setCurrent(document);
        }
    });

    // A close button on a tab is "close this document", not "remove a widget": canClose() gets its say.
    QObject::connect(d->tabs, &QTabBar::tabCloseRequested, root, [this](int index) {
        auto* d = dptr();
        if (d->documents == nullptr || index < 0 || static_cast<std::size_t>(index) >= d->entries.size()) {
            return;
        }
        if (vn::appfw::Document* document = d->entries[static_cast<std::size_t>(index)].document; document != nullptr) {
            d->documents->close(document);
        }
    });

    // Moving a tab reorders the strip only; entries (and therefore the tab <-> document mapping) move with it.
    QObject::connect(d->tabs, &QTabBar::tabMoved, root, [this](int from, int to) {
        auto* d = dptr();
        if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= d->entries.size()
            || static_cast<std::size_t>(to) >= d->entries.size()) {
            return;
        }

        Impl::Entry moved = std::move(d->entries[static_cast<std::size_t>(from)]);
        d->entries.erase(d->entries.begin() + from);
        d->entries.insert(d->entries.begin() + to, std::move(moved));
    });

    // `opened` adds the tab (it does not select: create() leaves the current document alone).
    d->connections.push_back(documents.opened.connect(
        [this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
            if (args.document() != nullptr) {
                dptr()->addTab(*args.document());
            }
        }));

    // `currentChanged` drives the screen and the strip: selecting a document shows it and selects its tab.
    d->connections.push_back(documents.currentChanged.connect(
        [this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { syncToCurrentDocument(); }));

    // `closed` is where the view and the tab of that document go: one view per instance means it dies with it.
    d->connections.push_back(documents.closed.connect(
        [this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
            if (args.document() != nullptr) {
                dptr()->removeTab(*args.document());
            }
        }));

    // Documents that were already open when this host was built get their tabs now.
    for (vn::appfw::Document* document : documents.documents()) {
        if (document != nullptr) {
            d->addTab(*document);
        }
    }

    syncToCurrentDocument();
}

CentralDocumentHost::~CentralDocumentHost()
{
    auto* d = dptr();

    d->connections.clear();

    // Pages are DESTROYED HERE (the objects), never by their widgets: the container below stays alive until the base
    // destructor deletes it, and a page widget dying through Qt's parenting would run UIElement's `destroyed` callback,
    // whose `delete this` would then collide with these owners. That is also why every page is handed over with
    // owns=false: the container owns the widgets, this host owns the view objects (see the class note).
    for (Impl::Entry& entry : d->entries) {
        if (entry.window != nullptr) {
            entry.window->takePage();
            entry.window.reset();
        }
        d->releaseEntry(entry);
    }
    d->entries.clear();
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
    for (const auto& entry : dptr()->entries) {
        if (entry.document == &document) {
            return entry.view.get();
        }
    }
    return nullptr;
}

bool CentralDocumentHost::ownsCentralArea() const
{
    return dptr()->installed;
}

int CentralDocumentHost::documentCount() const
{
    return static_cast<int>(dptr()->entries.size());
}

vn::appfw::Document* CentralDocumentHost::documentAt(int index) const
{
    const auto* d = dptr();
    if (index < 0 || static_cast<std::size_t>(index) >= d->entries.size()) {
        return nullptr;
    }
    return d->entries[static_cast<std::size_t>(index)].document;
}

bool CentralDocumentHost::isFloating(const vn::appfw::Document& document) const
{
    for (const auto& entry : dptr()->entries) {
        if (entry.document == &document) {
            return entry.window != nullptr;
        }
    }
    return false;
}

void CentralDocumentHost::setFloating(vn::appfw::Document& document, bool floating)
{
    auto* d = dptr();

    Impl::Entry* entry = d->findEntry(document);
    if (entry == nullptr || (entry->window != nullptr) == floating) {
        return;
    }

    // The callbacks outlive this call, so they name the document by pointer: the reference parameter is only borrowed
    // for the duration of this function.
    vn::appfw::Document* const target = &document;

    if (floating) {
        DocumentView* view = obtainView(document);
        if (view == nullptr) {
            return; // No view registered (or the factory refused): there is no page to move, so nothing floats.
        }

        QWidget* const page = entry->page();
        if (page == nullptr) {
            return;
        }

        installIntoCentralArea();

        auto window = std::make_unique<FloatingDocumentWindow>(
            qtText(document.title()),
            [this, target] { setFloating(*target, false); },
            [this, target] {
                if (auto* documents = dptr()->documents; documents != nullptr) {
                    documents->close(target); // canClose() gets its say; a veto leaves the window alone
                }
            });

        // Focus follows the document: working in a floated window makes it the document commands act on.
        window->setOnActivate([this, target] {
            auto* documents = dptr()->documents;
            if (documents != nullptr && documents->current() != target) {
                documents->setCurrent(target);
            }
        });

        // One parent at a time: out of the page stack, into the window.
        if (d->stack != nullptr && d->stack->indexOf(page) >= 0) {
            d->stack->removeWidget(page);
        }
        page->setParent(nullptr);
        window->setPage(page);
        window->resize(page->sizeHint().expandedTo(QSize(320, 240)));
        window->show();

        entry->window = std::move(window);
        return;
    }

    // Dock back: the page returns to the central stack, at its tab position. It becomes the visible page only when the
    // document is the current one.
    const std::unique_ptr<FloatingDocumentWindow> window = std::move(entry->window);
    QWidget* const                                page   = window->takePage();

    if (page != nullptr) {
        page->setParent(nullptr);
        if (d->stack != nullptr) {
            d->stack->addWidget(page);
        }
    }

    if (d->shown == entry->view.get() && d->stack != nullptr && page != nullptr) {
        d->stack->setCurrentWidget(page);
    }
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

    // Defensive: a document that entered the open set before this host existed still gets its tab.
    if (d->findEntry(document) == nullptr) {
        d->addTab(document);
    }
    Impl::Entry* const entry = d->findEntry(document);
    if (entry == nullptr) {
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

    entry->view.reset(created);
    return created;
}

void CentralDocumentHost::present(vn::appfw::Document* document)
{
    // The strip follows the manager, never the other way round: selecting a tab calls setCurrent(), which lands here.
    dptr()->syncTabSelection(document);

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
        if (view != nullptr) {
            showView(*view); // Re-selecting the current tab still brings its page/window to the front.
        }
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
    showView(*view);

    view->activate();
    d->shown         = view;
    d->shown_element = view->content();
}

void CentralDocumentHost::showView(DocumentView& view)
{
    auto* d = dptr();

    Impl::Entry* const entry = d->findEntry(view.document());
    if (entry == nullptr) {
        return;
    }

    if (entry->window != nullptr) {
        // Floated: the page lives in its own window, so bring that window up instead of touching the stack. The central
        // area keeps showing the last docked page - "one visible view" is a per-window rule, not a per-application one.
        entry->window->raise();
        entry->window->activateWindow();
        return;
    }

    QWidget* const page = entry->page();
    if (d->stack == nullptr || page == nullptr) {
        return;
    }

    if (d->stack->indexOf(page) < 0) {
        // First time this view is shown: take it out of whatever parent it had, so the container can own the page.
        page->setParent(nullptr);
        d->stack->addWidget(page);
    }
    d->stack->setCurrentWidget(page);
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

    auto* stack = d->stack;
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
