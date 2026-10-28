#include <vine/appfw/gui/CentralDocumentHost.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QAction>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

/// vn::String is UTF-8, so the bytes are what Qt wants.
QString qtText(const String& text)
{
    return QString::fromUtf8(reinterpret_cast<const char*>(text.data()), static_cast<int>(text.size()));
}

/// The page a CentralDocumentHost shows when the central area cannot show the current document: no current document,
/// a type without a registered view, or a document that is floating in its own window. It exists so that "nothing
/// here" is explained rather than shown as an empty area.
class MessagePage final : public Control {
  public:
    explicit MessagePage(QWidget* parent = nullptr)
      : Control(new QWidget(parent), /*owns=*/false)
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
 * @brief The strip itself: a QTabBar whose sheet uses palette roles, which Qt resolves when the sheet is polished.
 *
 * A theme switch changes the application palette (GuiApplication::applyTheme() calls QApplication::setPalette()), and
 * that does NOT re-resolve an already-polished sheet: a dark switch would leave the strip light until the next start.
 * The change arrives as a PaletteChange event, so the strip re-applies its own sheet there - which also refreshes the
 * close buttons, because QWidget::setStyleSheet repolishes the whole subtree. The re-apply emits PaletteChange itself,
 * hence the guard; the widget's own palette is NOT used to decide (a styled widget keeps the one polish gave it - it is
 * the application palette that moved).
 */
class DocumentTabBar final : public QTabBar {
  public:
    explicit DocumentTabBar(QWidget* parent = nullptr)
      : QTabBar(parent)
    {}

  protected:
    void changeEvent(QEvent* event) override
    {
        QTabBar::changeEvent(event);

        // Both forms count: Qt sends ApplicationPaletteChange to the application/top-levels and PaletteChange to the
        // rest, and which one a widget sees depends on where it sits.
        const bool palette_changed = event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange;
        if (!palette_changed || repolishing_) {
            return;
        }

        repolishing_ = true;
        setStyleSheet(styleSheet());
        repolishing_ = false;
    }

  private:
    bool repolishing_ = false;
};

/**
 * @brief The close button on a tab: a flat "×" instead of the platform style's boxed indicator.
 *
 * Fusion (the style the app runs on) draws the stock close indicator as a second little widget next to the title, and
 * colours it red on the selected tab. A text button in the tab's own colours reads as part of the title instead.
 */
class TabCloseButton final : public QToolButton {
  public:
    explicit TabCloseButton(QWidget* parent)
      : QToolButton(parent)
    {
        setObjectName(QStringLiteral("vnTabCloseButton"));
        setText(QStringLiteral("×"));
        setAutoRaise(true);
        setCursor(Qt::ArrowCursor);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(QStringLiteral("关闭这个文档"));
        setFixedSize(16, 16);
    }
};

/**
 * @brief The window a floated document lives in: a title bar, a dock button and the view's page.
 *
 * A plain top-level QWidget, NOT a DockingPane: the docking manager never sees it, which is what keeps a floated
 * document from being dropped on an edge, an auto-hide strip or another pane (see the class note of
 * CentralDocumentHost).
 *
 * ACTIVATION IS REPORTED, NOT INTERPRETED: setOnActivate() says "this window became the active one"; what that means
 * for the document (it becomes current) is the host's policy, not the window's.
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
        // The OS decoration and the bar below both name the document: the window is what the user finds in the task
        // bar, so it needs a title of its own, not just a label inside it.
        setWindowTitle(title);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);

        auto* bar        = new QWidget(this);
        auto* bar_layout = new QHBoxLayout(bar);
        bar_layout->setContentsMargins(4, 2, 4, 2);

        title_ = new QLabel(title, bar);
        bar_layout->addWidget(title_, 1);

        auto* dock = new QToolButton(bar);
        dock->setText(QStringLiteral("停靠"));
        dock->setToolTip(QStringLiteral("把这个文档停靠回中央区"));
        bar_layout->addWidget(dock);

        auto* close_button = new QToolButton(bar);
        close_button->setText(QStringLiteral("关闭"));
        close_button->setToolTip(QStringLiteral("关闭这个文档"));
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
     * @brief Replaces the title of the bar and of the window itself (a document's title may change).
     *
     * @param title New title.
     */
    void setTitle(const QString& title)
    {
        title_->setText(title);
        setWindowTitle(title);
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
    struct Entry {
        vn::appfw::Document*                    document = nullptr;
        std::unique_ptr<DocumentView>           view;
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

    /// The view whose activate() is in effect - the current document's view. Null while no current view is presented.
    DocumentView* current_view = nullptr;

    /// What the central area shows: the current view's content(), or the message page. Null before installation.
    UIElement* central_page = nullptr;

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
        const int            index = tabs->addTab(qtText(document.title()));

        // The tab bar owns the button and deletes it with the tab, so the callback may name the document by pointer:
        // a closed document takes its button (and this connection) with it.
        auto* const                close  = new TabCloseButton(tabs);
        vn::appfw::Document* const target = &document;
        QObject::connect(close, &QToolButton::clicked, tabs, [this, target] {
            if (documents != nullptr) {
                documents->close(target); // canClose() gets its say
            }
        });
        tabs->setTabButton(index, QTabBar::RightSide, close);

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

        if (current_view != nullptr && current_view == entry.view.get()) {
            // The manager fires currentChanged(nullptr) before `closed`; this is the defensive path.
            current_view->deactivate();
            current_view = nullptr;
            central_page = nullptr;
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
            }
        }

        entries.erase(entries.begin() + index);

        if (tabs == nullptr) {
            return;
        }

        // A TAB STRIP CANNOT SAY "NOTHING IS SELECTED": QTabBar always keeps one tab current (measured on Qt 6.10 -
        // setCurrentIndex(-1) is a no-op while tabs exist), so a manager left without a current document would make the
        // strip claim a document the central area refuses to show. Closing therefore asks the manager for the tab that
        // took the closed one's place - the same REQUEST every other gesture makes, so the manager stays the one owner
        // (R7) and the three surfaces keep agreeing (N8). Explicitly setting no current document is still allowed; it
        // is an application policy the strip cannot express, not something this host second-guesses.
        if (documents != nullptr && documents->current() == nullptr && !entries.empty()) {
            const std::size_t replacement = std::min(static_cast<std::size_t>(index), entries.size() - 1);
            documents->setCurrent(entries[replacement].document);
        }

        syncTabSelection(documents != nullptr ? documents->current() : nullptr);
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

    /// Keeps every surface that shows a title in step with the document: the tab text and a floating window's bar.
    void syncTabTitles()
    {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            Entry& entry = entries[i];
            if (entry.document == nullptr) {
                continue;
            }

            const QString title = qtText(entry.document->title());
            if (tabs != nullptr) {
                const int index = static_cast<int>(i);
                if (tabs->tabText(index) != title) {
                    tabs->setTabText(index, title);
                }
            }
            if (entry.window != nullptr) {
                entry.window->setTitle(title);
            }
        }
    }

    /// @return The one message page, built on first use.
    QWidget* messagePage()
    {
        if (message == nullptr) {
            message = std::make_unique<MessagePage>();
        }
        return static_cast<QWidget*>(message->impl());
    }

    /// Puts a page (a view's content or the message page) up, taking it into the stack the first time.
    ///
    /// @param page Page to show; ignored when there is no stack.
    void showPage(QWidget* page)
    {
        if (stack == nullptr || page == nullptr) {
            return;
        }
        if (stack->indexOf(page) < 0) {
            page->setParent(nullptr);
            stack->addWidget(page);
        }
        stack->setCurrentWidget(page);
    }

    /// Puts the message page up and says @p text. Only called once the central area is the host's.
    ///
    /// @param text What to say (UTF-8).
    void showMessage(const String& text)
    {
        QWidget* const page = messagePage();
        message->setText(text);

        showPage(page);
        central_page = message.get();
    }
};

CentralDocumentHost::CentralDocumentHost(vn::appfw::DocumentManager& documents, DocumentViewRegistry& views, DockPanelManager& docks)
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

    d->tabs = new DocumentTabBar(root);
    d->tabs->setDocumentMode(true);
    d->tabs->setExpanding(false);
    d->tabs->setMovable(true);
    // One document is not a choice: the bar follows count() instead of every add/remove repeating "visible at two".
    // (The close buttons are TabCloseButton instances set per tab, not QTabBar's stock indicator.)
    d->tabs->setAutoHide(true);
    d->tabs->setElideMode(Qt::ElideRight);
    d->tabs->setUsesScrollButtons(true);

    // Flat, not a row of boxes: an unselected tab is the strip's own surface, the selected one takes the page surface
    // plus an accent underline, and hover only tints. Palette roles only (like TableStyle.hpp), so a light/dark palette
    // is followed while painting instead of pinning colours; the underline keeps its width in both states, so selecting
    // a tab does not move the strip by a pixel.
    d->tabs->setStyleSheet(QStringLiteral("QTabBar {"
                                          " background: palette(window);"
                                          "}"
                                          "QTabBar::tab {"
                                          " background: transparent;"
                                          " color: palette(window-text);"
                                          " border: none;"
                                          " border-bottom: 2px solid transparent;"
                                          " padding: 6px 10px;"
                                          " margin-right: 1px;"
                                          "}"
                                          "QTabBar::tab:selected {"
                                          " background: palette(base);"
                                          " color: palette(text);"
                                          " border-bottom: 2px solid palette(highlight);"
                                          "}"
                                          "QTabBar::tab:hover:!selected {"
                                          " background: palette(alternate-base);"
                                          "}"
                                          "QToolButton#vnTabCloseButton {"
                                          " background: transparent;"
                                          " border: none;"
                                          " padding: 0px;"
                                          " color: palette(window-text);"
                                          "}"
                                          "QToolButton#vnTabCloseButton:hover {"
                                          " color: palette(highlight);"
                                          "}"));

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

    // Moving a tab reorders the strip only; entries (and therefore the tab <-> document mapping) move with it.
    QObject::connect(d->tabs, &QTabBar::tabMoved, root, [this](int from, int to) {
        auto* d = dptr();
        if (from < 0 || to < 0 || static_cast<std::size_t>(from) >= d->entries.size() || static_cast<std::size_t>(to) >= d->entries.size()) {
            return;
        }

        Impl::Entry moved = std::move(d->entries[static_cast<std::size_t>(from)]);
        d->entries.erase(d->entries.begin() + from);
        d->entries.insert(d->entries.begin() + to, std::move(moved));
    });

    // The strip represents every open document, floating ones included: clicking such a tab brings its window up.
    QObject::connect(d->tabs, &QTabBar::tabBarClicked, root, [this](int index) {
        auto* d = dptr();
        if (index < 0 || static_cast<std::size_t>(index) >= d->entries.size()) {
            return;
        }
        if (Impl::Entry& entry = d->entries[static_cast<std::size_t>(index)]; entry.window != nullptr) {
            entry.window->raise();
            entry.window->activateWindow();
        }
    });

    // The strip's context menu: float/dock/close. Each item is only a REQUEST (setFloating moves a page, close() asks
    // canClose()); without it a document is floatable from code only.
    d->tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(d->tabs, &QWidget::customContextMenuRequested, root, [this](const QPoint& pos) {
        auto* d = dptr();

        const int index = d->tabs != nullptr ? d->tabs->tabAt(pos) : -1;
        if (index < 0 || static_cast<std::size_t>(index) >= d->entries.size()) {
            return;
        }

        vn::appfw::Document* const document = d->entries[static_cast<std::size_t>(index)].document;
        if (document == nullptr || d->documents == nullptr) {
            return;
        }
        const bool floating = d->entries[static_cast<std::size_t>(index)].window != nullptr;

        QMenu          menu(d->tabs);
        QAction* const toggle = menu.addAction(floating ? QStringLiteral("停靠回中央区") : QStringLiteral("浮动到独立窗口"));
        menu.addSeparator();
        QAction* const close_action = menu.addAction(QStringLiteral("关闭文档"));

        QAction* const chosen = menu.exec(d->tabs->mapToGlobal(pos));
        if (chosen == nullptr) {
            return;
        }

        // The menu ran a nested event loop: the document may have closed meanwhile. Compare POINTERS (never dereference
        // the old one) and let the entry list prove it is still open.
        Impl::Entry* entry = nullptr;
        for (Impl::Entry& candidate : d->entries) {
            if (candidate.document == document) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            return;
        }

        if (chosen == toggle) {
            // Floating what the user just picked is "work on this one": selecting it first keeps current, tab and the
            // window that comes up saying the same thing.
            if (d->documents->current() != document) {
                d->documents->setCurrent(document);
            }
            setFloating(*document, entry->window == nullptr);
        }
        else if (chosen == close_action) {
            d->documents->close(document);
        }
    });

    // `opened` adds the tab (it does not select: create() leaves the current document alone).
    d->connections.push_back(documents.opened.connect([this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
        if (args.document() != nullptr) {
            dptr()->addTab(*args.document());
        }
    }));

    // `currentChanged` drives the screen and the strip: selecting a document shows it and selects its tab.
    d->connections.push_back(documents.currentChanged.connect([this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs&) { syncToCurrentDocument(); }));

    // `closed` is where the view and the tab of that document go: one view per instance means it dies with it. The
    // replacement selection belongs to removing the tab (see Impl::removeTab): a QTabBar cannot render "nothing is
    // selected", so the tab that takes the closed one's place is REQUESTED as current and the manager decides.
    d->connections.push_back(documents.closed.connect([this](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
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
    d->installed    = false;
    d->previous     = nullptr;
    d->current_view = nullptr;
    d->central_page = nullptr;
}

void CentralDocumentHost::syncToCurrentDocument()
{
    auto* d = dptr();

    vn::appfw::Document* const document = d->documents != nullptr ? d->documents->current() : nullptr;

    d->syncTabSelection(document);
    d->syncTabTitles();

    DocumentView* const view = document != nullptr ? obtainView(*document) : nullptr;

    // Exactly one activate()/deactivate() pair per current view. The host clears its current view before calling
    // deactivate() and sets it again after activate(), so a view never sees itself as "the one on screen" from inside
    // those callbacks (see DocumentView::renderControl()).
    const bool switched = d->current_view != view;
    if (switched && d->current_view != nullptr) {
        DocumentView* const previous = d->current_view;
        d->current_view              = nullptr;
        previous->deactivate();
    }

    if (document == nullptr) {
        // Contract 2: with nothing to show, the host leaves the central area to the shell.
        if (d->installed) {
            d->showMessage(u8"没有当前文档");
        }
        else {
            d->central_page = nullptr;
        }
    }
    else {
        installIntoCentralArea();

        Impl::Entry* const entry = d->findEntry(*document);

        if (entry != nullptr && entry->window != nullptr) {
            // The page lives in its own window; the central area says so instead of showing another document. That is
            // what keeps "selected tab", "current document" and "central content" one answer.
            if (!entry->window->isActiveWindow()) {
                entry->window->raise();
                entry->window->activateWindow();
            }

            String text = u8"“";
            text.append(document->title());
            text.append(u8"” 在独立窗口中");
            d->showMessage(text);
        }
        else if (view == nullptr) {
            // Two different reasons, two different sentences: "nothing is registered for this type" is a registration
            // gap, "the factory refused this document" is the view's own judgement (a document it cannot present).
            const bool registered = d->views != nullptr && d->views->hasView(document->typeId());

            String text = u8"“";
            text.append(document->typeId());
            text.append(registered ? u8"” 类型的视图工厂拒绝了这份文档" : u8"” 类型没有注册视图");
            d->showMessage(text);
        }
        else if (QWidget* const page = entry != nullptr ? entry->page() : nullptr; page != nullptr) {
            d->showPage(page);
            d->central_page = view->content();
        }
        else {
            d->showMessage(u8"当前文档没有可显示的页");
        }

        if (switched && view != nullptr) {
            view->activate();
            d->current_view = view;
        }
    }
}

raw_ptr<DocumentView> CentralDocumentHost::currentView() const
{
    return dptr()->current_view;
}

raw_ptr<UIElement> CentralDocumentHost::shownContent() const
{
    return dptr()->central_page;
}

raw_ptr<DocumentView> CentralDocumentHost::findView(const vn::appfw::Document& document) const
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

        // Focus follows the document: working in a floated window makes it the document commands act on. The window
        // only REPORTS the activation; the manager decides, and the host follows (see syncToCurrentDocument()).
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

        // Both take effect before the show: showing can activate the window synchronously, and the activation handler
        // calls setCurrent() -> syncToCurrentDocument(), which must already see the page in its window (otherwise it
        // would take it back into the central stack).
        entry->window = std::move(window);
        entry->window->show();
    }
    else {
        // Dock back: the page returns to the central stack. Whether it becomes the visible page is decided below, by
        // the one rule "the central area shows the current document".
        const std::unique_ptr<FloatingDocumentWindow> window = std::move(entry->window);
        QWidget* const                                page   = window->takePage();

        if (page != nullptr) {
            page->setParent(nullptr);
            if (d->stack != nullptr) {
                d->stack->addWidget(page);
            }
        }
    }

    // One place decides what the screen shows after the page moved.
    syncToCurrentDocument();
}

DocumentView* CentralDocumentHost::obtainView(vn::appfw::Document& document)
{
    if (DocumentView* existing = findView(document); existing != nullptr) {
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
