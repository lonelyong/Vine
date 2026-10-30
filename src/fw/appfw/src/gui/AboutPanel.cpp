#include <vine/appfw/gui/AboutPanel.hpp>

#include <QApplication>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/gui/Icon.hpp>

#include "ControlData.hpp"
#include "Convert.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(AboutPanel, Control)

namespace
{

/// @brief Edge length of the logo in the identity row.
constexpr int kLogoSize = 48;

/// @brief Style of a secondary line: follows the theme's mid colour instead of pinning one.
QString secondaryStyle()
{
    return QStringLiteral("color: palette(mid);");
}

/**
 * @brief Resolves the info's logo, falling back to the application's window icon.
 *
 * @param path Logo path from the info (file or Qt resource); empty means "use the window icon".
 * @return The icon to draw; a null icon when neither is set.
 */
Icon resolveLogo(const std::filesystem::path& path)
{
    if (!path.empty()) {
        const std::u8string text = path.u8string();
        return Icon(String::fromUtf8(std::string_view(reinterpret_cast<const char*>(text.data()), text.size())));
    }

    if (qApp != nullptr && !qApp->windowIcon().isNull()) {
        return Icon(qApp->windowIcon());
    }

    return {};
}

} // namespace

struct AboutPanel::Impl : public ControlData {
    AppInfo info;

    QLabel*      logo_label        = nullptr;
    QLabel*      name_label        = nullptr;
    QLabel*      version_label     = nullptr;
    QLabel*      description_label = nullptr;
    QLabel*      copyright_label   = nullptr;
    QFrame*      rule              = nullptr;
    QFormLayout* rows              = nullptr;
    QVBoxLayout* links             = nullptr;
    QLabel*      credits           = nullptr;

    /// @brief Drops everything that came from the previous info (facts, links, credits).
    void clearDetails()
    {
        while (rows != nullptr && rows->rowCount() > 0) {
            rows->removeRow(0);
        }

        while (links != nullptr && links->count() > 0) {
            QLayoutItem* item = links->takeAt(0);
            delete item->widget();
            delete item;
        }

        if (credits != nullptr) {
            credits->clear();
            credits->hide();
        }
    }

    /// @brief Pushes the current info into the widget tree.
    void applyContent()
    {
        if (name_label != nullptr) {
            name_label->setText(Convert::toQString(info.name));
        }
        if (version_label != nullptr) {
            version_label->setText(Convert::toQString(info.version));
            version_label->setVisible(!info.version.empty());
        }
        if (description_label != nullptr) {
            description_label->setText(Convert::toQString(info.description));
            description_label->setVisible(!info.description.empty());
        }
        if (copyright_label != nullptr) {
            QString line = Convert::toQString(info.copyright);
            if (!info.license.empty()) {
                line = line.isEmpty() ? Convert::toQString(info.license)
                                      : line + QStringLiteral(" · ") + Convert::toQString(info.license);
            }
            copyright_label->setText(line);
            copyright_label->setVisible(!line.isEmpty());
        }
        if (logo_label != nullptr) {
            const Icon icon = resolveLogo(info.logo);
            logo_label->setVisible(!icon.isNull());
            logo_label->setPixmap(icon.isNull() ? QPixmap() : icon.value().pixmap(kLogoSize, kLogoSize));
        }

        clearDetails();

        if (rows != nullptr) {
            for (const AppInfoRow& row : info.build_facts) {
                if (row.label.empty()) {
                    continue;
                }

                auto* label = new QLabel(Convert::toQString(row.label));
                label->setStyleSheet(secondaryStyle());

                auto* value = new QLabel(Convert::toQString(row.value));
                value->setTextInteractionFlags(Qt::TextSelectableByMouse);

                rows->addRow(label, value);
            }
        }

        if (links != nullptr) {
            for (const AppInfoLink& entry : info.links) {
                if (entry.text.empty() || entry.url.empty()) {
                    continue;
                }

                auto* link = new QLabel(QStringLiteral("<a href=\"%1\">%2</a>")
                                          .arg(Convert::toQString(entry.url).toHtmlEscaped(),
                                               Convert::toQString(entry.text).toHtmlEscaped()));
                link->setTextFormat(Qt::RichText);
                link->setOpenExternalLinks(true);
                link->setTextInteractionFlags(Qt::TextBrowserInteraction);

                links->addWidget(link);
            }
        }

        if (credits != nullptr) {
            credits->setText(Convert::toQString(info.credits));
            credits->setVisible(!info.credits.empty());
        }

        // The rule is the seam between "who this is" and "the details": without details it would float.
        if (rule != nullptr) {
            const bool details = (rows != nullptr && rows->rowCount() > 0) || (links != nullptr && links->count() > 0)
                              || (credits != nullptr && credits->isVisible());
            rule->setVisible(details);
        }
    }
};

AboutPanel::AboutPanel()
  : Control(new Impl(), new QWidget())
{
    auto* root = impl<QWidget>();
    auto* data = dptr();

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(12);

    auto* identity = new QHBoxLayout();
    identity->setSpacing(16);

    data->logo_label = new QLabel(root);
    data->logo_label->setFixedSize(kLogoSize, kLogoSize);
    data->logo_label->setScaledContents(true);
    identity->addWidget(data->logo_label, 0, Qt::AlignTop);

    auto* titles = new QVBoxLayout();
    titles->setSpacing(4);

    auto* name_row = new QHBoxLayout();
    name_row->setSpacing(10);

    data->name_label = new QLabel(root);
    QFont name_font  = data->name_label->font();
    name_font.setPointSizeF(name_font.pointSizeF() + 8.0);
    name_font.setWeight(QFont::DemiBold);
    data->name_label->setFont(name_font);

    data->version_label = new QLabel(root);
    data->version_label->setStyleSheet(secondaryStyle());
    data->version_label->setAlignment(Qt::AlignBottom);

    name_row->addWidget(data->name_label);
    name_row->addWidget(data->version_label);
    name_row->addStretch(1);

    data->description_label = new QLabel(root);
    data->description_label->setWordWrap(true);

    titles->addLayout(name_row);
    titles->addWidget(data->description_label);
    titles->addStretch(1);

    identity->addLayout(titles, 1);
    layout->addLayout(identity);

    data->rule = new QFrame(root);
    data->rule->setFixedHeight(1);
    data->rule->setFrameShape(QFrame::NoFrame);
    data->rule->setAttribute(Qt::WA_StyledBackground, true);
    data->rule->setStyleSheet(QStringLiteral("background-color: palette(mid);"));
    layout->addWidget(data->rule);

    auto* rows_host = new QWidget(root);
    data->rows      = new QFormLayout(rows_host);
    data->rows->setContentsMargins(0, 0, 0, 0);
    data->rows->setHorizontalSpacing(16);
    data->rows->setVerticalSpacing(6);
    data->rows->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(rows_host);

    auto* links_host = new QWidget(root);
    data->links      = new QVBoxLayout(links_host);
    data->links->setContentsMargins(0, 0, 0, 0);
    data->links->setSpacing(4);
    layout->addWidget(links_host);

    data->credits = new QLabel(root);
    data->credits->setTextFormat(Qt::PlainText);
    data->credits->setWordWrap(true);
    data->credits->setTextInteractionFlags(Qt::TextSelectableByMouse);
    data->credits->setStyleSheet(secondaryStyle());
    QFont credits_font = data->credits->font();
    credits_font.setStyleHint(QFont::Monospace);
    credits_font.setFamily(QStringLiteral("monospace"));
    credits_font.setPointSizeF(credits_font.pointSizeF() - 0.5);
    data->credits->setFont(credits_font);
    data->credits->hide();
    layout->addWidget(data->credits);

    layout->addStretch(1);

    data->copyright_label = new QLabel(root);
    data->copyright_label->setStyleSheet(secondaryStyle());
    layout->addWidget(data->copyright_label);

    // The panel shows the application's own identity unless a host says otherwise.
    if (auto* app = Application::current(); app != nullptr) {
        data->info = app->appInfo();
    }

    data->applyContent();
}

AboutPanel::~AboutPanel()
{
    // d is released by UIElement; the widgets are the panel's own children.
}

void AboutPanel::setAppInfo(const AppInfo& info)
{
    dptr()->info = info;
    dptr()->applyContent();
}

const AppInfo& AboutPanel::appInfo() const
{
    return dptr()->info;
}

inline auto AboutPanel::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto AboutPanel::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
