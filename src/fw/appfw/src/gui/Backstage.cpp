#include <vine/appfw/gui/Backstage.hpp>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include <QAction>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
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
    String                    id;
    String                    title;
    Icon                      icon;
    std::vector<RibbonAction*> items;
};

BackstagePage::BackstagePage()
  : Control(new Impl(), new QWidget())
{
    auto* page   = impl<QWidget>();
    auto* layout = new QVBoxLayout(page);

    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(8);
    layout->addStretch(1); // item buttons are inserted before the stretch
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

    auto* page   = impl<QWidget>();
    auto* layout = page != nullptr ? qobject_cast<QVBoxLayout*>(page->layout()) : nullptr;
    auto* action = item->impl<QAction>();

    if (layout == nullptr || action == nullptr) {
        return;
    }

    auto* button = new QToolButton(page);

    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setAutoRaise(true);
    button->setIconSize(QSize(24, 24));

    // The stretch stays last, so appended items keep their order.
    layout->insertWidget(layout->count() - 1, button);

    dptr()->items.push_back(item);
}

int BackstagePage::numItems() const
{
    return (int)dptr()->items.size();
}

RibbonAction* BackstagePage::itemAt(int index) const
{
    return (index >= 0 && index < (int)dptr()->items.size()) ? dptr()->items[(size_t)index] : nullptr;
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
    std::vector<BackstagePage*> pages;
    BackstageWindow*            window   = nullptr;
    QWidget*                    main     = nullptr;
    QListWidget*                nav      = nullptr;
    QStackedWidget*             stack    = nullptr;
    QLabel*                     title    = nullptr;
    EventArgs                   args;
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

    auto* nav = new QListWidget(native);
    nav->setFixedWidth(220);
    nav->setIconSize(QSize(24, 24));
    nav->setSpacing(2);
    nav->setFrameShape(QFrame::NoFrame);
    nav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* right        = new QWidget(native);
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(0);

    auto* header        = new QWidget(right);
    auto* header_layout = new QHBoxLayout(header);
    header_layout->setContentsMargins(16, 12, 16, 12);
    header_layout->setSpacing(8);

    auto* back = new QPushButton(QStringLiteral("\u2190"), header);
    back->setFlat(true);
    back->setFixedWidth(32);
    back->setToolTip(QStringLiteral("返回 (Esc)"));

    auto* title = new QLabel(header);
    auto  font  = title->font();
    font.setBold(true);
    font.setPointSize(font.pointSize() + 6);
    title->setFont(font);

    header_layout->addWidget(back);
    header_layout->addWidget(title);
    header_layout->addStretch(1);

    auto* stack = new QStackedWidget(right);

    right_layout->addWidget(header);
    right_layout->addWidget(stack, 1);

    root->addWidget(nav);
    root->addWidget(right, 1);

    data->nav   = nav;
    data->stack = stack;
    data->title = title;

    QObject::connect(back, &QPushButton::clicked, native, [native] { native->hide(); });

    QObject::connect(nav, &QListWidget::currentRowChanged, native, [data](int row) {
        if (row < 0 || row >= (int)data->pages.size()) {
            return;
        }

        data->stack->setCurrentIndex(row);
        data->title->setText(toQString(data->pages[(size_t)row]->title()));
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
    if (std::find(data->pages.begin(), data->pages.end(), page) != data->pages.end()) {
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

    if (!page->icon().isNull()) {
        item->setIcon(page->icon().value());
    }

    data->stack->addWidget(page_widget);
    data->pages.push_back(page);

    if (data->pages.size() == 1) {
        setCurrentPage(page);
    }

    return true;
}

BackstagePage* Backstage::findPage(const String& id) const
{
    if (id.empty()) {
        return nullptr;
    }

    for (BackstagePage* page : dptr()->pages) {
        if (page->id() == id) {
            return page;
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
    return (index >= 0 && index < (int)dptr()->pages.size()) ? dptr()->pages[(size_t)index] : nullptr;
}

void Backstage::setCurrentPage(BackstagePage* page)
{
    if (page == nullptr) {
        return;
    }

    int index = -1;

    for (int i = 0; i < (int)dptr()->pages.size(); ++i) {
        if (dptr()->pages[(size_t)i] == page) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        return;
    }

    dptr()->nav->setCurrentRow(index);
    dptr()->stack->setCurrentIndex(index);
    dptr()->title->setText(toQString(page->title()));
}

BackstagePage* Backstage::currentPage() const
{
    const int row = dptr()->nav != nullptr ? dptr()->nav->currentRow() : -1;

    return (row >= 0 && row < (int)dptr()->pages.size()) ? dptr()->pages[(size_t)row] : nullptr;
}

void Backstage::open()
{
    auto* data = dptr();

    if (data->window == nullptr) {
        return;
    }

    if (data->pages.empty()) {
        VN_LOGW("Backstage::open: no page was added, the page is not shown");
        return;
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
