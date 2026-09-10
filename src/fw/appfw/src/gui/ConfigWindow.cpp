#include <vine/appfw/gui/ConfigWindow.hpp>

#include <any>
#include <limits>
#include <vector>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <vine/appfw/ConfigCategory.hpp>
#include <vine/appfw/ConfigGroup.hpp>

#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

// Fallback range of an editor whose item declares no range: it must never clamp
// the value it is asked to show. Wide enough for any setting, and still far from
// double extremes that QDoubleSpinBox cannot round sanely.
constexpr double s_unbounded_double = 1.0e15;

// Tab title: label > name > "General".
String categoryTitle(const ConfigCategory* cat)
{
    if (!cat->label().empty())
        return cat->label();
    if (!cat->name().empty())
        return cat->name();
    return String(u8"通用");
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
        QObject::connect(e, &QLineEdit::textChanged, [config, key](const QString& t) { config->setString(key, Convert::fromQString(t)); });
        return e;
    }
    case ConfigItemType::Bool:
    {
        auto* e = new QCheckBox();
        QObject::connect(e, &QCheckBox::toggled, [config, key](bool on) { config->setBool(key, on); });
        return e;
    }
    case ConfigItemType::Int:
    {
        auto* e = new QSpinBox();
        if (item.hasRange())
            e->setRange(item.minInt(), item.maxInt());
        else
            e->setRange(std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
        e->setSingleStep(item.step() >= 1.0 ? static_cast<int>(item.step()) : 1);
        QObject::connect(e, QOverload<int>::of(&QSpinBox::valueChanged), [config, key](int v) { config->setInt(key, v); });
        return e;
    }
    case ConfigItemType::Double:
    {
        auto* e = new QDoubleSpinBox();
        if (item.hasRange())
            e->setRange(item.minDouble(), item.maxDouble());
        else
            e->setRange(-s_unbounded_double, s_unbounded_double);
        e->setSingleStep(item.step());
        e->setDecimals(6);
        QObject::connect(e, QOverload<double>::of(&QDoubleSpinBox::valueChanged), [config, key](double v) { config->setDouble(key, v); });
        return e;
    }
    case ConfigItemType::Choice:
    {
        auto*      e       = new QComboBox();
        const auto choices = item.choices();
        for (const auto& c : choices) e->addItem(Convert::toQString(c.description));
        QObject::connect(e, QOverload<int>::of(&QComboBox::currentIndexChanged), [config, key, choices](int idx) {
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

// Index of the choice a Choice item should display: the one equal to the stored
// value, or to the item default when the key was never written (that is what
// reset() would store).
//
// Returns -1 when the value is not among the choices, so the panel never claims
// a selection the configuration does not hold.
int choiceIndex(const ConfigManager& config, const ConfigItem& item)
{
    const String& key          = item.key();
    const bool    stored       = config.contains(key);
    const bool    has_default  = item.hasDefault();
    const auto    default_type = item.defaultType();
    const auto&   cs           = item.choices();

    for (size_t i = 0; i < cs.size(); ++i) {
        const auto& v = cs[i].value;
        if (const auto* as_int = std::any_cast<int>(&v)) {
            if (stored ? config.getInt(key, *as_int) == *as_int : has_default && default_type == ConfigItemType::Int && item.defaultInt() == *as_int)
                return static_cast<int>(i);
        }
        else if (const auto* as_double = std::any_cast<double>(&v)) {
            if (stored ? config.getDouble(key, *as_double) == *as_double : has_default && default_type == ConfigItemType::Double && item.defaultDouble() == *as_double)
                return static_cast<int>(i);
        }
        else if (const auto* as_string = std::any_cast<String>(&v)) {
            if (stored ? config.getString(key, *as_string) == *as_string : has_default && default_type == ConfigItemType::String && item.defaultString() == *as_string)
                return static_cast<int>(i);
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
    widget->blockSignals(true);
    switch (item.type()) {
    case ConfigItemType::String:
        static_cast<QLineEdit*>(widget)->setText(
            Convert::toQString(config.getString(key, item.hasDefault() ? item.defaultString() : String())));
        break;
    case ConfigItemType::Bool: static_cast<QCheckBox*>(widget)->setChecked(config.getBool(key, item.hasDefault() && item.defaultBool())); break;
    case ConfigItemType::Int: static_cast<QSpinBox*>(widget)->setValue(config.getInt(key, item.hasDefault() ? item.defaultInt() : 0)); break;
    case ConfigItemType::Double:
        static_cast<QDoubleSpinBox*>(widget)->setValue(config.getDouble(key, item.hasDefault() ? item.defaultDouble() : 0.0));
        break;
    case ConfigItemType::Choice: static_cast<QComboBox*>(widget)->setCurrentIndex(choiceIndex(config, item)); break;
    }
    widget->blockSignals(false);
}

} // namespace

V_OBJECT_META_IMPL(ConfigWindow, Window)

struct ConfigWindow::Impl : public UIElementData {
    /// One editable item: the editor widget and the ConfigManager key it shows.
    struct Editor {
        QWidget* widget = nullptr;
        String   key;
    };

    ConfigRegistry*     registry = nullptr;
    ConfigManager*      config   = nullptr;
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
                auto* label = new QLabel(Convert::toQString(item->label()));
                if (!item->description().empty())
                    label->setToolTip(Convert::toQString(item->description()));
                form->addRow(label, editor);
                has_item = true;
            }
        }
        if (!has_item) {
            auto* tip = new QLabel(QStringLiteral("(空)"));
            vlay->addWidget(tip);
        }
        scroll->setWidget(container);
        tabs->addTab(scroll, Convert::toQString(categoryTitle(cat)));
        if (!cat->description().empty())
            tabs->setTabToolTip(tabs->count() - 1, Convert::toQString(cat->description()));
    }

    if (registry->categories().empty()) {
        auto* tip = new QLabel(QStringLiteral("(未注册配置项)"));
        tabs->addTab(tip, QStringLiteral("通用"));
    }
    auto* lay = new QVBoxLayout(root);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(tabs);

    refresh();
}

ConfigWindow::~ConfigWindow()
{
    // d is released by UIElement
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
            data->config->remove(item->key());
            continue;
        }
        switch (item->type()) {
        case ConfigItemType::String: data->config->setString(item->key(), item->defaultString()); break;
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

V_APPFWGUI_NS_END
