#include <vine/appfw/gui/ProgressPresenter.hpp>

#include <chrono>
#include <optional>

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <vine/Signal.hpp>

#include <vine/appfw/ProgressHost.hpp>
#include "ControlData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

/// How long an operation runs before the bar appears.
constexpr int kShowDelayMs = 400;

/// How long the bar stays visible after the operation ends.
constexpr int kHideDelayMs = 300;

} // namespace

VN_OBJECT_META_IMPL(ProgressPresenter, Control)

struct ProgressPresenter::Impl : public ControlData {
    /// Owning presenter, redrawn from the change handler.
    ProgressPresenter* self = nullptr;

    QProgressBar* bar             = nullptr;
    QLabel*       chain_label     = nullptr;
    QLabel*       background_label = nullptr;
    QPushButton*  cancel          = nullptr;
    QTimer*       timer           = nullptr;

    /// Connection to ProgressHost::changed(); held so it is cancelled with the presenter.
    vn::Connection hosts_changed{};

    /// The tracked foreground host (drives the main bar), or nullptr.
    vn::appfw::ProgressHost* foreground = nullptr;
    std::chrono::steady_clock::time_point host_seen_at{};
    std::chrono::steady_clock::time_point hide_at{};
    bool bar_visible   = false;
    bool progress_seen = false;
    bool hide_pending  = false;

    /// Redraws on every progress state change, whichever thread reported it.
    ///
    /// The registry is written by the operation, which reports from whatever thread it runs on,
    /// while the widgets may only be touched on the application thread. Notifications that arrive
    /// on the application thread redraw inline - that is the common case, and going through the
    /// event loop would only delay it - and the rest are posted to the native widget. Posting to
    /// the widget (rather than to the application) is what makes the posted call safe: Qt drops a
    /// queued call whose receiver has been destroyed, and this presenter is destroyed with its
    /// widget, so the call can neither outlive the widgets nor run after `self` is gone.
    ///
    /// @param data State to redraw.
    static void onHostsChanged(Impl* data)
    {
        auto* const root = static_cast<QWidget*>(data->impl);
        if (root == nullptr) {
            return;
        }

        if (QThread::currentThread() == root->thread()) {
            data->self->refresh();
            return;
        }

        QMetaObject::invokeMethod(root, [data] { data->self->refresh(); }, Qt::QueuedConnection);
    }
};

ProgressPresenter::ProgressPresenter(QWidget* parent)
  : Control(new Impl(), new QWidget(parent))
{
    auto* data = dptr();
    auto* root = impl<QWidget>();
    data->self = this;

    auto* layout = new QHBoxLayout(root);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(4);

    data->bar = new QProgressBar(root);
    data->bar->setRange(0, 1000);
    data->bar->setFixedWidth(160);
    data->bar->setTextVisible(false);
    layout->addWidget(data->bar);

    data->chain_label = new QLabel(root);
    data->chain_label->setVisible(false);
    layout->addWidget(data->chain_label);

    data->background_label = new QLabel(root);
    data->background_label->setVisible(false);
    layout->addWidget(data->background_label);

    data->cancel = new QPushButton(QStringLiteral("Cancel"), root);
    data->cancel->setFixedHeight(data->bar->sizeHint().height());
    layout->addWidget(data->cancel);

    // The wrapper is not a QObject, so signal contexts use the native widget.
    // The cancel button stops the foreground operation only.
    QObject::connect(data->cancel, &QPushButton::clicked, root, [data] {
        if (data->foreground) {
            data->foreground->cancelSource().request_stop();
        }
    });

    data->timer = new QTimer(root);
    data->timer->setSingleShot(true);
    QObject::connect(data->timer, &QTimer::timeout, root, [data] { data->self->refresh(); });

    data->hosts_changed = vn::appfw::ProgressHost::changed().connect([data] { Impl::onHostsChanged(data); });

    // The bar is hidden until an operation shows up; pick up an operation that is already
    // running, so that embedding the presenter mid-operation does not wait for the next change.
    data->self->refresh();

    root->setVisible(false);
}

ProgressPresenter::~ProgressPresenter()
{
    // d is deleted by UIElement
}

bool ProgressPresenter::isBusy() const
{
    // Any active host (foreground or background) keeps the presenter engaged.
    return vn::appfw::ProgressHost::isActive();
}

void ProgressPresenter::refresh()
{
    using namespace std::chrono;

    auto* const data = dptr();
    const auto  now  = steady_clock::now();
    auto* const fg    = vn::appfw::ProgressHost::current();
    const auto  hosts = vn::appfw::ProgressHost::activeHosts();
    const auto  chain = vn::appfw::ProgressHost::foregroundStack();
    // 后台宿主 = 活跃宿主中不在前台栈里的（真正并行的任务）。
    const std::size_t bg_count = hosts.size() >= chain.size() ? hosts.size() - chain.size() : 0;

    // Track the current foreground (top of stack); reset per-host state on
    // change. When a nested child takes over the bar, keep it visible instead
    // of flickering.
    if (fg != data->foreground) {
        data->foreground = fg;
        if (fg != nullptr) {
            data->host_seen_at  = now;
            data->progress_seen = false;
        }
        else {
            data->bar_visible = false;
        }
    }

    // Foreground main bar (innermost LongRunning command).
    if (fg != nullptr) {
        if (fg->indicator().position() > 0.0) {
            data->progress_seen = true;
        }
        if (!data->bar_visible) {
            const auto elapsed = duration_cast<milliseconds>(now - data->host_seen_at).count();
            if (elapsed >= kShowDelayMs) {
                data->bar_visible = true;
            }
        }
        if (data->bar_visible) {
            if (data->progress_seen) {
                const double pos = fg->indicator().position();
                data->bar->setRange(0, 1000);
                data->bar->setValue(static_cast<int>(pos * 1000));
                data->bar->setTextVisible(true);
                if (fg->label().empty()) {
                    data->bar->setFormat(QStringLiteral("%p%"));
                }
                else {
                    data->bar->setFormat(QString::fromStdString(fg->label()) + QStringLiteral(" %p%"));
                }
            }
            else {
                // No progress reported yet: show an indeterminate busy bar.
                data->bar->setRange(0, 0);
                data->bar->setTextVisible(false);
            }
        }
    }

    // Chain breadcrumb (only meaningful while a nested child is running).
    const bool has_chain = chain.size() > 1;
    data->chain_label->setVisible(has_chain);
    if (has_chain) {
        QString text;
        for (auto* h : chain) {
            if (!text.isEmpty()) {
                text += QStringLiteral(" \u25B8 ");
            }
            QString name = QString::fromStdString(h->label());
            text += name.isEmpty() ? QStringLiteral("\u2026") : name;
        }
        data->chain_label->setText(text);
    }

    // Background activity badge (parallel tasks running alongside).
    const bool has_bg = bg_count > 0;
    data->background_label->setVisible(has_bg);
    if (has_bg) {
        data->background_label->setText(QStringLiteral("后台 %1").arg(static_cast<int>(bg_count)));
    }

    // Overall visibility: foreground bar past the threshold, or background
    // activity; hide shortly after everything drains.
    const bool want_show = data->bar_visible || has_bg;
    if (want_show) {
        data->hide_pending = false;
        setVisible(true);
    }
    else if (visible()) {
        if (!data->hide_pending) {
            data->hide_pending = true;
            data->hide_at      = now + milliseconds(kHideDelayMs);
        }
        if (now >= data->hide_at) {
            setVisible(false);
            data->hide_pending = false;
        }
    }
    else {
        // Already hidden: nothing to wait for, and arming a wakeup would only make an idle
        // window wake up again to hide what is hidden.
        data->hide_pending = false;
    }

    // Arm the one timer this presenter ever holds, for the deadline that is actually pending:
    // the bar appearing, or the bar hiding. With none pending there is no timer at all.
    std::optional<steady_clock::time_point> next_due;
    if (fg != nullptr && !data->bar_visible) {
        next_due = data->host_seen_at + milliseconds(kShowDelayMs);
    }
    if (data->hide_pending && (!next_due || data->hide_at < *next_due)) {
        next_due = data->hide_at;
    }

    if (!next_due) {
        data->timer->stop();
        return;
    }

    const auto remaining = duration_cast<milliseconds>(*next_due - now).count();
    data->timer->start(remaining > 0 ? static_cast<int>(remaining) : 1);
}

inline auto ProgressPresenter::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto ProgressPresenter::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
