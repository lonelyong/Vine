#include <vine/appfw/gui/ConfigWindow.hpp>

#include <any>
#include <limits>
#include <optional>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QValidator>

#include <vine/appfw/Application.hpp>
#include <vine/appfw/ConfigCategory.hpp>
#include <vine/appfw/ConfigGroup.hpp>
#include <vine/appfw/Translation.hpp>

#include "Convert.hpp"
#include "GuiText.hpp"
#include "WindowData.hpp"

VN_APPFWGUI_NS_BEGIN

namespace
{

// Fallback range of an editor whose item declares no range: it must never clamp
// the value it is asked to show. Wide enough for any setting, and still far from
// double extremes that QDoubleSpinBox cannot round sanely.
constexpr double kUnboundedDouble = 1.0e15;

class OptionalSpinBox : public QSpinBox {
  public:
    using QSpinBox::QSpinBox;

    QLineEdit* input() const
    {
        return lineEdit();
    }

    std::optional<int> optionalValue() const
    {
        const QString text = lineEdit()->text();
        if (text.isEmpty() || !lineEdit()->hasAcceptableInput())
            return std::nullopt;
        bool      ok    = false;
        const int value = QLocale().toInt(text, &ok);
        return ok ? std::optional<int>(value) : std::nullopt;
    }

  protected:
    QValidator::State validate(QString& text, int& position) const override
    {
        if (text.isEmpty())
            return QValidator::Acceptable;
        return QSpinBox::validate(text, position);
    }
};

class OptionalDoubleSpinBox : public QDoubleSpinBox {
  public:
    using QDoubleSpinBox::QDoubleSpinBox;

    QLineEdit* input() const
    {
        return lineEdit();
    }

    std::optional<double> optionalValue() const
    {
        const QString text = lineEdit()->text();
        if (text.isEmpty() || !lineEdit()->hasAcceptableInput())
            return std::nullopt;
        bool         ok    = false;
        const double value = QLocale().toDouble(text, &ok);
        return ok ? std::optional<double>(value) : std::nullopt;
    }

  protected:
    QValidator::State validate(QString& text, int& position) const override
    {
        if (text.isEmpty())
            return QValidator::Acceptable;
        return QDoubleSpinBox::validate(text, position);
    }
};

class OptionalNumericValueEditor : public QStackedWidget {
  public:
    OptionalNumericValueEditor(QAbstractSpinBox* value_editor, QLineEdit* input, bool required)
      : value_editor_(value_editor)
      , input_(input)
      , unset_button_(new QPushButton(required ? translate("appfw", "Required, click to set") : translate("appfw", "(Not set)"), this))
      , clear_button_(new QPushButton(QStringLiteral("×"), this))
    {
        setObjectName(QStringLiteral("optionalNumericValueEditor"));
        unset_button_->setObjectName(QStringLiteral("configValueUnset"));
        clear_button_->setObjectName(QStringLiteral("configValueClear"));
        clear_button_->setToolTip(translate("appfw", "Clear setting"));
        clear_button_->setVisible(!required);

        auto* value_page   = new QWidget(this);
        auto* value_layout = new QHBoxLayout(value_page);
        value_layout->setContentsMargins(0, 0, 0, 0);
        value_layout->addWidget(value_editor);
        value_layout->addWidget(clear_button_);
        addWidget(unset_button_);
        addWidget(value_page);
    }

    QAbstractSpinBox* valueEditor() const
    {
        return value_editor_;
    }

    QPushButton* unsetButton() const
    {
        return unset_button_;
    }

    QPushButton* clearButton() const
    {
        return clear_button_;
    }

    void beginEditing()
    {
        const QSignalBlocker blocker(input_);
        input_->clear();
        setCurrentIndex(1);
        input_->setFocus();
    }

    void setConfigured(bool configured)
    {
        setCurrentIndex(configured ? 1 : 0);
    }

  private:
    QAbstractSpinBox* value_editor_;
    QLineEdit*        input_;
    QPushButton*      unset_button_;
    QPushButton*      clear_button_;
};

/// The visible hint on a row whose new value only takes effect after a restart; empty for every other row.
QString restartMarker(const ConfigItem& item)
{
    if (!item.restartRequired())
        return QString();
    return QStringLiteral(" (") + translate("appfw", "takes effect after restart") + QLatin1Char(')');
}

/// Shows or hides the restart notice. It asks the application, not this window: the fact belongs to the run, so two
/// windows agree and a window opened later still reports a change made earlier.
void updateRestartNotice(QLabel* banner)
{
    const Application* app = Application::current();
    banner->setVisible(app != nullptr && !app->pendingRestartKeys().empty());
}

// Tab title: label > name > "General".
String categoryTitle(const ConfigCategory* cat)
{
    if (!cat->label().empty())
        return cat->label();
    if (!cat->name().empty())
        return cat->name();
    return vn::appfw::translate("appfw", "General");
}

// Group title: label > name.
String groupTitle(const ConfigGroup* grp)
{
    if (!grp->label().empty())
        return grp->label();
    return grp->name();
}

// Creates the editor widget for the item type and wires live write-back to ConfigManager.
QWidget* makeEditorWidget(ConfigManager* config, const ConfigItem& item)
{
    const String key = item.key();
    switch (item.type()) {
    case ConfigItemType::String:
    {
        auto* e = new QLineEdit();
        e->setPlaceholderText(item.isRequired() ? translate("appfw", "Required") : translate("appfw", "(Not set)"));
        QObject::connect(e, &QLineEdit::textChanged, [config, key, item](const QString& t) {
            const String value = Convert::fromQString(t);
            if (value.empty()) {
                if (!item.isRequired())
                    config->remove(key);
                return;
            }
            if (item.isValidString(value))
                config->setString(key, value);
        });
        return e;
    }
    case ConfigItemType::Bool:
    {
        auto* e = new QCheckBox();
        e->setTristate(true);
        e->setObjectName(QStringLiteral("configBoolEditor"));
        QObject::connect(e, &QCheckBox::checkStateChanged, [config, key, item, e](Qt::CheckState state) {
            if (state == Qt::PartiallyChecked) {
                if (!item.isRequired()) {
                    config->remove(key);
                    e->setText(translate("appfw", "(Not set)"));
                }
                else if (config->contains(key)) {
                    const QSignalBlocker blocker(e);
                    e->setCheckState(config->getBool(key, false) ? Qt::Checked : Qt::Unchecked);
                }
                return;
            }
            e->setText(QString());
            config->setBool(key, state == Qt::Checked);
        });
        return e;
    }
    case ConfigItemType::Int:
    {
        auto* e = new OptionalSpinBox();
        if (item.hasRange())
            e->setRange(item.minInt(), item.maxInt());
        else
            e->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        e->setSingleStep(item.step() >= 1.0 ? static_cast<int>(item.step()) : 1);
        auto* editor = new OptionalNumericValueEditor(e, e->input(), item.isRequired());
        QObject::connect(editor->unsetButton(), &QPushButton::clicked, [editor] { editor->beginEditing(); });
        QObject::connect(editor->clearButton(), &QPushButton::clicked, [config, key, editor] {
            config->remove(key);
            editor->setConfigured(false);
        });
        QObject::connect(e, QOverload<int>::of(&QSpinBox::valueChanged), [config, key, item, e](int) {
            const auto value = e->optionalValue();
            if (value && item.isValidInt(*value))
                config->setInt(key, *value);
        });
        QObject::connect(e->input(), &QLineEdit::textEdited, [config, key, item, e](const QString&) {
            const auto value = e->optionalValue();
            if (value && item.isValidInt(*value))
                config->setInt(key, *value);
        });
        return editor;
    }
    case ConfigItemType::Double:
    {
        auto* e = new OptionalDoubleSpinBox();
        if (item.hasRange())
            e->setRange(item.minDouble(), item.maxDouble());
        else
            e->setRange(-kUnboundedDouble, kUnboundedDouble);
        e->setSingleStep(item.step());
        e->setDecimals(6);
        auto* editor = new OptionalNumericValueEditor(e, e->input(), item.isRequired());
        QObject::connect(editor->unsetButton(), &QPushButton::clicked, [editor] { editor->beginEditing(); });
        QObject::connect(editor->clearButton(), &QPushButton::clicked, [config, key, editor] {
            config->remove(key);
            editor->setConfigured(false);
        });
        QObject::connect(e, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [config, key, item, e](double) {
            const auto value = e->optionalValue();
            if (value && item.isValidDouble(*value))
                config->setDouble(key, *value);
        });
        QObject::connect(e->input(), &QLineEdit::textEdited, [config, key, item, e](const QString&) {
            const auto value = e->optionalValue();
            if (value && item.isValidDouble(*value))
                config->setDouble(key, *value);
        });
        return editor;
    }
    case ConfigItemType::Choice:
    {
        auto*      e       = new QComboBox();
        const auto choices = item.choices();
        const int  offset  = item.isRequired() ? 0 : 1;
        if (offset != 0)
            e->addItem(translate("appfw", "(Not set)"));
        else
            e->setPlaceholderText(translate("appfw", "Select an option"));
        for (const auto& c : choices) e->addItem(Convert::toQString(c.description));
        QObject::connect(e, QOverload<int>::of(&QComboBox::currentIndexChanged), [config, key, choices, offset](int idx) {
            if (offset != 0 && idx == 0) {
                config->remove(key);
                return;
            }
            idx -= offset;
            if (idx < 0 || static_cast<size_t>(idx) >= choices.size())
                return;
            const auto& v = choices[static_cast<size_t>(idx)].value;
            if (const auto* as_int = std::any_cast<int>(&v))
                config->setInt(key, *as_int);
            else if (const auto* as_double = std::any_cast<double>(&v))
                config->setDouble(key, *as_double);
            else {
                const String* as_string = std::any_cast<String>(&v);
                config->setString(key, as_string != nullptr ? *as_string : String());
            }
        });
        return e;
    }
    }
    return nullptr;
}

// Index of the matching stored choice, or -1 when the panel must not claim a selection.
int choiceIndex(const ConfigManager& config, const ConfigItem& item)
{
    const String& key    = item.key();
    const bool    stored = config.contains(key);
    if (!stored && !item.isRequired())
        return 0;
    const auto& cs = item.choices();

    for (size_t i = 0; i < cs.size(); ++i) {
        const auto& v = cs[i].value;
        if (const auto* as_int = std::any_cast<int>(&v)) {
            if (stored && config.getInt(key, *as_int) == *as_int)
                return static_cast<int>(i) + (item.isRequired() ? 0 : 1);
        }
        else if (const auto* as_double = std::any_cast<double>(&v)) {
            if (stored && config.getDouble(key, *as_double) == *as_double)
                return static_cast<int>(i) + (item.isRequired() ? 0 : 1);
        }
        else if (const auto* as_string = std::any_cast<String>(&v)) {
            if (stored && config.getString(key, *as_string) == *as_string)
                return static_cast<int>(i) + (item.isRequired() ? 0 : 1);
        }
    }
    return -1;
}

// Writes the value stored for item into its editor without emitting signals.
// Every item is addressed by its own key, never by the position of the editor in
// the window: a plugin may register items after the window was built, which
// changes the traversal order of the registry.
void writeEditorValue(QWidget* widget, const ConfigManager& config, const ConfigItem& item)
{
    const String key = item.key();
    switch (item.type()) {
    case ConfigItemType::String:
    {
        auto*                line_edit = static_cast<QLineEdit*>(widget);
        const QSignalBlocker blocker(line_edit);
        const String         value = config.contains(key) ? config.getString(key, String()) : String();
        line_edit->setText(Convert::toQString(value));
        break;
    }
    case ConfigItemType::Bool:
    {
        auto*                checkbox = static_cast<QCheckBox*>(widget);
        const QSignalBlocker blocker(checkbox);
        if (config.contains(key)) {
            checkbox->setCheckState(config.getBool(key, false) ? Qt::Checked : Qt::Unchecked);
            checkbox->setText(QString());
        }
        else {
            checkbox->setCheckState(Qt::PartiallyChecked);
            checkbox->setText(translate("appfw", item.isRequired() ? "Required" : "(Not set)"));
        }
        break;
    }
    case ConfigItemType::Int:
    {
        auto*                editor = static_cast<OptionalNumericValueEditor*>(widget);
        auto*                spin   = static_cast<OptionalSpinBox*>(editor->valueEditor());
        const QSignalBlocker blocker(spin);
        const bool           configured = config.contains(key);
        if (configured)
            spin->setValue(config.getInt(key, spin->value()));
        else
            spin->input()->clear();
        editor->setConfigured(configured);
        break;
    }
    case ConfigItemType::Double:
    {
        auto*                editor = static_cast<OptionalNumericValueEditor*>(widget);
        auto*                spin   = static_cast<OptionalDoubleSpinBox*>(editor->valueEditor());
        const QSignalBlocker blocker(spin);
        const bool           configured = config.contains(key);
        if (configured)
            spin->setValue(config.getDouble(key, spin->value()));
        else
            spin->input()->clear();
        editor->setConfigured(configured);
        break;
    }
    case ConfigItemType::Choice:
    {
        auto*                combo = static_cast<QComboBox*>(widget);
        const QSignalBlocker blocker(combo);
        combo->setCurrentIndex(choiceIndex(config, item));
        break;
    }
    }
}

} // namespace

VN_OBJECT_META_IMPL(ConfigWindow, Window)

struct ConfigWindow::Impl : public WindowData {
    /// One editable item: the editor widget and the ConfigManager key it shows.
    struct Editor {
        QWidget* widget = nullptr;
        String   key;
    };

    ConfigRegistry*     registry = nullptr;
    ConfigManager*      config   = nullptr;
    QLabel*             banner   = nullptr;
    vn::Connection      config_changed{};
    std::vector<Editor> editors; // Window traversal order, always addressed by key
};

ConfigWindow::ConfigWindow(ConfigRegistry* registry, ConfigManager* config)
  : Window(new Impl(), new QDialog())
{
    auto* data     = dptr();
    data->registry = registry;
    data->config   = config;

    auto* root = impl<QDialog>();
    auto* tabs = new QTabWidget(root);
    tabs->setTabBarAutoHide(true); // Hides the tab bar when there is a single tab

    for (ConfigCategory* cat : registry->categories()) {
        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        auto* container = new QWidget();
        auto* vlay      = new QVBoxLayout(container);

        bool has_item = false;
        for (ConfigGroup* grp : cat->groups()) {
            const auto items = grp->items();
            if (items.empty())
                continue; // Skip empty groups
            auto* box = new QGroupBox(Convert::toQString(groupTitle(grp)));
            if (!grp->description().empty())
                box->setToolTip(Convert::toQString(grp->description()));
            auto* form = new QFormLayout(box);
            vlay->addWidget(box);
            for (const ConfigItem* item : items) {
                auto* editor = makeEditorWidget(config, *item);
                data->editors.push_back(Impl::Editor{ editor, item->key() });
                if (item->readOnly())
                    editor->setEnabled(false);
                auto* label = new QLabel(Convert::toQString(item->label()) + restartMarker(*item));
                if (!item->description().empty())
                    label->setToolTip(Convert::toQString(item->description()));
                form->addRow(label, editor);
                has_item = true;
            }
        }
        if (!has_item) {
            auto* tip = new QLabel(translate("appfw", "(empty)"));
            vlay->addWidget(tip);
        }
        scroll->setWidget(container);
        tabs->addTab(scroll, Convert::toQString(categoryTitle(cat)));
        if (!cat->description().empty())
            tabs->setTabToolTip(tabs->count() - 1, Convert::toQString(cat->description()));
    }

    if (registry->categories().empty()) {
        auto* tip = new QLabel(translate("appfw", "(no config item registered)"));
        tabs->addTab(tip, translate("appfw", "General"));
    }

    auto* banner = new QLabel(translate("appfw", "Some changes take effect only after a restart"));
    banner->setWordWrap(true);
    banner->setVisible(false);
    data->banner = banner;

    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(banner);
    lay->addWidget(tabs);

    data->config_changed = config->changed.connect([banner](ConfigManager&, ConfigChangedEventArgs&) {
        QMetaObject::invokeMethod(banner, [banner] { updateRestartNotice(banner); }, Qt::AutoConnection);
    });
    updateRestartNotice(banner);

    refresh();
}

ConfigWindow::~ConfigWindow()
{
    // The handler runs from ConfigManager::changed, so it must go before the banner (a child widget) is destroyed.
    dptr()->config_changed.disconnect();
}

void ConfigWindow::refresh()
{
    auto* data = dptr();
    for (const auto& editor : data->editors) {
        const ConfigItem* item = data->registry->item(editor.key);
        if (item == nullptr)
            continue; // the registry changed after the window was built
        writeEditorValue(editor.widget, *data->config, *item);
    }
}

void ConfigWindow::reset()
{
    auto* data = dptr();
    for (const auto& editor : data->editors) {
        const ConfigItem* item = data->registry->item(editor.key);
        if (item == nullptr)
            continue; // the registry changed after the window was built
        if (!item->hasDefault()) {
            if (!item->isRequired())
                data->config->remove(item->key());
            continue;
        }
        switch (item->type()) {
        case ConfigItemType::String:
            if (item->defaultString().empty() && !item->isRequired())
                data->config->remove(item->key());
            else
                data->config->setString(item->key(), item->defaultString());
            break;
        case ConfigItemType::Bool: data->config->setBool(item->key(), item->defaultBool()); break;
        case ConfigItemType::Int: data->config->setInt(item->key(), item->defaultInt()); break;
        case ConfigItemType::Double: data->config->setDouble(item->key(), item->defaultDouble()); break;
        case ConfigItemType::Choice:
            switch (item->defaultType()) {
            case ConfigItemType::Int: data->config->setInt(item->key(), item->defaultInt()); break;
            case ConfigItemType::Double: data->config->setDouble(item->key(), item->defaultDouble()); break;
            default: data->config->setString(item->key(), item->defaultString()); break;
            }
            break;
        }
    }
    refresh();
}

raw_ptr<ConfigRegistry> ConfigWindow::registry() const
{
    return dptr()->registry;
}

raw_ptr<ConfigManager> ConfigWindow::config() const
{
    return dptr()->config;
}

inline auto ConfigWindow::dptr() -> Impl*
{
    return static_cast<Impl*>(Window::d);
}

inline auto ConfigWindow::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(Window::d);
}

VN_APPFWGUI_NS_END
