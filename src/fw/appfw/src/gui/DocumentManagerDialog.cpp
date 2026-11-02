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
#include "GuiText.hpp"
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
        return translate("appfw", "— (no source)");
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
    root->setWindowTitle(translate("appfw", "Document manager"));

    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(10, 10, 10, 8);
    lay->setSpacing(8);

    data->filter = new QLineEdit(root);
    data->filter->setPlaceholderText(translate("appfw", "Filter by type / plugin / payload / title / source…"));
    data->filter->setClearButtonEnabled(true);
    lay->addWidget(data->filter);

    // Registered types: what exists, who contributed it, and what it can be built from. The payload column is the one
    // that answers "why does this file not open" - it lists what an opener registered for the type accepts.
    data->types_label = new QLabel(translate("appfw", "Registered types"), root);
    lay->addWidget(data->types_label);

    data->types_table = new QTableWidget(0, 6, root);
    data->types_table->setHorizontalHeaderLabels({ translate("appfw", "Type"),
                                                   translate("appfw", "Display name"),
                                                   translate("appfw", "Plugin"),
                                                   translate("appfw", "Can create"),
                                                   translate("appfw", "Accepts payloads"),
                                                   translate("appfw", "Source scheme") });
    data->types_table->horizontalHeader()->setStretchLastSection(true);
    data->types_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->types_table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->types_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->types_table->setMinimumHeight(140);
    detail::blendIntoSurface(data->types_table);
    lay->addWidget(data->types_table, 1);

    // Open documents: the same list a host shows as tabs, plus the two things a tab cannot show - the source it is
    // backed by and whether it holds unsaved changes.
    data->documents_label = new QLabel(translate("appfw", "Open documents"), root);
    lay->addWidget(data->documents_label);

    data->documents_table = new QTableWidget(0, 4, root);
    data->documents_table->setHorizontalHeaderLabels(
        { translate("appfw", "Title"), translate("appfw", "Type"), translate("appfw", "Source"), translate("appfw", "State") });
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

    auto* refresh_btn = new QPushButton(translate("appfw", "Refresh"), root);
    auto* close_btn   = new QPushButton(translate("appfw", "Close"), root);
    refresh_btn->setToolTip(translate("appfw", "List the registered types and the open documents again."));
    close_btn->setToolTip(
        translate("appfw",
                  "Document types cannot be switched here: who can use one is decided by the plugin that registers it (disable or skip that plugin)."));
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
        data->types_label->setText(translate("appfw", "Registered types (0)"));
        data->documents_label->setText(translate("appfw", "Open documents (0)"));
        data->message_label->setText(translate("appfw", "There is no document manager."));
        return;
    }

    const std::vector<vn::appfw::DocumentTypeInfo> types = data->manager->types();
    for (const auto& info : types) {
        const int row = data->types_table->rowCount();
        data->types_table->insertRow(row);

        data->types_table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(info.type_id)));
        data->types_table->setItem(row,
                                   1,
                                   new QTableWidgetItem(info.display_name.empty() ? Convert::toQString(info.type_id) : Convert::toQString(info.display_name)));
        data->types_table->setItem(row, 2, new QTableWidgetItem(info.owner.empty() ? translate("appfw", "Host") : Convert::toQString(info.owner)));
        data->types_table->setItem(row, 3, new QTableWidgetItem(info.can_create ? translate("appfw", "Yes") : translate("appfw", "No (open only)")));
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
            title_item->setToolTip(translate("appfw", "Current document"));
        }
        data->documents_table->setItem(row, 0, title_item);
        data->documents_table->setItem(row, 1, new QTableWidgetItem(Convert::toQString(document->typeId())));
        data->documents_table->setItem(row, 2, new QTableWidgetItem(sourceText(document->source())));
        data->documents_table->setItem(row, 3, new QTableWidgetItem(document->isDirty() ? translate("appfw", "Unsaved") : translate("appfw", "Saved")));
    }

    data->types_label->setText(translate("appfw", "Registered types (%1)").arg(types.size()));
    data->documents_label->setText(translate("appfw", "Open documents (%1)").arg(documents.size()));
    data->message_label->setText(translate("appfw",
                                           "Document types are registered by plugins; what can be opened follows from the accepted payloads. This view is "
                                           "read-only - who can use a type is up to the plugin that registers it."));

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
