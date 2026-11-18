#include <vine/appfw/gui/CommandManagerDialog.hpp>

#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/Signal.hpp>

#include "Convert.hpp"
#include "GuiText.hpp"
#include "TableStyle.hpp"
#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

/// Renders a table row as disabled: the command stays listed but cannot run.
void greyOutRow(QTableWidget* table, int row)
{
    QFont font = table->font();
    font.setItalic(true);
    for (int col = 0; col < table->columnCount(); ++col) {
        if (auto* item = table->item(row, col)) {
            item->setFont(font);
            item->setForeground(QBrush(QColor(140, 140, 140)));
        }
    }
}

/// The command name a table row holds (column 0), or an empty name for a row without one.
String nameOfItem(const QTableWidgetItem* item)
{
    if (item == nullptr) {
        return {};
    }
    const auto* text = item->text().utf16();
    return String::fromUtf16(reinterpret_cast<const char16_t*>(text), item->text().size());
}

} // namespace

VN_OBJECT_META_IMPL(CommandManagerDialog, Window)

struct CommandManagerDialog::Impl : public WindowData {
    vn::appfw::CommandManager* manager       = nullptr;
    QLineEdit*                 filter        = nullptr;
    QTableWidget*              table         = nullptr;
    QPushButton*               toggle_btn    = nullptr;
    QLabel*                    message_label = nullptr;
    /// Connection on the manager's commandsChanged(), dropped by the destructor.
    vn::Connection commands_handler{};

    /// The command name the current row holds, or an empty name when nothing is selected.
    String selectedName() const;
    /// Selects the row of the named command, if it is listed and visible.
    void selectByName(const String& name);
};

CommandManagerDialog::CommandManagerDialog(vn::appfw::CommandManager* manager)
  : Window(new Impl(), new QDialog())
{
    auto* data    = dptr();
    data->manager = manager;

    auto* root = impl<QDialog>();
    root->setWindowTitle(translate("appfw", "Command manager"));

    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(8);

    data->filter = new QLineEdit(root);
    data->filter->setPlaceholderText(translate("appfw", "Filter by name / alias / plugin / group / description…"));
    data->filter->setClearButtonEnabled(true);
    lay->addWidget(data->filter);

    data->table = new QTableWidget(0, 6, root);
    data->table->setHorizontalHeaderLabels({ translate("appfw", "Name"),
                                             translate("appfw", "State"),
                                             translate("appfw", "Alias"),
                                             translate("appfw", "Plugin"),
                                             translate("appfw", "Group"),
                                             translate("appfw", "Description") });
    data->table->horizontalHeader()->setStretchLastSection(true);
    data->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->table->setMinimumHeight(160);
    detail::blendIntoSurface(data->table);
    lay->addWidget(data->table, 1);

    // Footer: what just happened on the left, the actions on the right (the same
    // shape as the plugin manager dialog).
    auto* bottom_lay = new QHBoxLayout();
    bottom_lay->setSpacing(8);

    data->message_label = new QLabel(root);
    data->message_label->setWordWrap(true);
    data->message_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottom_lay->addWidget(data->message_label, 1);

    auto* refresh_btn = new QPushButton(translate("appfw", "Refresh"), root);
    data->toggle_btn  = new QPushButton(translate("appfw", "Disable command"), root);
    auto* close_btn   = new QPushButton(translate("appfw", "Close"), root);
    refresh_btn->setToolTip(translate("appfw", "List the registered commands again."));
    data->toggle_btn->setToolTip(
        translate("appfw",
                  "A disabled command stays in the manager (it just cannot run) and can be enabled again at any time; the choice survives a restart."));
    bottom_lay->addWidget(refresh_btn);
    bottom_lay->addWidget(data->toggle_btn);
    bottom_lay->addWidget(close_btn);
    lay->addLayout(bottom_lay);

    QObject::connect(data->filter, &QLineEdit::textChanged, root, [this] { applyFilter(); });
    QObject::connect(refresh_btn, &QPushButton::clicked, root, [this] { refresh(); });
    QObject::connect(data->toggle_btn, &QPushButton::clicked, root, [this] { toggleSelectedEnabled(); });
    QObject::connect(close_btn, &QPushButton::clicked, root, [root] { root->close(); });

    // The toggle follows the selected row: disabled (not hidden) while nothing is
    // selected, because a vanishing button would flicker as the selection moves.
    QObject::connect(data->table, &QTableWidget::itemSelectionChanged, root, [this] { updateActions(); });

    refresh();

    // The list follows the command set while the dialog is open, like the console completion does: a plugin that
    // registers or drops a command must not need the Refresh button. The event fires on the thread that changed the
    // set, while the rows are widgets - so off the application thread it is posted, and the queued call checks the
    // dialog is still there (it captures the widget, not the Impl).
    if (data->manager != nullptr) {
        data->commands_handler = data->manager->commandsChanged.connect([this](vn::appfw::CommandManager&, vn::EventArgs&) {
            if (MainThreadDispatcher::isMainThread()) {
                refresh();
                return;
            }
            QPointer<QDialog> alive = impl<QDialog>();
            static_cast<void>(MainThreadDispatcher::postToMainThread([this, alive] {
                if (!alive.isNull()) {
                    refresh();
                }
            }));
        });
    }
}

CommandManagerDialog::~CommandManagerDialog()
{
    dptr()->commands_handler.disconnect();
    // d is released by UIElement
}

void CommandManagerDialog::refresh()
{
    auto* data = dptr();

    // The selection is remembered by NAME: the table is a plain projection of commandInfos() and is rebuilt from
    // scratch, so toggling a command would otherwise drop the row the user is working on.
    const String selected = data->selectedName();

    data->table->setRowCount(0);
    if (!data->manager) {
        updateActions();
        return;
    }

    for (const auto& info : data->manager->commandInfos()) {
        const int row = data->table->rowCount();
        data->table->insertRow(row);

        data->table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(info.name)));
        data->table->setItem(row, 1, new QTableWidgetItem(info.enabled ? translate("appfw", "Enabled") : translate("appfw", "Disabled")));

        QString aliases;
        bool    first = true;
        for (const auto& alias : info.aliases) {
            if (!first) {
                aliases += QStringLiteral(", ");
            }
            aliases += Convert::toQString(alias);
            first = false;
        }
        data->table->setItem(row, 2, new QTableWidgetItem(aliases));

        const QString owner = info.owner.empty() ? translate("appfw", "Host") : Convert::toQString(info.owner);
        data->table->setItem(row, 3, new QTableWidgetItem(owner));
        data->table->setItem(row, 4, new QTableWidgetItem(Convert::toQString(info.group)));
        data->table->setItem(row, 5, new QTableWidgetItem(Convert::toQString(info.description)));

        // A disabled command stays listed: it is dimmed rather than removed, so the
        // user can find it again and enable it.
        if (!info.enabled) {
            greyOutRow(data->table, row);
        }
    }

    applyFilter();
    data->selectByName(selected);
    updateActions();
}

String CommandManagerDialog::Impl::selectedName() const
{
    const int row = table->currentRow();
    return row >= 0 ? nameOfItem(table->item(row, 0)) : String{};
}

void CommandManagerDialog::Impl::selectByName(const String& name)
{
    if (name.empty()) {
        return;
    }

    const QString wanted = Convert::toQString(name);
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* item = table->item(row, 0);
        if (item != nullptr && item->text() == wanted && !table->isRowHidden(row)) {
            table->selectRow(row);
            return;
        }
    }
}

void CommandManagerDialog::applyFilter()
{
    auto*         data   = dptr();
    const QString needle = data->filter->text().trimmed();

    for (int row = 0; row < data->table->rowCount(); ++row) {
        bool match = needle.isEmpty();
        for (int col = 0; col < data->table->columnCount() && !match; ++col) {
            if (const auto* item = data->table->item(row, col)) {
                match = item->text().contains(needle, Qt::CaseInsensitive);
            }
        }
        data->table->setRowHidden(row, !match);
    }
}

void CommandManagerDialog::toggleSelectedEnabled()
{
    auto* data = dptr();
    if (!data->manager) {
        return;
    }

    const int row = data->table->currentRow();
    if (row < 0) {
        return;
    }

    const auto* name_item = data->table->item(row, 0);
    if (!name_item) {
        return;
    }

    // Taken out of the table before the manager is touched: setCommandEnabled() fires commandsChanged(), the dialog
    // refreshes itself from it, and every item of the old rows - this one included - is gone by the time it returns.
    const String  name  = nameOfItem(name_item);
    const QString label = name_item->text();

    // Disabling is a flag on the registration, never a removal, so the command keeps
    // its metadata and its aliases and can be enabled again at any time.
    const bool enable = !data->manager->isCommandEnabled(name);
    data->manager->setCommandEnabled(name, enable);

    data->message_label->setText(enable ? translate("appfw", "Enabled the command \"%1\".").arg(label)
                                        : translate("appfw", "Disabled the command \"%1\" (it stays disabled after a restart).").arg(label));
    refresh();
}

void CommandManagerDialog::updateActions()
{
    auto*       data = dptr();
    const int   row  = data->table->currentRow();
    const auto* item = row >= 0 ? data->table->item(row, 0) : nullptr;

    data->toggle_btn->setEnabled(item != nullptr);
    if (item == nullptr) {
        data->toggle_btn->setText(translate("appfw", "Disable command"));
        return;
    }

    const bool enabled = data->manager != nullptr && data->manager->isCommandEnabled(nameOfItem(item));
    data->toggle_btn->setText(enabled ? translate("appfw", "Disable command") : translate("appfw", "Enable command"));
}

inline auto CommandManagerDialog::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto CommandManagerDialog::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
