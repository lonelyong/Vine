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
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"
#include "TableStyle.hpp"

V_APPFWGUI_NS_BEGIN

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

} // namespace

V_OBJECT_META_IMPL(CommandManagerDialog, Window)

struct CommandManagerDialog::Impl : public UIElementData {
    vine::appfw::CommandManager* manager       = nullptr;
    QLineEdit*                   filter        = nullptr;
    QTableWidget*                table         = nullptr;
    QPushButton*                 toggle_btn    = nullptr;
    QLabel*                      message_label = nullptr;
};

CommandManagerDialog::CommandManagerDialog(vine::appfw::CommandManager* manager)
  : Window(new Impl(), new QDialog())
{
    auto* data    = dptr();
    data->manager = manager;

    auto* root = impl<QDialog>();
    root->setWindowTitle(QStringLiteral("命令管理器"));

    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(8);

    data->filter = new QLineEdit(root);
    data->filter->setPlaceholderText(QStringLiteral("筛选名称 / 别名 / 来源插件 / 分组 / 描述…"));
    data->filter->setClearButtonEnabled(true);
    lay->addWidget(data->filter);

    data->table = new QTableWidget(0, 6, root);
    data->table->setHorizontalHeaderLabels({ QStringLiteral("名称"), QStringLiteral("状态"), QStringLiteral("别名"), QStringLiteral("来源插件"), QStringLiteral("分组"), QStringLiteral("描述") });
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

    auto* refresh_btn = new QPushButton(QStringLiteral("刷新"), root);
    data->toggle_btn  = new QPushButton(QStringLiteral("禁用命令"), root);
    auto* close_btn   = new QPushButton(QStringLiteral("关闭"), root);
    refresh_btn->setToolTip(QStringLiteral("重新列出已注册的命令。"));
    data->toggle_btn->setToolTip(QStringLiteral("禁用的命令仍留在管理器里（只是不能执行），随时可以再启用；该选择重启后依然有效。"));
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
}

CommandManagerDialog::~CommandManagerDialog()
{
    // d is released by UIElement
}

void CommandManagerDialog::refresh()
{
    auto* data = dptr();
    data->table->setRowCount(0);
    if (!data->manager) {
        updateActions();
        return;
    }

    for (const auto& info : data->manager->commandInfos()) {
        const int row = data->table->rowCount();
        data->table->insertRow(row);

        data->table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(info.name)));
        data->table->setItem(row, 1, new QTableWidgetItem(info.enabled ? QStringLiteral("启用") : QStringLiteral("已禁用")));

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

        const QString owner = info.owner.empty() ? QStringLiteral("主机") : Convert::toQString(info.owner);
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
    updateActions();
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

    const auto* u16   = name_item->text().utf16();
    const String name = String::fromUtf16(reinterpret_cast<const char16_t*>(u16), name_item->text().size());

    // Disabling is a flag on the registration, never a removal, so the command keeps
    // its metadata and its aliases and can be enabled again at any time.
    const bool enable = !data->manager->isCommandEnabled(name);
    data->manager->setCommandEnabled(name, enable);

    data->message_label->setText(enable ? QStringLiteral("已启用命令“%1”。").arg(name_item->text())
                                        : QStringLiteral("已禁用命令“%1”（重启后仍保持禁用）。").arg(name_item->text()));
    refresh();
}

void CommandManagerDialog::updateActions()
{
    auto*       data = dptr();
    const int   row  = data->table->currentRow();
    const auto* item = row >= 0 ? data->table->item(row, 0) : nullptr;

    data->toggle_btn->setEnabled(item != nullptr);
    if (item == nullptr) {
        data->toggle_btn->setText(QStringLiteral("禁用命令"));
        return;
    }

    const auto* u16   = item->text().utf16();
    const String name = String::fromUtf16(reinterpret_cast<const char16_t*>(u16), item->text().size());
    const bool   enabled = data->manager != nullptr && data->manager->isCommandEnabled(name);

    data->toggle_btn->setText(enabled ? QStringLiteral("禁用命令") : QStringLiteral("启用命令"));
}

inline auto CommandManagerDialog::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto CommandManagerDialog::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
