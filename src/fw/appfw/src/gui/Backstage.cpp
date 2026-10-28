#include <vine/appfw/gui/Backstage.hpp>

#include <algorithm>
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
#include <QListWidget>
#include <QShortcut>
#include <QSize>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/logging/Log.hpp>

#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(BackstagePage, Control)
VN_OBJECT_META_IMPL(Backstage, Control)

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

    String                 id;
    String                 title;
    Icon                   icon;
    std::vector<ItemEntry> items;
    /// The capped column the item buttons are appended to; null while the page has no widget.
    QVBoxLayout* items_layout = nullptr;
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

void BackstagePage::addItem(RibbonAction* item)
{
    if (item == nullptr) {
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

inline auto BackstagePage::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto BackstagePage::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

struct Backstage::Impl : public ControlData {
    /// One added page: the wrapper, its navigation row, and whether the host shows it.
    struct PageEntry {
        BackstagePage*   page     = nullptr;
        QListWidgetItem* nav_item = nullptr;
        bool             visible  = true;
    };

    std::vector<PageEntry> pages;
    BackstageWindow*       window = nullptr;
    QWidget*               main   = nullptr;
    QListWidget*           nav    = nullptr;
    QStackedWidget*        stack  = nullptr;
    QLabel*                title  = nullptr;
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
        for (int step = 1; step < count; ++step) {
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
  : Control(new Impl(),
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
        if (row < 0 || row >= (int)data->pages.size()) {
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
    auto* data = dptr();

    if (page == nullptr || data->window == nullptr) {
        return false;
    }

    auto* page_widget = page->impl<QWidget>();

    if (page_widget == nullptr) {
        return false;
    }

    // Adding the same object twice would put two navigation rows on one widget.
    if (data->indexOf(page) >= 0) {
        return false;
    }

    const String id = page->id();

    if (!id.empty() && findPage(id) != nullptr) {
        VN_LOGW("Backstage::addPage: duplicate page id '{}', page not added", id.std_str_view());
        return false;
    }

    // The stack owns the widget from now on; the wrapper must not delete it again.
    page->setOwnsImpl(false);

    auto* item = new QListWidgetItem(data->nav);
    item->setText(toQString(page->title()));
    item->setSizeHint(QSize(0, kNavRowHeight));

    if (!page->icon().isNull()) {
        item->setIcon(page->icon().value());
    }

    data->stack->addWidget(page_widget);
    data->pages.push_back({ page, item, true });

    if (data->nav->currentRow() < 0) {
        setCurrentPage(page);
    }

    return true;
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

void Backstage::open()
{
    auto* data = dptr();

    if (data->window == nullptr) {
        return;
    }

    const bool any_visible = std::any_of(data->pages.begin(), data->pages.end(), [](const Impl::PageEntry& entry) { return entry.visible; });

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
