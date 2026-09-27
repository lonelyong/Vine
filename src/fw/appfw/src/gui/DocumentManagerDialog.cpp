#include <vine/appfw/gui/DocumentManagerDialog.hpp>

#include <algorithm>
#include <vector>

#include <QAbstractItemView>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Convert.hpp"
#include "TableStyle.hpp"
#include "WindowData.hpp"

#include <vine/appfw/Document.hpp>

VN_APPFWGUI_NS_BEGIN

namespace
{

/// Joins a list of names for one table cell (an empty list reads as a dash, not as a blank).
QString joinOrDash(const std::vector<String>& names)
{
    if (names.empty()) {
        return QStringLiteral("—");
    }

    QString joined;
    bool    first = true;
    for (const auto& name : names) {
        if (!first) {
            joined += QStringLiteral(", ");
        }
        joined += Convert::toQString(name);
        first = false;
    }
    return joined;
}

/// The source of a document as one cell: "scheme:address", or a dash when it is backed by nothing.
QString sourceText(const vn::appfw::DocumentSource& source)
{
    if (!source.valid()) {
        return QStringLiteral("—（没有来源）");
    }
    return QStringLiteral("%1:%2").arg(Convert::toQString(source.scheme), Convert::toQString(source.address));
}

} // namespace

VN_OBJECT_META_IMPL(DocumentManagerDialog, Window)

struct DocumentManagerDialog::Impl : public WindowData {
    vn::appfw::DocumentManager* manager         = nullptr;
    QLineEdit*                  filter          = nullptr;
    QLabel*                     types_label     = nullptr;
    QTableWidget*               types_table     = nullptr;
    QLabel*                     documents_label = nullptr;
    QTableWidget*               documents_table = nullptr;
    QLabel*                     message_label   = nullptr;
};

DocumentManagerDialog::DocumentManagerDialog(vn::appfw::DocumentManager* manager)
  : Window(new Impl(), new QDialog())
{
    auto* data    = dptr();
    data->manager = manager;

    auto* root = impl<QDialog>();
    root->setWindowTitle(QStringLiteral("文档管理器"));

    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(8);

    data->filter = new QLineEdit(root);
    data->filter->setPlaceholderText(QStringLiteral("筛选类型 / 来源插件 / 载荷 / 标题 / 来源…"));
    data->filter->setClearButtonEnabled(true);
    lay->addWidget(data->filter);

    // Registered types: what exists, who contributed it, and what it can be built from. The payload column is the one
    // that answers "why does this file not open" - it lists what an opener registered for the type accepts.
    data->types_label = new QLabel(QStringLiteral("已注册的类型"), root);
    lay->addWidget(data->types_label);

    data->types_table = new QTableWidget(0, 6, root);
    data->types_table->setHorizontalHeaderLabels({ QStringLiteral("类型"), QStringLiteral("显示名"),
                                                   QStringLiteral("来源插件"), QStringLiteral("可新建"),
                                                   QStringLiteral("接受的载荷"), QStringLiteral("来源方案") });
    data->types_table->horizontalHeader()->setStretchLastSection(true);
    data->types_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->types_table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->types_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->types_table->setMinimumHeight(140);
    detail::blendIntoSurface(data->types_table);
    lay->addWidget(data->types_table, 1);

    // Open documents: the same list a host shows as tabs, plus the two things a tab cannot show - the source it is
    // backed by and whether it holds unsaved changes.
    data->documents_label = new QLabel(QStringLiteral("已打开的文档"), root);
    lay->addWidget(data->documents_label);

    data->documents_table = new QTableWidget(0, 4, root);
    data->documents_table->setHorizontalHeaderLabels({ QStringLiteral("标题"), QStringLiteral("类型"),
                                                       QStringLiteral("来源"), QStringLiteral("状态") });
    data->documents_table->horizontalHeader()->setStretchLastSection(true);
    data->documents_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->documents_table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->documents_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->documents_table->setMinimumHeight(120);
    detail::blendIntoSurface(data->documents_table);
    lay->addWidget(data->documents_table, 1);

    auto* bottom_lay = new QHBoxLayout();
    bottom_lay->setSpacing(8);

    data->message_label = new QLabel(root);
    data->message_label->setWordWrap(true);
    data->message_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottom_lay->addWidget(data->message_label, 1);

    auto* refresh_btn = new QPushButton(QStringLiteral("刷新"), root);
    auto* close_btn   = new QPushButton(QStringLiteral("关闭"), root);
    refresh_btn->setToolTip(QStringLiteral("重新列出已注册的类型与已打开的文档。"));
    close_btn->setToolTip(QStringLiteral("文档类型不能在这里开关：谁能用由注册它的插件决定（禁用或跳过插件）。"));
    bottom_lay->addWidget(refresh_btn);
    bottom_lay->addWidget(close_btn);
    lay->addLayout(bottom_lay);

    QObject::connect(data->filter, &QLineEdit::textChanged, root, [this] { applyFilter(); });
    QObject::connect(refresh_btn, &QPushButton::clicked, root, [this] { refresh(); });
    QObject::connect(close_btn, &QPushButton::clicked, root, [root] { root->close(); });

    refresh();
}

DocumentManagerDialog::~DocumentManagerDialog()
{
    // d is released by UIElement
}

void DocumentManagerDialog::refresh()
{
    auto* data = dptr();
    data->types_table->setRowCount(0);
    data->documents_table->setRowCount(0);
    if (!data->manager) {
        data->types_label->setText(QStringLiteral("已注册的类型（0）"));
        data->documents_label->setText(QStringLiteral("已打开的文档（0）"));
        data->message_label->setText(QStringLiteral("没有文档管理器。"));
        return;
    }

    const std::vector<vn::appfw::DocumentTypeInfo> types = data->manager->types();
    for (const auto& info : types) {
        const int row = data->types_table->rowCount();
        data->types_table->insertRow(row);

        data->types_table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(info.type_id)));
        data->types_table->setItem(row, 1, new QTableWidgetItem(info.display_name.empty()
                                                                    ? Convert::toQString(info.type_id)
                                                                    : Convert::toQString(info.display_name)));
        data->types_table->setItem(row, 2, new QTableWidgetItem(info.owner.empty() ? QStringLiteral("主机")
                                                                                   : Convert::toQString(info.owner)));
        data->types_table->setItem(row, 3, new QTableWidgetItem(info.can_create ? QStringLiteral("是")
                                                                               : QStringLiteral("否（只能打开）")));
        data->types_table->setItem(row, 4, new QTableWidgetItem(joinOrDash(info.payload_types)));
        data->types_table->setItem(row, 5, new QTableWidgetItem(joinOrDash(info.source_schemes)));

        if (!info.description.empty()) {
            data->types_table->item(row, 0)->setToolTip(Convert::toQString(info.description));
        }
    }

    const std::vector<vn::appfw::Document*> documents = data->manager->documents();
    for (const auto* document : documents) {
        if (document == nullptr) {
            continue;
        }
        const int row = data->documents_table->rowCount();
        data->documents_table->insertRow(row);

        const bool is_current = (document == data->manager->current());
        auto*      title_item = new QTableWidgetItem(Convert::toQString(document->title()));
        if (is_current) {
            // "Current" is a manager fact, not a view fact: the dialog says which one it is.
            title_item->setText(QStringLiteral("● %1").arg(title_item->text()));
            title_item->setToolTip(QStringLiteral("当前文档"));
        }
        data->documents_table->setItem(row, 0, title_item);
        data->documents_table->setItem(row, 1, new QTableWidgetItem(Convert::toQString(document->typeId())));
        data->documents_table->setItem(row, 2, new QTableWidgetItem(sourceText(document->source())));
        data->documents_table->setItem(row, 3, new QTableWidgetItem(document->isDirty() ? QStringLiteral("未保存")
                                                                                        : QStringLiteral("已保存")));
    }

    data->types_label->setText(QStringLiteral("已注册的类型（%1）").arg(types.size()));
    data->documents_label->setText(QStringLiteral("已打开的文档（%1）").arg(documents.size()));
    data->message_label->setText(
        QStringLiteral("文档类型由插件注册；能开什么由“接受的载荷”决定。这里只读 —— 谁能用由注册它的插件说了算。"));

    applyFilter();
}

void DocumentManagerDialog::applyFilter()
{
    auto*         data   = dptr();
    const QString needle = data->filter->text().trimmed();

    const auto filter_table = [&needle](QTableWidget* table) {
        for (int row = 0; row < table->rowCount(); ++row) {
            bool match = needle.isEmpty();
            for (int col = 0; col < table->columnCount() && !match; ++col) {
                if (const auto* item = table->item(row, col)) {
                    match = item->text().contains(needle, Qt::CaseInsensitive);
                }
            }
            table->setRowHidden(row, !match);
        }
    };

    filter_table(data->types_table);
    filter_table(data->documents_table);
}

inline auto DocumentManagerDialog::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto DocumentManagerDialog::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

VN_APPFWGUI_NS_END
