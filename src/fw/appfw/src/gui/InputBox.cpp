#include <vine/appfw/gui/InputBox.hpp>

#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMargins>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <limits>

#include <vine/appfw/gui/MessageBox.hpp>
#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

/**
 * @brief Creates the editor widget for a field.
 *
 * @param field  Field to create the editor for.
 * @param parent Parent widget of the created editor.
 * @return The newly created editor, or null when the kind is not supported.
 */
QWidget* createEditor(const InputBox::Field& field, QWidget* parent)
{
    switch (field.kind) {
    case InputBox::Kind::String:
    {
        const auto& f = static_cast<const InputBox::StringField&>(field);
        auto*       e = new QLineEdit(parent);
        e->setMaxLength(std::max(0, f.max_len));
        e->setText(Convert::toQString(f.def_value));
        return e;
    }
    case InputBox::Kind::Int:
    {
        const auto& f  = static_cast<const InputBox::IntField&>(field);
        auto*       e  = new QSpinBox(parent);
        const int   lo = std::min(f.min_val, f.max_val);
        const int   hi = std::max(f.min_val, f.max_val);
        e->setRange(lo, hi);
        const std::int64_t span = std::int64_t(hi) - std::int64_t(lo);
        const std::int64_t cap  = std::min<std::int64_t>(std::max<std::int64_t>(span, 1), std::numeric_limits<int>::max());
        std::int64_t       step = f.step;
        if (step == 0) {
            step = span / 10;
        }
        step = std::clamp<std::int64_t>(step, 1, cap);
        e->setSingleStep(static_cast<int>(step));
        e->setValue(std::clamp(f.def_value, lo, hi));
        return e;
    }
    case InputBox::Kind::Double:
    {
        const auto&  f  = static_cast<const InputBox::DoubleField&>(field);
        auto*        e  = new QDoubleSpinBox(parent);
        const double lo = std::min(f.min_val, f.max_val);
        const double hi = std::max(f.min_val, f.max_val);
        e->setDecimals(std::max(0, f.decimals));
        e->setRange(lo, hi);
        double step = f.step;
        if (step == 0.0) {
            step = (hi - lo) / 10.0;
        }
        if (step <= 0.0) {
            step = 1.0;
        }
        e->setSingleStep(step);
        e->setValue(std::clamp(f.def_value, lo, hi));
        return e;
    }
    case InputBox::Kind::Index:
    {
        const auto& f = static_cast<const InputBox::IndexField&>(field);
        auto*       e = new QComboBox(parent);
        for (const auto& item : f.items) {
            e->addItem(Convert::toQString(item));
        }
        if (f.def_value >= 0 && f.def_value < static_cast<int>(f.items.size())) {
            e->setCurrentIndex(f.def_value);
        }
        else {
            e->setCurrentIndex(-1);
        }
        e->setEnabled(!f.items.empty());
        return e;
    }
    }
    return nullptr;
}

/**
 * @brief Validates a field and writes the entered value back into it.
 *
 * @param field  Field to validate and read back into.
 * @param editor Editor widget created for the field.
 * @param error  Receives the localized failure description on validation error.
 * @return True when the field is valid, false otherwise.
 */
bool readField(InputBox::Field& field, QWidget* editor, QString& error)
{
    error.clear();
    const auto fail = [&](const QString& message) {
        error = field.label.empty() ? message : Convert::toQString(field.label) + QStringLiteral("：") + message;
        return false;
    };

    switch (field.kind) {
    case InputBox::Kind::String:
    {
        auto&       f    = static_cast<InputBox::StringField&>(field);
        const auto& text = static_cast<QLineEdit*>(editor)->text();
        if (text.isEmpty()) {
            if (f.required) {
                return fail(QStringLiteral("不能为空"));
            }
            f.value = String();
            return true;
        }
        if (text.size() < f.min_len || text.size() > f.max_len) {
            return fail(QStringLiteral("长度需在 %1 ~ %2 之间").arg(f.min_len).arg(f.max_len));
        }
        f.value = Convert::fromQString(text);
        return true;
    }
    case InputBox::Kind::Int:
    {
        auto& f = static_cast<InputBox::IntField&>(field);
        f.value = static_cast<QSpinBox*>(editor)->value();
        return true;
    }
    case InputBox::Kind::Double:
    {
        auto& f = static_cast<InputBox::DoubleField&>(field);
        f.value = static_cast<QDoubleSpinBox*>(editor)->value();
        return true;
    }
    case InputBox::Kind::Index:
    {
        auto&     f     = static_cast<InputBox::IndexField&>(field);
        const int index = static_cast<QComboBox*>(editor)->currentIndex();
        if (index < 0) {
            if (f.required) {
                return fail(QStringLiteral("必须选择一项"));
            }
            f.value.reset();
            return true;
        }
        f.value = index;
        return true;
    }
    }
    return true;
}

} // namespace

V_OBJECT_META_IMPL(InputBox, Window)

struct InputBox::Impl : public UIElementData {
    /// One built row of the form.
    struct Entry {
        Field*   field;  ///< Borrowed field definition.
        QWidget* editor; ///< Editor created for the field.
    };

    String              prompt;
    std::vector<Field*> fields;
    std::vector<Entry>  rows;
};

InputBox::InputBox(QWidget* parent, const String& title, const String& prompt, const std::vector<Field*>& fields)
  : Window(new Impl(), new QDialog(parent))
{
    auto* data   = dptr();
    data->prompt = prompt;
    data->fields = fields;

    auto* root = impl<QDialog>();
    root->setWindowTitle(title.empty() ? QStringLiteral("请输入") : Convert::toQString(title));
    root->setMinimumWidth(360);

    buildUi();
}

void InputBox::buildUi()
{
    auto* data = dptr();
    auto* root = impl<QDialog>();

    auto* lay = new QVBoxLayout(root);

    if (!data->prompt.empty()) {
        auto* prompt_label = new QLabel(Convert::toQString(data->prompt), root);
        prompt_label->setWordWrap(true);
        lay->addWidget(prompt_label);
    }

    auto* form = new QFormLayout();
    form->setContentsMargins(QMargins(0, 8, 0, 0));
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    lay->addLayout(form);

    data->rows.reserve(data->fields.size());
    for (auto* field : data->fields) {
        QWidget* editor = createEditor(*field, root);
        if (!editor) {
            continue;
        }
        form->addRow(Convert::toQString(field->label), editor);
        data->rows.push_back({ field, editor });
    }

    auto* ok_btn     = new QPushButton(QStringLiteral("确定"), root);
    auto* cancel_btn = new QPushButton(QStringLiteral("取消"), root);
    ok_btn->setDefault(true);
    cancel_btn->setAutoDefault(false);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch();
    buttons->addWidget(ok_btn);
    buttons->addWidget(cancel_btn);
    lay->addLayout(buttons);

    QObject::connect(ok_btn, &QPushButton::clicked, root, [this] { confirm(); });
    QObject::connect(cancel_btn, &QPushButton::clicked, root, [this] { cancel(); });
}

InputBox::~InputBox()
{
    // d is released by UIElement.
}

void InputBox::confirm()
{
    auto* data = dptr();
    for (const auto& entry : data->rows) {
        QString error;
        if (!readField(*entry.field, entry.editor, error)) {
            MessageBox::warning(this, String(), Convert::fromQString(error));
            return;
        }
    }
    impl<QDialog>()->accept();
}

void InputBox::cancel()
{
    impl<QDialog>()->reject();
}

bool InputBox::showAndWait(QWidget* parent, const String& title, const String& prompt, const std::vector<Field*>& fields)
{
    InputBox box(parent, title, prompt, fields);
    return box.exec() == QDialog::Accepted;
}

inline auto InputBox::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto InputBox::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
