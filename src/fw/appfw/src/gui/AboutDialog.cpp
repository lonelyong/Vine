#include <vine/appfw/gui/AboutDialog.hpp>

#include <memory>

#include <QDialog>
#include <QVBoxLayout>
#include <QWidget>

#include <vine/appfw/gui/AboutPanel.hpp>

#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

VN_OBJECT_META_IMPL(AboutDialog, Window)

struct AboutDialog::Impl : public WindowData {
    /// The content, shared with the Backstage page that shows the same panel.
    std::unique_ptr<AboutPanel> panel;
};

AboutDialog::AboutDialog()
  : Window(new Impl(), new QDialog())
{
    auto* root = impl<QDialog>();
    root->setWindowTitle(QStringLiteral("关于"));

    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(24, 20, 24, 20);

    auto* panel = new AboutPanel();
    layout->addWidget(panel->impl<QWidget>());

    // The dialog's widget tree owns the panel's widget from here on; the wrapper must not delete it.
    panel->setOwnsImpl(false);
    dptr()->panel.reset(panel);
}

AboutDialog::~AboutDialog()
{
    // The panel wrapper goes with d; its widget is owned by the dialog and dies with it.
}

void AboutDialog::setAppInfo(const AppInfo& info)
{
    dptr()->panel->setAppInfo(info);
}

const AppInfo& AboutDialog::appInfo() const
{
    return dptr()->panel->appInfo();
}

inline auto AboutDialog::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto AboutDialog::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
