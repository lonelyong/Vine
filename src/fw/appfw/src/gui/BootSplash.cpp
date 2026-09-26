#include <vine/appfw/gui/BootSplash.hpp>

#include <algorithm>
#include <cctype>

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QScreen>
#include <QRect>
#include <QSvgRenderer>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/Signal.hpp>

#include <vine/appfw/AppBuilder.hpp>
#include <vine/appfw/ProgressHost.hpp>
#include <vine/appfw/StartupProgress.hpp>

#include "Convert.hpp"
#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

/// Frame size; the width is fixed so the status line has a predictable room.
constexpr int kWidth  = 440;
constexpr int kHeight = 152;

/// Panel corner radius of the self-drawn frame.
constexpr int kRadius = 8;

/// Logo box; a logo is scaled into it, keeping its aspect ratio.
constexpr int kLogoSize = 64;

/// Fraction scale of the bar: a 1/1000 step is finer than any progress report.
constexpr int kBarRange = 1000;

/// Marks the bar as busy in progressFraction().
constexpr double kIndeterminate = -1.0;

/// The frame: a frameless top-level window that draws its own rounded panel.
///
/// Painting the panel (instead of styling the widget) is what keeps the frame readable on both themes: the colours come
/// from the palette, so the frame follows the application theme like every other window.
class SplashWindow : public QWidget
{
  public:
    explicit SplashWindow(QWidget* parent = nullptr)
      : QWidget(parent)
    {}

    /**
     * @brief Returns whether this window has painted at least once.
     *
     * @return true once paintEvent() has run, false while the window is still empty.
     */
    bool hasPainted() const noexcept
    {
        return painted_;
    }

  protected:
    void paintEvent(QPaintEvent*) override
    {
        painted_ = true;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        // Half a pixel in: the border is drawn centred on the path.
        const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setBrush(palette().window());
        painter.setPen(palette().mid().color());
        painter.drawRoundedRect(panel, kRadius, kRadius);
    }

  private:
    /// Set by the first paint; when that happens is the window system's decision, not the frame's.
    bool painted_{ false };
};

/// Loads the logo scaled into the frame's logo box, keeping its aspect ratio; a null pixmap when it cannot be read.
QPixmap loadLogo(const std::filesystem::path& file)
{
    const QString path = QString::fromStdU16String(file.u16string());

    std::string suffix = file.extension().string();
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (suffix == ".svg") {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) {
            return QPixmap();
        }

        const double dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
        QPixmap      pixmap(qRound(kLogoSize * dpr), qRound(kLogoSize * dpr));
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);

        // A vector logo has no single right size, so the box is what the frame knows: aspect-fit into it.
        QSizeF size = renderer.defaultSize();
        if (size.isEmpty()) {
            size = QSizeF(kLogoSize, kLogoSize);
        }
        size.scale(QSizeF(kLogoSize, kLogoSize), Qt::KeepAspectRatio);

        const QRectF target(QPointF((kLogoSize - size.width()) / 2.0, (kLogoSize - size.height()) / 2.0), size);
        QPainter     painter(&pixmap);
        renderer.render(&painter, target);
        return pixmap;
    }

    QPixmap pixmap(path);
    if (pixmap.isNull()) {
        return pixmap;
    }
    return pixmap.scaled(QSize(kLogoSize, kLogoSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

/// Moves a frame of its own size to the centre of the primary screen.
///
/// The primary screen is the right one for a boot: there is no user-chosen monitor yet, and a frame on a monitor that
/// happens to hold the pointer would appear behind the main window.
void centerOnPrimaryScreen(QWidget* widget)
{
    const QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        return;
    }

    const QRect available = screen->availableGeometry();
    widget->move(available.center() - QPoint(widget->width() / 2, widget->height() / 2));
}

} // namespace

VN_OBJECT_META_IMPL(BootSplash, Window)

struct BootSplash::Impl : public WindowData {
    /// Owning frame, repainted from the change handler.
    BootSplash* self = nullptr;

    QLabel*       logo     = nullptr;
    QLabel*       title    = nullptr;
    QLabel*       subtitle = nullptr;
    QLabel*       status_label = nullptr;
    QProgressBar* bar      = nullptr;

    /// Subscription to ProgressHost::changed(); held so it is cancelled with the frame.
    vn::Connection  hosts_changed{};

    /// Status line as last reported, before elision.
    QString status;

    /// Fraction shown by the bar; negative while the bar is busy.
    double fraction = kIndeterminate;

    /// Applies the frame's identity: title, subtitle and logo.
    ///
    /// Empty fields are hidden rather than left as blank rows, so a frame configured with nothing but its progress
    /// still looks deliberate. A null logo pixmap (missing or unreadable file) is treated as "no logo" for the same
    /// reason: a boot must not fail over its decoration.
    ///
    /// @param config Appearance of the frame.
    void applyConfig(const SplashConfig& config)
    {
        const QString title_text = config.title.empty() ? QCoreApplication::applicationName()
                                                        : QString::fromUtf8(config.title.data(), static_cast<int>(config.title.size()));
        title->setText(title_text);
        static_cast<QWidget*>(impl)->setWindowTitle(title_text);

        subtitle->setText(QString::fromUtf8(config.subtitle.data(), static_cast<int>(config.subtitle.size())));
        subtitle->setVisible(!subtitle->text().isEmpty());

        const QPixmap logo_pixmap = config.logo.empty() ? QPixmap() : loadLogo(config.logo);
        logo->setPixmap(logo_pixmap);
        logo->setVisible(!logo_pixmap.isNull());
    }

    /// Redraws on every progress state change, whichever thread reported it.
    ///
    /// Notifications that arrive on the application thread are handled inline, and the frame is repainted right there:
    /// the boot reports from that thread, so this is the common case, and the report would otherwise only be painted
    /// after the boot it describes had already finished. Posting to the native widget (rather than to the application)
    /// is what makes the other case safe: Qt drops a queued call whose receiver has been destroyed, and this frame is
    /// destroyed with its widget.
    ///
    /// @param data State to redraw.
    static void onStartupChanged(Impl* data)
    {
        auto* const root = static_cast<QWidget*>(data->impl);
        if (root == nullptr) {
            return;
        }

        if (QThread::currentThread() != root->thread()) {
            QMetaObject::invokeMethod(root, [data, root] { data->self->refresh(); root->repaint(); }, Qt::QueuedConnection);
            return;
        }

        data->self->refresh();

        // Repainted synchronously rather than by pumping the event queue (the host is busy loading and will not return
        // to the event loop for a while): pumping would also run the timers of everything else that is starting up, and
        // an embedded render surface drives its own attach backoff that way - letting it run before the application has
        // laid its window out makes it build a swapchain on a window that has no native handle yet.
        root->repaint();
    }
};

BootSplash::BootSplash(const SplashConfig& config)
  : Window(new Impl(), new SplashWindow(nullptr))
{
    auto* const data = dptr();
    auto* const root = impl<QWidget>();
    data->self       = this;

    root->setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint);
    root->setAttribute(Qt::WA_TranslucentBackground);
    root->setFixedSize(kWidth, kHeight);

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(0);

    auto* header = new QHBoxLayout();
    header->setSpacing(16);

    data->logo = new QLabel(root);
    data->logo->setFixedSize(kLogoSize, kLogoSize);
    data->logo->setAlignment(Qt::AlignCenter);
    header->addWidget(data->logo, 0, Qt::AlignVCenter);
    layout->addLayout(header, 0);

    auto* identity = new QVBoxLayout();
    identity->setSpacing(2);

    data->title = new QLabel(root);
    QFont title_font = data->title->font();
    if (title_font.pointSizeF() > 0.0) {
        title_font.setPointSizeF(title_font.pointSizeF() + 5.0);
    }
    title_font.setBold(true);
    data->title->setFont(title_font);
    data->title->setStyleSheet(QStringLiteral("color: palette(window-text);"));
    identity->addWidget(data->title);

    data->subtitle = new QLabel(root);
    data->subtitle->setStyleSheet(QStringLiteral("color: palette(mid);"));
    identity->addWidget(data->subtitle);
    identity->addStretch(1);
    header->addLayout(identity, 1);

    layout->addStretch(1);

    data->bar = new QProgressBar(root);
    data->bar->setFixedHeight(6);
    data->bar->setTextVisible(false);
    data->bar->setStyleSheet(QStringLiteral("QProgressBar { border: none; border-radius: 3px; background: palette(mid); }"
                                            "QProgressBar::chunk { border-radius: 3px; background: palette(highlight); }"));
    layout->addWidget(data->bar);

    data->status_label = new QLabel(root);
    data->status_label->setStyleSheet(QStringLiteral("color: palette(window-text);"));
    layout->addSpacing(8);
    layout->addWidget(data->status_label);

    data->applyConfig(config);

    data->hosts_changed = vn::appfw::ProgressHost::changed().connect([data] { Impl::onStartupChanged(data); });

    // Pick up a boot that is already reporting, so a frame created after the first stage does not wait for the next one.
    refresh();

    centerOnPrimaryScreen(root);
}

BootSplash::~BootSplash()
{
    // d is deleted by UIElement
}

String BootSplash::statusText() const
{
    return Convert::fromQString(dptr()->status);
}

double BootSplash::progressFraction() const
{
    return dptr()->fraction;
}

bool BootSplash::isIndeterminate() const
{
    return dptr()->fraction < 0.0;
}

bool BootSplash::hasPainted() const noexcept
{
    // The frame's top-level widget is the SplashWindow this class created, so the downcast is exact.
    return static_cast<const SplashWindow*>(impl<QWidget>())->hasPainted();
}

void BootSplash::refresh()
{
    auto* const       data = dptr();
    const auto* const boot = StartupProgress::current();

    if (boot == nullptr) {
        // Nothing is being reported (any more): keep the last picture, the host is about to close the frame.
        return;
    }

    const std::string text = boot->label();
    data->status           = QString::fromUtf8(text.data(), static_cast<int>(text.size()));

    // Elide by hand: a plugin name can be longer than the frame, and QLabel would simply widen it.
    const QFontMetrics metrics(data->status_label->font());
    data->status_label->setText(metrics.elidedText(data->status, Qt::ElideMiddle, kWidth - 48));

    if (boot->isCounted()) {
        data->fraction = std::clamp(boot->fraction(), 0.0, 1.0);
        data->bar->setRange(0, kBarRange);
        data->bar->setValue(static_cast<int>(data->fraction * kBarRange));
    }
    else {
        // Busy: what is happening is known, how long it takes is not.
        data->fraction = kIndeterminate;
        data->bar->setRange(0, 0);
    }
}

inline auto BootSplash::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto BootSplash::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
