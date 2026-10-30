#include <vine/appfw/gui/Backstage.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QAction>
#include <QEvent>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayoutItem>
#include <QListWidget>
#include <QShortcut>
#include <QSize>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/Exception.hpp>
#include <vine/logging/Log.hpp>

#include "ControlData.hpp"
#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(BackstagePage, Control)
VN_OBJECT_META_IMPL(Backstage, Window)

namespace
{

/// @brief Converts the framework's UTF-16 string to Qt.
QString toQString(const String& text)
{
    return QString::fromStdU16String(text.toUtf16());
}

/// @brief Width of the navigation rail (the Office Backstage's left column).
constexpr int kRailWidth = 240;

/// @brief Row height of one navigation entry; fixed so the rail keeps its rhythm across fonts.
constexpr int kNavRowHeight = 40;

/// @brief Cap of the item column inside a page.
///
/// A row that is stretched across a maximized 4K window stops reading as one entry and starts reading as a
/// gap between two labels, which is why every Backstage-like surface caps its content column.
constexpr int kPageColumnWidth = 760;

/**
 * @brief The navigation rail's stylesheet: one flat row per page, accent-filled selection.
 *
 * Every colour is a palette lookup resolved while painting, so the rail follows a light/dark theme change
 * instead of pinning one theme's colours (same rule as detail::blendIntoSurface()).
 *
 * @return The stylesheet to apply to the rail's QListWidget.
 */
QString navStyles()
{
    return QStringLiteral("QListWidget {"
                          " background: transparent;"
                          " border: none;"
                          " outline: none;"
                          " }"
                          "QListWidget::item {"
                          " padding: 8px 10px;"
                          " border-radius: 6px;"
                          " color: palette(window-text);"
                          " }"
                          "QListWidget::item:hover {"
                          " background-color: palette(midlight);"
                          " }"
                          "QListWidget::item:selected {"
                          " background-color: palette(highlight);"
                          " color: palette(highlighted-text);"
                          " }");
}

/**
 * @brief The item column's stylesheet: command entries are flat rows with a hover fill.
 *
 * @return The stylesheet to apply to the column that holds a page's item buttons.
 */
QString itemStyles()
{
    return QStringLiteral("QToolButton {"
                          " text-align: left;"
                          " padding: 9px 12px;"
                          " border: none;"
                          " border-radius: 6px;"
                          " color: palette(text);"
                          " }"
                          "QToolButton:hover {"
                          " background-color: palette(midlight);"
                          " }"
                          "QToolButton:pressed {"
                          " background-color: palette(mid);"
                          " }"
                          "QToolButton:disabled {"
                          " color: palette(mid);"
                          " }");
}

/**
 * @brief The Backstage's native window: a top-level tool window that follows the main window.
 *
 * A child overlay would be painted below the central 3D surface (a native child window), so the
 * Backstage is its own top-level window; this is the same rule the docking flyout follows.
 */
class BackstageWindow final : public QWidget {
  public:
    explicit BackstageWindow(QWidget* main_window)
      : QWidget(main_window, Qt::Tool | Qt::FramelessWindowHint)
      , main_window_(main_window)
    {
        setAutoFillBackground(true);
        setFocusPolicy(Qt::StrongFocus);

        // Frameless, so the window has no system edge: a hairline is what tells the user where the
        // overlay ends and the main window behind it begins.
        setObjectName(QStringLiteral("vnBackstageWindow"));
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet(QStringLiteral("#vnBackstageWindow {"
                                     " border: 1px solid palette(mid);"
                                     " background-color: palette(window);"
                                     " }"));

        if (main_window_ != nullptr) {
            main_window_->installEventFilter(this);

            // The main window can be destroyed before its owned Backstage: forget it instead of
            // touching a dangling widget in the destructor.
            QObject::connect(main_window_, &QObject::destroyed, this, [this] { main_window_ = nullptr; });
        }
    }

    ~BackstageWindow() override
    {
        if (main_window_ != nullptr) {
            main_window_->removeEventFilter(this);
        }
    }

    /// @brief Fired after the window became visible.
    std::function<void()> shown;
    /// @brief Fired after the window was hidden.
    std::function<void()> hidden;

    /// @brief Places and sizes the window over the main window's client area.
    void syncToMainWindow()
    {
        if (main_window_ == nullptr || !main_window_->isVisible()) {
            return;
        }

        const QPoint origin = main_window_->mapToGlobal(QPoint(0, 0));

        if (pos() != origin) {
            move(origin);
        }
        if (size() != main_window_->size()) {
            resize(main_window_->size());
        }
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == main_window_ && event != nullptr) {
            switch (event->type()) {
            case QEvent::Move:
            case QEvent::Resize:
            case QEvent::Show:
            {
                if (isVisible()) {
                    syncToMainWindow();
                }
                break;
            }

            case QEvent::WindowStateChange:
            {
                if (main_window_ != nullptr && main_window_->isMinimized()) {
                    hide();
                }
                else if (isVisible()) {
                    syncToMainWindow();
                }
                break;
            }

            case QEvent::Hide:
            case QEvent::Close:
            {
                hide();
                break;
            }

            default: break;
            }
        }

        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent* event) override
    {
        syncToMainWindow();
        QWidget::showEvent(event);

        if (shown) {
            shown();
        }
    }

    void hideEvent(QHideEvent* event) override
    {
        QWidget::hideEvent(event);

        if (hidden) {
            hidden();
        }
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event != nullptr && event->key() == Qt::Key_Escape) {
            hide();
            event->accept();
            return;
        }

        QWidget::keyPressEvent(event);
    }

  private:
    QWidget* main_window_ = nullptr;
};

} // namespace

struct BackstagePage::Impl : public ControlData {
    /// One added item: its action (identity) and the button the page built for it.
    struct ItemEntry {
        RibbonAction* action  = nullptr;
        QToolButton*  button  = nullptr;
        bool          visible = true;
    };

    String                id;
    String                title;
    Icon                  icon;
    String                provider;
    BackstagePageRole     role   = BackstagePageRole::None;
    BackstagePagePolicy   policy = BackstagePagePolicy::None;
    std::vector<ItemEntry> items;
    /// The page widget's root layout and the command column inside it; both are the page's own widgets.
    QVBoxLayout* page_layout  = nullptr;
    QWidget*     items_column = nullptr;
    /// The capped column the item buttons are appended to; null while the page has no command column.
    QVBoxLayout* items_layout = nullptr;
    /// The installed page body (a control the page shows instead of the command column), or null.
    Control* body = nullptr;
};

BackstagePage::BackstagePage()
  : Control(new Impl(), new QWidget())
{
    auto* page   = impl<QWidget>();
    auto* layout = new QVBoxLayout(page);

    layout->setContentsMargins(20, 8, 32, 32);
    layout->setSpacing(0);

    auto* column        = new QWidget(page);
    auto* column_layout = new QVBoxLayout(column);

    column->setMaximumWidth(kPageColumnWidth);
    column->setStyleSheet(itemStyles());
    column_layout->setContentsMargins(0, 0, 0, 0);
    column_layout->setSpacing(2);
    column_layout->addStretch(1); // item buttons are inserted before the stretch

    layout->addWidget(column, 0, Qt::AlignTop);
    layout->addStretch(1);

    dptr()->page_layout  = layout;
    dptr()->items_column = column;
    dptr()->items_layout = column_layout;
}

BackstagePage::~BackstagePage()
{
    // d is deleted by UIElement; the page widget is owned by the Backstage stack after addPage().
}

void BackstagePage::setId(const String& id)
{
    dptr()->id = id;
}

String BackstagePage::id() const
{
    return dptr()->id;
}

void BackstagePage::setTitle(const String& title)
{
    dptr()->title = title;
}

String BackstagePage::title() const
{
    return dptr()->title;
}

void BackstagePage::setIcon(const Icon& icon)
{
    dptr()->icon = icon;
}

Icon BackstagePage::icon() const
{
    return dptr()->icon;
}

void BackstagePage::setRole(BackstagePageRole role)
{
    dptr()->role = role;
}

BackstagePageRole BackstagePage::role() const
{
    return dptr()->role;
}

void BackstagePage::setPolicy(BackstagePagePolicy policy)
{
    dptr()->policy = policy;
}

BackstagePagePolicy BackstagePage::policy() const
{
    return dptr()->policy;
}

void BackstagePage::setProvider(const String& provider)
{
    dptr()->provider = provider;
}

String BackstagePage::provider() const
{
    return dptr()->provider;
}

void BackstagePage::addItem(RibbonAction* item)
{
    if (item == nullptr) {
        return;
    }

    if (dptr()->body != nullptr) {
        VN_LOGW("BackstagePage::addItem: the page shows a body, so it takes no command items");
        return;
    }

    // The action is the item's identity (and setItemVisible()'s key): one entry per action.
    for (const auto& entry : dptr()->items) {
        if (entry.action == item) {
            return;
        }
    }

    auto* layout = dptr()->items_layout;
    auto* action = item->impl<QAction>();

    if (layout == nullptr || action == nullptr) {
        return;
    }

    auto* button = new QToolButton(layout->parentWidget());

    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setIconSize(QSize(20, 20));
    button->setMinimumHeight(36);
    button->setCursor(Qt::PointingHandCursor);

    // The stretch stays last, so appended items keep their order.
    layout->insertWidget(layout->count() - 1, button);

    dptr()->items.push_back({ item, button, true });
}

int BackstagePage::numItems() const
{
    return (int)dptr()->items.size();
}

RibbonAction* BackstagePage::itemAt(int index) const
{
    return (index >= 0 && index < (int)dptr()->items.size()) ? dptr()->items[(size_t)index].action : nullptr;
}

void BackstagePage::setItemVisible(RibbonAction* item, bool visible)
{
    if (item == nullptr) {
        return;
    }

    for (auto& entry : dptr()->items) {
        if (entry.action != item) {
            continue;
        }
        entry.visible = visible;
        if (entry.button != nullptr) {
            entry.button->setVisible(visible);
        }
        return;
    }
}

bool BackstagePage::isItemVisible(const RibbonAction* item) const
{
    if (item == nullptr) {
        return false;
    }

    for (const auto& entry : dptr()->items) {
        if (entry.action == item) {
            return entry.visible;
        }
    }
    return false;
}

bool BackstagePage::setBody(Control* body)
{
    if (body == nullptr) {
        return false;
    }

    auto* data   = dptr();
    auto* widget = body->impl<QWidget>();

    if (widget == nullptr || data->page_layout == nullptr) {
        return false;
    }

    if (data->body == body) {
        return true;
    }

    if (!data->items.empty()) {
        VN_LOGW("BackstagePage::setBody: the page already has command items; a page body is one thing");
        return false;
    }

    if (data->body != nullptr) {
        // The previous body is the caller's widget again (same rule removePages() follows).
        if (auto* previous = data->body->impl<QWidget>(); previous != nullptr) {
            data->page_layout->removeWidget(previous);
            previous->setParent(nullptr);
            data->body->setOwnsImpl(true);
        }
        data->body = nullptr;
    }

    // The command column and its trailing stretch step aside: a body is the whole page area. Anything else in the
    // layout would be a previous body, and that was handed back above.
    while (QLayoutItem* item = data->page_layout->takeAt(0)) {
        QWidget* child = item->widget();
        delete item;
        if (child != nullptr && child == data->items_column) {
            delete child; // the page built it; nothing outside refers to it
        }
    }

    data->items_column = nullptr;
    data->items_layout = nullptr;

    data->page_layout->addWidget(widget, 1);
    data->body = body;

    // The page widget owns the body from now on; the wrapper must not delete it again.
    body->setOwnsImpl(false);
    return true;
}

Control* BackstagePage::body() const
{
    return dptr()->body;
}

inline auto BackstagePage::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto BackstagePage::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

namespace
{

/// One registered page: the logical model the navigation and the content stack are derived from.
struct PageEntry
{
    BackstagePage*       page      = nullptr;
    QListWidgetItem*     nav_item  = nullptr;
    BackstagePlacement   placement;
    std::uint64_t        seq       = 0; ///< Registration order: the deterministic tie-break.
    BackstagePlaceStatus status    = BackstagePlaceStatus::Placed;
    bool                 visible   = true;
};

/// @brief Whether the page must stay in the terminal suffix.
bool isTerminal(const BackstagePage* page)
{
    return page != nullptr && testFlag(page->policy(), BackstagePagePolicy::Terminal);
}

/// @brief Index of the first terminal page, or the list size when there is none.
int terminalBegin(const std::vector<PageEntry>& placed)
{
    for (int i = 0; i < (int)placed.size(); ++i) {
        if (isTerminal(placed[(size_t)i].page)) {
            return i;
        }
    }
    return (int)placed.size();
}

/// @brief Append index for a role: role-bearing pages sort by role value among themselves.
///
/// Role-less pages are transparent for this scan: a role only orders against other roles.
///
/// @param placed         Already placed entries.
/// @param role           Role of the page being placed, or None.
/// @param terminal_begin Index of the first terminal page.
/// @return Insertion index in [0, terminal_begin].
int appendIndexForRole(const std::vector<PageEntry>& placed, BackstagePageRole role, int terminal_begin)
{
    if (role == BackstagePageRole::None) {
        return terminal_begin;
    }

    const int rank = static_cast<int>(role);

    for (int i = 0; i < terminal_begin; ++i) {
        const BackstagePage* page = placed[(size_t)i].page;
        if (page->role() != BackstagePageRole::None && static_cast<int>(page->role()) > rank) {
            return i;
        }
    }

    return terminal_begin;
}

/// @brief Where a page goes in the current logical order, and why.
struct Resolved
{
    int                  index  = -1;
    BackstagePlaceStatus status = BackstagePlaceStatus::Placed;
};

/// @brief Resolves one page against the pages placed before it (registration order).
///
/// This is the rule RibbonBar resolves tabs with, applied to pages: anchors only look backwards, so
/// the resolution is a single pass and cannot form a cycle.
///
/// @param placed    Entries placed so far, in logical order.
/// @param page      Page being placed.
/// @param placement Placement request of that page.
/// @return The insertion index and the status to report.
Resolved resolvePage(const std::vector<PageEntry>& placed, const BackstagePage* page, const BackstagePlacement& placement)
{
    if (isTerminal(page)) {
        // Terminal pages form the suffix, in registration order; a relative placement is not used.
        return { (int)placed.size(), BackstagePlaceStatus::Placed };
    }

    const int terminal_begin = terminalBegin(placed);

    int                  index  = terminal_begin;
    BackstagePlaceStatus status = BackstagePlaceStatus::Placed;

    if (placement.where == BackstagePlacement::Where::Append) {
        index = appendIndexForRole(placed, page->role(), terminal_begin);
    } else if (!placement.anchor_id.empty() && placement.anchor_role != BackstagePageRole::None) {
        return { -1, BackstagePlaceStatus::InvalidAnchor };
    } else if (!placement.anchor_id.empty()) {
        int anchor = -1;

        for (int i = 0; i < (int)placed.size(); ++i) {
            if (placed[(size_t)i].page->id() == placement.anchor_id) {
                anchor = i;
                break;
            }
        }

        if (anchor < 0) {
            status = BackstagePlaceStatus::PlacedByFallback;
            index  = terminal_begin;
        }
        else {
            index = (placement.where == BackstagePlacement::Where::Before) ? anchor : anchor + 1;
        }
    } else if (placement.anchor_role != BackstagePageRole::None) {
        int anchor = -1;

        for (int i = 0; i < (int)placed.size(); ++i) {
            if (placed[(size_t)i].page->role() == placement.anchor_role) {
                anchor = i;
                break;
            }
        }

        if (anchor < 0) {
            status = BackstagePlaceStatus::PlacedByFallback;
            index  = appendIndexForRole(placed, placement.anchor_role, terminal_begin);
        }
        else {
            index = (placement.where == BackstagePlacement::Where::Before) ? anchor : anchor + 1;
        }
    } else {
        return { -1, BackstagePlaceStatus::InvalidAnchor };
    }

    if (index > terminal_begin) {
        index  = terminal_begin;
        status = BackstagePlaceStatus::ClampedBeforeTerminal;
    }

    return { index, status };
}

} // namespace

struct Backstage::Impl : public WindowData {
    std::vector<PageEntry> pages; // logical order, kept in sync with the navigation rows and the stack
    std::uint64_t          next_seq   = 0;
    /// True while the rows are being re-ordered: the selection signals must not drive the stack then.
    bool                   reordering = false;
    BackstageWindow*       window     = nullptr;
    QWidget*               main       = nullptr;
    QListWidget*           nav        = nullptr;
    QStackedWidget*        stack      = nullptr;
    QLabel*                title      = nullptr;
    EventArgs              args;

    /// @return The index of @p page in the stable page order, or -1.
    int indexOf(const BackstagePage* page) const
    {
        for (size_t i = 0; i < pages.size(); ++i) {
            if (pages[i].page == page) {
                return (int)i;
            }
        }
        return -1;
    }

    /// @brief Picks the next visible page after @p from (wrapping), or clears the selection.
    void selectNextVisible(int from)
    {
        const int count = (int)pages.size();
        // Every page is a candidate, including @p from itself: with a single visible page the wrap lands on it,
        // and a page hidden by the caller is already marked invisible before this runs, so it is skipped.
        for (int step = 1; step <= count; ++step) {
            const int candidate = (from + step) % count;
            if (pages[(size_t)candidate].visible) {
                nav->setCurrentRow(candidate);
                return;
            }
        }

        // Nothing is left to show: no current page, no page content (QStackedWidget allows -1).
        nav->setCurrentRow(-1);
        stack->setCurrentIndex(-1);
        title->clear();
    }
};

Backstage::Backstage(MainWindow* window)
  : Window(new Impl(),
           new BackstageWindow(window != nullptr ? qobject_cast<QWidget*>(window->impl()) : nullptr),
           /*owns=*/false)
{
    auto* native = static_cast<BackstageWindow*>(impl<QWidget>());
    auto* data   = dptr();

    data->window = native;
    data->main   = window != nullptr ? qobject_cast<QWidget*>(window->impl()) : nullptr;

    if (native == nullptr) {
        return;
    }

    // The window is parented to the main window, which may destroy it first (teardown).
    QObject::connect(native, &QObject::destroyed, native, [data] { data->window = nullptr; });

    if (data->main != nullptr) {
        // close() must not touch the main window after it was destroyed.
        QObject::connect(data->main, &QObject::destroyed, native, [data] { data->main = nullptr; });
    }

    auto* root = new QHBoxLayout(native);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // LEFT: the navigation rail. The back arrow sits at its top-left, like the Office Backstage's: the way out
    // belongs next to the way in, not in the content header where it competes with the page title.
    auto* rail = new QWidget(native);
    rail->setObjectName(QStringLiteral("vnBackstageRail"));
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setFixedWidth(kRailWidth);
    rail->setStyleSheet(QStringLiteral("#vnBackstageRail {"
                                       " background-color: palette(window);"
                                       " border-right: 1px solid palette(mid);"
                                       " }"));

    auto* rail_layout = new QVBoxLayout(rail);
    rail_layout->setContentsMargins(8, 8, 8, 8);
    rail_layout->setSpacing(6);

    auto* back = new QToolButton(rail);
    back->setText(QStringLiteral("\u2190"));
    back->setAutoRaise(true);
    back->setFixedSize(36, 36);
    back->setCursor(Qt::PointingHandCursor);
    back->setToolTip(QStringLiteral("返回 (Esc)"));

    auto* nav = new QListWidget(rail);
    nav->setIconSize(QSize(20, 20));
    nav->setSpacing(2);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    nav->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    nav->setStyleSheet(navStyles());

    // One point larger than the content text: in a Backstage the rail is a signpost, not a list of details.
    auto nav_font = nav->font();
    nav_font.setPointSizeF(nav_font.pointSizeF() + 1.0);
    nav->setFont(nav_font);

    rail_layout->addWidget(back, 0, Qt::AlignLeft);
    rail_layout->addWidget(nav, 1);

    // RIGHT: the page surface. It paints the window's base colour, so the rail (window colour) reads as a
    // separate column instead of the whole window being one flat grey.
    auto* content = new QWidget(native);
    content->setObjectName(QStringLiteral("vnBackstageContent"));
    content->setAttribute(Qt::WA_StyledBackground, true);
    content->setStyleSheet(QStringLiteral("#vnBackstageContent { background-color: palette(base); }"));

    auto* content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    auto* header        = new QWidget(content);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(32, 22, 32, 16);
    header_layout->setSpacing(8);

    auto* title = new QLabel(header);
    auto  font  = title->font();
    font.setPointSizeF(font.pointSizeF() + 9.0);
    font.setWeight(QFont::DemiBold);
    title->setFont(font);

    header_layout->addWidget(title);
    header_layout->addStretch(1);

    auto* rule = new QFrame(content);
    rule->setFixedHeight(1);
    rule->setFrameShape(QFrame::NoFrame);
    rule->setAttribute(Qt::WA_StyledBackground, true);
    rule->setStyleSheet(QStringLiteral("background-color: palette(mid);"));

    auto* stack = new QStackedWidget(content);

    // The stack's own margins would push every page in by a style-dependent amount, so the page's item column
    // would no longer line up with the page title above it.
    stack->setContentsMargins(0, 0, 0, 0);

    content_layout->addWidget(header);
    content_layout->addWidget(rule);
    content_layout->addWidget(stack, 1);

    root->addWidget(rail);
    root->addWidget(content, 1);

    data->nav   = nav;
    data->stack = stack;
    data->title = title;

    QObject::connect(back, &QAbstractButton::clicked, native, [native] { native->hide(); });

    QObject::connect(nav, &QListWidget::currentRowChanged, native, [data](int row) {
        // While the rows are re-ordered the row numbers are in flux: the stack follows the logical order there.
        if (data->reordering || row < 0 || row >= (int)data->pages.size()) {
            return;
        }

        data->stack->setCurrentIndex(row);
        data->title->setText(toQString(data->pages[(size_t)row].page->title()));
    });

    auto* escape = new QShortcut(QKeySequence(QKeySequence::Cancel), native);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    QObject::connect(escape, &QShortcut::activated, native, [native] { native->hide(); });

    native->shown  = [this] { opened.trigger(*this, dptr()->args); };
    native->hidden = [this] { closed.trigger(*this, dptr()->args); };
}

Backstage::~Backstage()
{
    auto* native = impl<QWidget>();

    if (native != nullptr) {
        auto* window = static_cast<BackstageWindow*>(native);

        // Stop the window from calling back into a half-destroyed object: destroying the window
        // can deliver a hide event, and the callbacks capture this.
        window->shown  = nullptr;
        window->hidden = nullptr;

        // The window is parented to the main window (Qt::Tool) and is not owned by the Control
        // base; delete it here so replacing a Backstage cannot leave an orphan window behind.
        // When the main window died first, impl is already null and the window was deleted with it.
        delete native;
    }

    dptr()->window = nullptr;

    // d is deleted by UIElement; the page widgets went with the window.
    // Page wrappers and items are owned by their creators (same rule as ribbon tabs/menu items).
}

bool Backstage::addPage(BackstagePage* page)
{
    if (page == nullptr) {
        // Legacy contract: a null page is rejected instead of throwing; the placement overload throws instead
        // (the same split RibbonTab::addGroup has).
        return false;
    }

    return addPage(page, BackstagePlacement::append()).index >= 0;
}

BackstagePlaceResult Backstage::addPage(BackstagePage* page, const BackstagePlacement& placement)
{
    VN_CHECK_NULL_THROW(page)

    auto* data = dptr();

    if (data->window == nullptr) {
        return { BackstagePlaceStatus::Unavailable, -1 };
    }

    // Adding the same object twice would put two navigation rows on one widget.
    const int existing = data->indexOf(page);

    if (existing >= 0) {
        return { BackstagePlaceStatus::AlreadyAdded, existing };
    }

    const String id = page->id();

    if (!id.empty() && findPage(id) != nullptr) {
        VN_LOGW("Backstage::addPage: duplicate page id '{}', page not added", id.std_str_view());
        return { BackstagePlaceStatus::DuplicateId, -1 };
    }

    if (placement.where != BackstagePlacement::Where::Append) {
        const bool has_id   = !placement.anchor_id.empty();
        const bool has_role = placement.anchor_role != BackstagePageRole::None;

        // Exactly one anchor identifies the sibling; both (or neither) is a caller bug.
        if (has_id == has_role) {
            VN_LOGW("Backstage::addPage: a relative placement needs exactly one anchor, page not added");
            return { BackstagePlaceStatus::InvalidAnchor, -1 };
        }
    }

    auto* page_widget = page->impl<QWidget>();

    if (page_widget == nullptr) {
        return { BackstagePlaceStatus::Unavailable, -1 };
    }

    // Captured before the reorder: the user keeps looking at the same page whatever moved around it.
    BackstagePage* keep = currentPage();

    // The stack owns the widget from now on; the wrapper must not delete it again.
    page->setOwnsImpl(false);

    auto* item = new QListWidgetItem(data->nav);
    item->setText(toQString(page->title()));
    item->setSizeHint(QSize(0, kNavRowHeight));

    if (!page->icon().isNull()) {
        item->setIcon(page->icon().value());
    }

    data->stack->addWidget(page_widget);
    data->pages.push_back(PageEntry{ page, item, placement, data->next_seq++, BackstagePlaceStatus::Placed, true });

    layoutPages(keep);

    // The first page added becomes the current one; every later add keeps what was selected.
    if (data->nav->currentRow() < 0) {
        data->selectNextVisible(-1); // rows are 0-based: the first step lands on row 0
    }

    const int index = data->indexOf(page);
    return { index >= 0 ? data->pages[(size_t)index].status : BackstagePlaceStatus::Unavailable, index };
}

void Backstage::layoutPages(BackstagePage* keep_current)
{
    auto* data = dptr();

    // Rebuild from the registration order: placement decides, registration order breaks ties.
    std::vector<PageEntry> all = data->pages;

    std::sort(all.begin(), all.end(), [](const PageEntry& a, const PageEntry& b) { return a.seq < b.seq; });

    std::vector<PageEntry> ordered;
    ordered.reserve(all.size());

    for (PageEntry& entry : all) {
        const Resolved resolved = resolvePage(ordered, entry.page, entry.placement);

        entry.status = resolved.status;

        const int index = resolved.index < 0 ? (int)ordered.size() : resolved.index;
        ordered.insert(ordered.begin() + index, entry);
    }

    data->pages = std::move(ordered);

    if (data->nav == nullptr || data->stack == nullptr) {
        return; // the window is gone (teardown); the logical order is all that is left to keep straight
    }

    // The logical order is the truth; the navigation rows and the stack indices only mirror it.
    const bool was_reordering = data->reordering;
    data->reordering          = true;

    for (int i = 0; i < (int)data->pages.size(); ++i) {
        QListWidgetItem* item = data->pages[(size_t)i].nav_item;
        const int        row  = data->nav->row(item);

        if (row >= 0 && row != i) {
            data->nav->insertItem(i, data->nav->takeItem(row));
        }
    }

    for (int i = 0; i < (int)data->pages.size(); ++i) {
        auto* widget = data->pages[(size_t)i].page->impl<QWidget>();

        if (widget == nullptr) {
            continue;
        }

        const int current = data->stack->indexOf(widget);

        if (current >= 0 && current != i) {
            data->stack->removeWidget(widget);
            data->stack->insertWidget(i, widget);
        }
    }

    data->reordering = was_reordering;

    if (keep_current != nullptr && data->indexOf(keep_current) >= 0 && isPageVisible(keep_current)) {
        setCurrentPage(keep_current);
    }
}

BackstagePage* Backstage::findPage(const String& id) const
{
    if (id.empty()) {
        return nullptr;
    }

    for (const auto& entry : dptr()->pages) {
        if (entry.page->id() == id) {
            return entry.page;
        }
    }

    return nullptr;
}

BackstagePage* Backstage::findPage(BackstagePageRole role) const
{
    if (role == BackstagePageRole::None) {
        return nullptr;
    }

    for (const auto& entry : dptr()->pages) {
        if (entry.page->role() == role) {
            return entry.page;
        }
    }

    return nullptr;
}

int Backstage::numPages() const
{
    return (int)dptr()->pages.size();
}

BackstagePage* Backstage::pageAt(int index) const
{
    return (index >= 0 && index < (int)dptr()->pages.size()) ? dptr()->pages[(size_t)index].page : nullptr;
}

void Backstage::setCurrentPage(BackstagePage* page)
{
    if (page == nullptr) {
        return;
    }

    const int index = dptr()->indexOf(page);
    if (index < 0 || !dptr()->pages[(size_t)index].visible) {
        return;
    }

    dptr()->nav->setCurrentRow(index);
    dptr()->stack->setCurrentIndex(index);
    dptr()->title->setText(toQString(page->title()));
}

BackstagePage* Backstage::currentPage() const
{
    const int row = dptr()->nav != nullptr ? dptr()->nav->currentRow() : -1;

    return (row >= 0 && row < (int)dptr()->pages.size()) ? dptr()->pages[(size_t)row].page : nullptr;
}

void Backstage::setPageVisible(BackstagePage* page, bool visible)
{
    auto* data = dptr();

    if (page == nullptr) {
        return;
    }

    const int index = data->indexOf(page);
    if (index < 0) {
        return;
    }

    auto& entry = data->pages[(size_t)index];
    if (entry.visible == visible) {
        return;
    }

    entry.visible = visible;
    entry.nav_item->setHidden(!visible);

    if (!visible) {
        if (data->nav->currentRow() == index) {
            data->selectNextVisible(index);
        }
        return;
    }

    // Nothing is current (every page was hidden): the page that comes back becomes the current one.
    if (data->nav->currentRow() < 0) {
        setCurrentPage(page);
    }
}

bool Backstage::isPageVisible(const BackstagePage* page) const
{
    const int index = dptr()->indexOf(page);
    return index >= 0 && dptr()->pages[(size_t)index].visible;
}

int Backstage::removePages(const String& provider)
{
    if (provider.empty()) {
        // An empty provider never matches, the same rule an empty page id follows.
        return 0;
    }

    auto* data = dptr();

    if (data->window == nullptr || data->nav == nullptr || data->stack == nullptr) {
        // Teardown: the widgets went with the window, so there is nothing left to unregister.
        return 0;
    }

    std::vector<BackstagePage*> doomed;

    for (const PageEntry& entry : data->pages) {
        if (entry.page->provider() == provider) {
            doomed.push_back(entry.page);
        }
    }

    if (doomed.empty()) {
        return 0;
    }

    BackstagePage* keep     = currentPage();
    const int      old_row  = data->nav->currentRow();
    bool           keep_gone = false;

    for (BackstagePage* page : doomed) {
        if (page == keep) {
            keep      = nullptr;
            keep_gone = true;
        }
    }

    // The rows renumber while they are deleted: the selection signals must not drive the stack in between.
    const bool was_reordering = data->reordering;
    data->reordering          = true;

    for (BackstagePage* page : doomed) {
        const int index = data->indexOf(page);

        if (index < 0) {
            continue;
        }

        if (QListWidgetItem* item = data->pages[(size_t)index].nav_item; item != nullptr) {
            delete data->nav->takeItem(data->nav->row(item));
        }

        data->pages.erase(data->pages.begin() + index);

        if (auto* widget = page->impl<QWidget>(); widget != nullptr) {
            data->stack->removeWidget(widget);
            // Out of the stack's ownership and back to the page: the page can be added again, and whichever dies
            // first deletes the widget exactly once.
            widget->setParent(nullptr);
            page->setOwnsImpl(true);
        }
    }

    data->reordering = was_reordering;

    if (keep != nullptr && data->indexOf(keep) >= 0) {
        setCurrentPage(keep);
    } else if (keep_gone && old_row >= 0) {
        // The page that followed the removed one slid into its slot (clamped to the new last page); selectNextVisible()
        // starts one past its argument and wraps, so pass that slot - 1 and hidden pages are skipped from there.
        data->selectNextVisible(std::min(old_row, (int)data->pages.size() - 1) - 1);
    }

    return (int)doomed.size();
}

void Backstage::open()
{
    auto* data = dptr();

    if (data->window == nullptr) {
        return;
    }

    const bool any_visible = std::any_of(data->pages.begin(), data->pages.end(), [](const PageEntry& entry) { return entry.visible; });

    if (!any_visible) {
        VN_LOGW("Backstage::open: no visible page, the page is not shown");
        return;
    }

    if (data->nav->currentRow() < 0) {
        data->selectNextVisible(-1); // rows are 0-based: the first step lands on row 0
    }

    data->window->syncToMainWindow();
    data->window->show();
    data->window->raise();
    data->window->activateWindow();

    if (data->nav != nullptr) {
        data->nav->setFocus();
    }
}

void Backstage::close()
{
    if (dptr()->window != nullptr) {
        dptr()->window->hide();
    }

    if (dptr()->main != nullptr) {
        dptr()->main->activateWindow();
    }
}

bool Backstage::isOpen() const
{
    return dptr()->window != nullptr && dptr()->window->isVisible();
}

inline auto Backstage::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto Backstage::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
