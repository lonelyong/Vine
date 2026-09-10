#include <vine/appfw/gui/PluginManagerDialog.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDialog>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStackedWidget>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"
#include "TableStyle.hpp"

V_APPFWGUI_NS_BEGIN

namespace
{

QString joinStrings(const std::vector<String>& list)
{
    QString result;
    bool    first = true;
    for (const auto& s : list) {
        if (!first) {
            result += QStringLiteral(", ");
        }
        result += Convert::toQString(s);
        first = false;
    }
    return result.isEmpty() ? QStringLiteral("—") : result;
}

/// Returns the discovery entry of a plugin, or nullptr when unknown.
const vine::appfw::PluginEntry* findEntry(const std::vector<vine::appfw::PluginEntry>& entries, const String& name)
{
    for (const auto& entry : entries) {
        if (entry.info.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// Converts a vine::String (UTF-8) to a filesystem path.
std::filesystem::path toPath(const String& text)
{
    return std::filesystem::path(std::u8string(text));
}

/// Returns the registration a plugin library was discovered through, or nullptr.
///
/// A registration points either at the library file itself or at a directory
/// that holds it (see PluginManager::installPlugin).
const vine::appfw::PluginRegistration* findRegistration(const std::vector<vine::appfw::PluginRegistration>& registrations,
                                                        const std::filesystem::path&                       library)
{
    for (const auto& registration : registrations) {
        const std::filesystem::path path = toPath(registration.path);
        if (library == path || library.parent_path() == path) {
            return &registration;
        }
    }
    return nullptr;
}

/// Returns the scope of a plugin as shown in the detail page.
QString scopeLabel(vine::appfw::PluginScope scope)
{
    switch (scope) {
    case vine::appfw::PluginScope::BuiltIn:  return QStringLiteral("程序自带");
    case vine::appfw::PluginScope::AllUsers: return QStringLiteral("所有用户");
    case vine::appfw::PluginScope::User:     return QStringLiteral("仅当前用户");
    }
    return QStringLiteral("未知");
}

/// Renders a list row as inactive, for a plugin that will not run.
void greyOut(QListWidgetItem* item)
{
    QFont font = item->font();
    font.setItalic(true);
    item->setFont(font);
    item->setForeground(QBrush(QColor(140, 140, 140)));
}

/// Fallback plugin icon, used when a plugin declares none (PluginInfo::icon).
///
/// A module glyph in plain shapes, because the host renders icons with
/// QSvgRenderer, which supports a static (Tiny) SVG subset: no scripts, no
/// external references, no filters.
constexpr const char* s_default_plugin_icon_svg =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><g fill="none" stroke="#5b8def" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><rect x="6.5" y="6.5" width="11" height="11" rx="2.5"/><path d="M10 3v3.5M14 3v3.5M10 17.5V21M14 17.5V21M3 10h3.5M3 14h3.5M17.5 10H21M17.5 14H21"/></g></svg>)SVG";

/// Renders an inline SVG at the given logical size.
///
/// Returns a null pixmap when the source is not a usable SVG, so the caller can
/// fall back to the default icon; that keeps a broken icon of a third-party
/// plugin from showing up as an empty row in the list.
QPixmap renderSvgIcon(const QString& svg, int size)
{
    QSvgRenderer renderer(svg.toUtf8());
    if (!renderer.isValid() || size <= 0) {
        return {};
    }

    // Render at the device pixel ratio so the icon stays crisp on a HiDPI screen.
    const qreal ratio = qGuiApp != nullptr ? qGuiApp->devicePixelRatio() : 1.0;
    QPixmap     pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    renderer.render(&painter);
    return pixmap;
}

/**
 * @brief Returns the short state label shown next to a plugin's name.
 *
 * @param entry Discovery entry, or nullptr when the plugin is unknown.
 * @return The state, e.g. "已加载" or "由程序跳过".
 */
QString stateLabel(const vine::appfw::PluginEntry* entry)
{
    if (entry == nullptr) {
        return QStringLiteral("元数据缺失");
    }
    if (entry->skipped) {
        return QStringLiteral("由程序跳过");
    }
    if (!entry->enabled) {
        return entry->loaded ? QStringLiteral("已禁用（运行中）") : QStringLiteral("已禁用");
    }
    return entry->loaded ? QStringLiteral("已加载") : QStringLiteral("未加载");
}

/**
 * @brief Returns the reason why a plugin is (not) running.
 *
 * The badge is the glance, this sentence is the explanation shown on the detail
 * page and in the row tooltip.
 *
 * @param entry Discovery entry, or nullptr when the plugin is unknown.
 * @return The explanation.
 */
QString stateExplanation(const vine::appfw::PluginEntry* entry)
{
    if (entry == nullptr) {
        return QStringLiteral("未发现该插件的元数据：库已不在注册位置，或注册文件指向的文件不可读。");
    }
    if (entry->skipped) {
        return QStringLiteral("由程序跳过：宿主程序把该插件列入了跳过列表，本次未加载，"
                              "只能由程序解除（启动参数或程序逻辑）。");
    }
    if (!entry->enabled && entry->loaded) {
        return QStringLiteral("已禁用：本次运行中仍在运行（命令与配置仍然有效），重启后不再加载。");
    }
    if (!entry->enabled) {
        return QStringLiteral("已禁用：本次未加载，因此没有命令与配置信息；重启后仍然不加载。"
                              "在右侧按钮或右键菜单中启用。");
    }
    if (entry->loaded) {
        return QStringLiteral("已加载：命令与配置已生效；程序退出时按依赖反序卸载。");
    }
    return QStringLiteral("已启用但未加载：依赖未满足或加载失败，详见日志。");
}

/// Returns the stylesheet of the state badge for a plugin.
QString badgeStyle(const vine::appfw::PluginEntry* entry)
{
    // Explicit colors on purpose: the badge has to read as a state in both the
    // light and the dark theme, and palette roles do not survive a stylesheet.
    const char* background = "#eceff1";
    const char* foreground = "#37474f";
    if (entry != nullptr) {
        if (entry->skipped) {
            background = "#ede7f6";
            foreground = "#4527a0";
        }
        else if (!entry->enabled) {
            background = "#fff3e0";
            foreground = "#b25000";
        }
        else if (entry->loaded) {
            background = "#e8f5e9";
            foreground = "#1b5e20";
        }
    }
    return QStringLiteral("background:%1; color:%2; border-radius:9px; padding:3px 10px; font-weight:600;")
        .arg(QString::fromLatin1(background), QString::fromLatin1(foreground));
}

} // namespace

V_OBJECT_META_IMPL(PluginManagerDialog, Window)

struct PluginManagerDialog::Impl : public UIElementData {
    vine::appfw::PluginManager* manager        = nullptr;
    QLineEdit*                  filter         = nullptr;
    QListWidget*                list           = nullptr;
    QPushButton*                load_btn       = nullptr;
    QToolButton*                install_btn    = nullptr;
    QStackedWidget*             stack          = nullptr;
    QLabel*                     icon_label     = nullptr;
    QLabel*                     title_label    = nullptr;
    QLabel*                     subtitle_label = nullptr;
    QLabel*                     meta_label     = nullptr;
    QLabel*                     status_badge   = nullptr;
    QLabel*                     status_label   = nullptr;
    QLabel*                     desc_label     = nullptr;
    QLabel*                     id_label       = nullptr;
    QLabel*                     version_label  = nullptr;
    QLabel*                     scope_label    = nullptr;
    QLabel*                     dep_label      = nullptr;
    QLabel*                     uuid_label     = nullptr;
    QLabel*                     path_label     = nullptr;
    QLabel*                     built_label    = nullptr;
    QLabel*                     email_label    = nullptr;
    QLabel*                     repo_label     = nullptr;
    QLabel*                     message_label  = nullptr;
    QTableWidget*               cmd_table      = nullptr;
    QTableWidget*               cfg_table      = nullptr;
    QPushButton*                toggle_btn     = nullptr;
    QPushButton*                uninstall_btn  = nullptr;

    /// Repository URL of the selected plugin; the info tab links to it.
    QString current_repo;

    /// Rendered icons, keyed by "<size>|<svg source>": the list is rebuilt on every
    /// refresh, and re-rendering the same SVG each time would be wasteful.
    QHash<QString, QPixmap> icons;

    /**
     * @brief Returns the icon of a plugin, falling back to the default icon.
     *
     * @param svg_source PluginInfo::icon of the plugin; empty means "use the default".
     * @param size       Logical icon size in pixels.
     * @return The icon; never the null icon, unless the default itself is unusable.
     */
    QPixmap iconFor(const vine::String& svg_source, int size)
    {
        const QString declared = Convert::toQString(svg_source);
        const QString key      = QStringLiteral("%1|%2").arg(size).arg(declared);
        if (const auto known = icons.find(key); known != icons.end()) {
            return known.value();
        }

        QPixmap pixmap = declared.isEmpty() ? QPixmap() : renderSvgIcon(declared, size);
        if (pixmap.isNull()) {
            pixmap = renderSvgIcon(QString::fromUtf8(s_default_plugin_icon_svg), size);
        }
        icons.insert(key, pixmap);
        return pixmap;
    }
};

PluginManagerDialog::PluginManagerDialog(vine::appfw::PluginManager* manager)
  : Window(new Impl(), new QDialog())
{
    auto* data    = dptr();
    data->manager = manager;

    auto* root = impl<QDialog>();
    root->setWindowTitle(QStringLiteral("插件管理器"));

    auto* outer = new QVBoxLayout(root);
    outer->setContentsMargins(10, 10, 10, 8);
    outer->setSpacing(8);

    auto* splitter = new QSplitter(Qt::Horizontal, root);
    splitter->setChildrenCollapsible(false);
    outer->addWidget(splitter, 1);

    // ---- Left: filter + plugin list + the two "bring a plugin in" actions ----
    auto* left     = new QWidget(splitter);
    auto* left_lay = new QVBoxLayout(left);
    left_lay->setContentsMargins(0, 0, 0, 0);
    left_lay->setSpacing(6);

    data->filter = new QLineEdit(left);
    data->filter->setPlaceholderText(QStringLiteral("筛选（名称 / 显示名 / 厂商 / 描述）"));
    data->filter->setClearButtonEnabled(true);
    left_lay->addWidget(data->filter);

    data->list = new QListWidget(left);
    data->list->setSelectionMode(QAbstractItemView::SingleSelection);
    data->list->setContextMenuPolicy(Qt::CustomContextMenu);
    data->list->setIconSize(QSize(20, 20));
    data->list->setUniformItemSizes(true);
    left_lay->addWidget(data->list, 1);

    data->load_btn = new QPushButton(QStringLiteral("加载插件…"), left);
    data->load_btn->setToolTip(QStringLiteral("立即加载并运行，不写注册表（重启后不保留）。"));

    data->install_btn = new QToolButton(left);
    data->install_btn->setText(QStringLiteral("安装插件"));
    data->install_btn->setPopupMode(QToolButton::InstantPopup);
    data->install_btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    data->install_btn->setToolTip(QStringLiteral("注册一个插件位置（库文件或目录，可在程序目录之外）；重启后加载。"));
    auto*    install_menu      = new QMenu(data->install_btn);
    QAction* install_for_user  = install_menu->addAction(QStringLiteral("仅当前用户…"));
    QAction* install_for_users = install_menu->addAction(QStringLiteral("所有用户…"));
    data->install_btn->setMenu(install_menu);

    auto* list_actions = new QHBoxLayout();
    list_actions->setSpacing(6);
    list_actions->addWidget(data->load_btn);
    list_actions->addWidget(data->install_btn);
    list_actions->addStretch();
    left_lay->addLayout(list_actions);

    splitter->addWidget(left);

    // ---- Right: placeholder + detail page ----
    data->stack = new QStackedWidget(splitter);

    // Placeholder: what the dialog is for, so an empty right pane is not a blank.
    auto* placeholder     = new QWidget(data->stack);
    auto* placeholder_lay = new QVBoxLayout(placeholder);
    placeholder_lay->addStretch();

    auto* placeholder_icon = new QLabel(placeholder);
    placeholder_icon->setPixmap(renderSvgIcon(QString::fromUtf8(s_default_plugin_icon_svg), 64));
    placeholder_icon->setAlignment(Qt::AlignCenter);

    auto* placeholder_text = new QLabel(QStringLiteral("选择一个插件查看详情"), placeholder);
    placeholder_text->setAlignment(Qt::AlignCenter);
    QFont placeholder_font = placeholder_text->font();
    placeholder_font.setPointSizeF(placeholder_font.pointSizeF() + 1.0);
    placeholder_text->setFont(placeholder_font);

    auto* placeholder_hint = new QLabel(QStringLiteral("详情页显示来源、依赖、命令与配置，并给出可执行的操作。"), placeholder);
    placeholder_hint->setAlignment(Qt::AlignCenter);
    placeholder_hint->setStyleSheet(QStringLiteral("color: gray;"));

    placeholder_lay->addWidget(placeholder_icon);
    placeholder_lay->addSpacing(6);
    placeholder_lay->addWidget(placeholder_text);
    placeholder_lay->addWidget(placeholder_hint);
    placeholder_lay->addStretch();
    data->stack->addWidget(placeholder);

    // The detail page scrolls: the header and the tab pages must not be squashed
    // by a small window (and the tables keep their own scrollbars).
    auto* detail_scroll = new QScrollArea(data->stack);
    detail_scroll->setWidgetResizable(true);
    detail_scroll->setFrameShape(QFrame::NoFrame);

    auto* detail     = new QWidget(detail_scroll);
    auto* detail_lay = new QVBoxLayout(detail);
    detail_lay->setContentsMargins(12, 4, 8, 4);
    detail_lay->setSpacing(10);
    detail_scroll->setWidget(detail);
    data->stack->addWidget(detail_scroll);

    // Header: icon, display name, identity line, vendor contact and state badge.
    auto* header     = new QWidget(detail);
    auto* header_lay = new QHBoxLayout(header);
    header_lay->setContentsMargins(0, 0, 0, 0);
    header_lay->setSpacing(12);

    data->icon_label = new QLabel(header);
    data->icon_label->setFixedSize(56, 56);
    data->icon_label->setAlignment(Qt::AlignCenter);
    header_lay->addWidget(data->icon_label, 0, Qt::AlignTop);

    auto* title_col = new QVBoxLayout();
    title_col->setSpacing(2);

    data->title_label = new QLabel(header);
    QFont title_font  = data->title_label->font();
    title_font.setPointSizeF(title_font.pointSizeF() + 3.0);
    title_font.setBold(true);
    data->title_label->setFont(title_font);
    data->title_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    data->title_label->setWordWrap(true);

    data->subtitle_label = new QLabel(header);
    data->subtitle_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    data->subtitle_label->setStyleSheet(QStringLiteral("color: gray;"));

    data->meta_label = new QLabel(header);
    data->meta_label->setTextFormat(Qt::RichText);
    data->meta_label->setOpenExternalLinks(true);
    data->meta_label->setWordWrap(true);

    title_col->addWidget(data->title_label);
    title_col->addWidget(data->subtitle_label);
    title_col->addWidget(data->meta_label);
    title_col->addStretch();
    header_lay->addLayout(title_col, 1);

    data->status_badge = new QLabel(header);
    data->status_badge->setAlignment(Qt::AlignCenter);
    header_lay->addWidget(data->status_badge, 0, Qt::AlignTop);
    detail_lay->addWidget(header);

    // The badge is the glance; this line is the reason (and what to do about it).
    data->status_label = new QLabel(detail);
    data->status_label->setWordWrap(true);
    data->status_label->setFrameShape(QFrame::StyledPanel);
    data->status_label->setMargin(6);
    data->status_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detail_lay->addWidget(data->status_label);

    // Actions: showDetail() shows only the ones that can take effect.
    data->toggle_btn    = new QPushButton(QStringLiteral("禁用插件"), detail);
    data->uninstall_btn = new QPushButton(QStringLiteral("卸载插件"), detail);
    data->toggle_btn->setToolTip(QStringLiteral("只写偏好，不动本次运行；下次启动生效。"));
    data->uninstall_btn->setToolTip(QStringLiteral("移除注册文件，重启后不再加载。"));

    auto* action_row = new QHBoxLayout();
    action_row->setSpacing(6);
    action_row->addWidget(data->toggle_btn);
    action_row->addWidget(data->uninstall_btn);
    action_row->addStretch();
    detail_lay->addLayout(action_row);
    data->toggle_btn->setVisible(false);
    data->uninstall_btn->setVisible(false);

    // The details are tabs: one facet at a time stays readable, and the tables
    // get the full height instead of a quarter of it. The pane background is set
    // here, rather than left to the style, so that it and the table headers are
    // the same colour by construction (the style shades the native pane
    // differently from any palette role).
    auto* tabs = new QTabWidget(detail);
    tabs->setStyleSheet(QStringLiteral("QTabWidget::pane { background: palette(window);"
                                       " border: 1px solid palette(mid); }"));

    // Info tab: the description, then identity, origin, needs, location, author.
    auto* info_page = new QWidget(tabs);
    auto* info_lay  = new QVBoxLayout(info_page);
    info_lay->setContentsMargins(10, 10, 10, 10);

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    const auto make_value = [info_page] {
        auto* label = new QLabel(info_page);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        label->setWordWrap(true);
        return label;
    };

    // The description leads the tab: it answers "what is this plugin" before the
    // identifiers do, which is why it is no longer a group of its own.
    data->desc_label    = make_value();
    data->id_label      = make_value();
    data->version_label = make_value();
    data->scope_label   = make_value();
    data->dep_label     = make_value();
    data->uuid_label    = make_value();
    data->path_label    = make_value();
    data->built_label   = make_value();
    data->email_label   = make_value();
    data->repo_label    = make_value();

    // Identifiers and paths read better in a monospaced font.
    QFont mono = data->uuid_label->font();
    mono.setFamilies({ QStringLiteral("monospace"), QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono") });
    data->uuid_label->setFont(mono);
    data->path_label->setFont(mono);

    form->addRow(QStringLiteral("描述"), data->desc_label);
    form->addRow(QStringLiteral("标识"), data->id_label);
    form->addRow(QStringLiteral("版本"), data->version_label);
    form->addRow(QStringLiteral("来源"), data->scope_label);
    form->addRow(QStringLiteral("依赖"), data->dep_label);
    form->addRow(QStringLiteral("UUID"), data->uuid_label);
    form->addRow(QStringLiteral("库路径"), data->path_label);
    form->addRow(QStringLiteral("构建框架"), data->built_label);
    form->addRow(QStringLiteral("邮箱"), data->email_label);
    form->addRow(QStringLiteral("仓库"), data->repo_label);

    // The row carries the URL, so a click opens the browser just like the former
    // "open repository" button did.
    data->repo_label->setOpenExternalLinks(true);
    info_lay->addLayout(form);
    info_lay->addStretch();
    tabs->addTab(info_page, QStringLiteral("信息"));

    // Commands tab: what the plugin adds to the command registry.
    auto* cmd_page = new QWidget(tabs);
    auto* cmd_lay  = new QVBoxLayout(cmd_page);
    cmd_lay->setContentsMargins(6, 6, 6, 6);
    data->cmd_table = new QTableWidget(0, 3, cmd_page);
    data->cmd_table->setHorizontalHeaderLabels({ QStringLiteral("命令"), QStringLiteral("分组"), QStringLiteral("描述") });
    data->cmd_table->horizontalHeader()->setStretchLastSection(true);
    data->cmd_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->cmd_table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->cmd_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->cmd_table->setMinimumHeight(120);
    detail::blendIntoSurface(data->cmd_table);
    cmd_lay->addWidget(data->cmd_table, 1);
    tabs->addTab(cmd_page, QStringLiteral("命令"));

    // Config tab: the items the plugin registered with the host.
    auto* cfg_page = new QWidget(tabs);
    auto* cfg_lay  = new QVBoxLayout(cfg_page);
    cfg_lay->setContentsMargins(6, 6, 6, 6);
    data->cfg_table = new QTableWidget(0, 3, cfg_page);
    data->cfg_table->setHorizontalHeaderLabels({ QStringLiteral("键"), QStringLiteral("标签"), QStringLiteral("描述") });
    data->cfg_table->horizontalHeader()->setStretchLastSection(true);
    data->cfg_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->cfg_table->setSelectionMode(QAbstractItemView::SingleSelection);
    data->cfg_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->cfg_table->setMinimumHeight(120);
    detail::blendIntoSurface(data->cfg_table);
    cfg_lay->addWidget(data->cfg_table, 1);
    tabs->addTab(cfg_page, QStringLiteral("配置"));

    detail_lay->addWidget(tabs, 1);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 300, 620 });

    // ---- Bottom: feedback on the left, dialog commands on the right ----
    auto* bottom_lay = new QHBoxLayout();
    bottom_lay->setSpacing(8);

    data->message_label = new QLabel(root);
    data->message_label->setWordWrap(true);
    data->message_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottom_lay->addWidget(data->message_label, 1);

    auto* refresh_btn = new QPushButton(QStringLiteral("刷新"), root);
    auto* close_btn   = new QPushButton(QStringLiteral("关闭"), root);
    refresh_btn->setToolTip(QStringLiteral("重新扫描自带插件目录与已注册的位置。"));
    bottom_lay->addWidget(refresh_btn);
    bottom_lay->addWidget(close_btn);
    outer->addLayout(bottom_lay);

    QObject::connect(data->filter, &QLineEdit::textChanged, root, [this] { refresh(); });
    QObject::connect(data->load_btn, &QPushButton::clicked, root, [this] { loadPlugin(); });
    QObject::connect(install_for_user, &QAction::triggered, root, [this] { installPlugin(vine::appfw::PluginScope::User); });
    QObject::connect(install_for_users, &QAction::triggered, root, [this] { installPlugin(vine::appfw::PluginScope::AllUsers); });
    QObject::connect(data->toggle_btn, &QPushButton::clicked, root, [this] { togglePluginEnabled(); });
    QObject::connect(data->uninstall_btn, &QPushButton::clicked, root, [this] { uninstallSelectedPlugin(); });
    QObject::connect(refresh_btn, &QPushButton::clicked, root, [this] { refresh(); });
    QObject::connect(close_btn, &QPushButton::clicked, root, [root] { root->close(); });

    QObject::connect(data->list, &QListWidget::currentItemChanged, root, [this](QListWidgetItem* item, QListWidgetItem*) {
        showDetail(item ? Convert::fromQString(item->data(Qt::UserRole).toString()) : vine::String{});
    });

    // Right-click menu: the same actions as the detail page, plus the two that
    // need no selection (bring a plugin in / rescan).
    QObject::connect(data->list, &QListWidget::customContextMenuRequested, root, [this](const QPoint& pos) {
        auto* data = dptr();
        auto* item = data->list->itemAt(pos);

        const String selected = item ? Convert::fromQString(item->data(Qt::UserRole).toString()) : String{};
        const auto   entries  = data->manager ? data->manager->pluginEntries() : std::vector<vine::appfw::PluginEntry>{};
        // Named copies: findRegistration()/findEntry() would return pointers into
        // temporaries otherwise.
        const auto  registrations = data->manager ? data->manager->pluginRegistrations()
                                                  : std::vector<vine::appfw::PluginRegistration>{};
        const auto* entry         = findEntry(entries, selected);

        // Same rule as the buttons: only offer the actions that can take effect.
        const bool toggleable = entry != nullptr && entry->scope != vine::appfw::PluginScope::BuiltIn && !entry->skipped;
        const auto* registration = entry != nullptr ? findRegistration(registrations, entry->path) : nullptr;

        QMenu menu(data->list);
        QAction* view_action      = menu.addAction(QStringLiteral("查看详情"));
        view_action->setEnabled(entry != nullptr);
        QAction* toggle_action    = menu.addAction(entry != nullptr && entry->enabled ? QStringLiteral("禁用插件（重启后生效）")
                                                                                   : QStringLiteral("启用插件（重启后生效）"));
        toggle_action->setVisible(toggleable);
        QAction* uninstall_action = menu.addAction(QStringLiteral("卸载插件（移除注册，重启生效）"));
        uninstall_action->setVisible(registration != nullptr);
        QAction* copy_path_action = menu.addAction(QStringLiteral("复制库路径"));
        copy_path_action->setVisible(entry != nullptr && !entry->path.empty());
        menu.addSeparator();
        QAction* load_action              = menu.addAction(QStringLiteral("加载插件…"));
        QAction* install_action           = menu.addAction(QStringLiteral("安装插件（仅当前用户）…"));
        QAction* install_all_users_action = menu.addAction(QStringLiteral("安装插件（所有用户）…"));
        QAction* refresh_action           = menu.addAction(QStringLiteral("刷新"));
        QAction* chosen                   = menu.exec(data->list->viewport()->mapToGlobal(pos));

        if (chosen == view_action) {
            if (item) {
                data->list->setCurrentItem(item);
            }
            showDetail(selected);
        } else if (chosen == toggle_action) {
            if (item) {
                data->list->setCurrentItem(item);
            }
            togglePluginEnabled();
        } else if (chosen == uninstall_action) {
            uninstallSelectedPlugin();
        } else if (chosen == copy_path_action && entry != nullptr) {
            QGuiApplication::clipboard()->setText(QString::fromUtf8(entry->path.u8string().c_str()));
            data->message_label->setText(QStringLiteral("已复制库路径。"));
        } else if (chosen == load_action) {
            loadPlugin();
        } else if (chosen == install_action) {
            installPlugin(vine::appfw::PluginScope::User);
        } else if (chosen == install_all_users_action) {
            installPlugin(vine::appfw::PluginScope::AllUsers);
        } else if (chosen == refresh_action) {
            refresh();
        }
    });

    refresh();
}

PluginManagerDialog::~PluginManagerDialog()
{
    // d is released by UIElement
}

void PluginManagerDialog::loadPlugin()
{
    auto* data = dptr();
    if (!data->manager) {
        return;
    }
    const QString file = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择插件库"), QString(), QStringLiteral("插件库 (*.dll *.so *.dylib)"));
    if (file.isEmpty()) {
        return;
    }

    // Report the outcome: the list only shows the state, and a plugin can be
    // refused (disabled, skipped, not a Vine plugin).
    if (data->manager->load(Convert::fromQString(file)) != nullptr) {
        data->message_label->setText(QStringLiteral("已加载并运行。"));
    }
    else {
        data->message_label->setText(QStringLiteral("加载失败：不是有效的 Vine 插件，或它已被禁用/被程序跳过（详见日志）。"));
    }
    refresh();
}

void PluginManagerDialog::refresh()
{
    auto* data = dptr();

    const String  prev = data->list->currentItem() ? Convert::fromQString(data->list->currentItem()->data(Qt::UserRole).toString())
                                                   : String{};
    const QString filter = data->filter != nullptr ? data->filter->text().trimmed() : QString();

    data->list->blockSignals(true);
    data->list->clear();

    int select_row = -1;
    if (data->manager) {
        // Discovery, not loading: plugins that are disabled, skipped by the host or
        // failed to load are listed as well, with their metadata, so they can be
        // inspected here instead of being invisible.
        for (const auto& entry : data->manager->pluginEntries()) {
            const String  plugin_name = entry.info.name;
            const QString name        = Convert::toQString(plugin_name);
            const QString display =
                entry.info.display_name.empty() ? name : Convert::toQString(entry.info.display_name);

            // The filter matches what the eye can see: name, display name, vendor
            // and description.
            QString haystack = display + QLatin1Char(' ') + name + QLatin1Char(' ') + Convert::toQString(entry.info.vendor)
                               + QLatin1Char(' ') + Convert::toQString(entry.info.description);
            const bool matches = filter.isEmpty() || haystack.toLower().contains(filter.toLower());

            auto* item = new QListWidgetItem(data->list);
            item->setData(Qt::UserRole, name);
            item->setIcon(QIcon(data->iconFor(entry.info.icon, 20)));

            QString text = display;
            if (!entry.info.version.empty()) {
                text += QStringLiteral("  ") + Convert::toQString(entry.info.version);
            }
            text += QStringLiteral("  ·  ") + stateLabel(&entry);
            item->setText(text);

            QString tooltip = display + QStringLiteral("（") + name + QStringLiteral("）");
            if (!entry.info.vendor.empty()) {
                tooltip += QStringLiteral("\n厂商：") + Convert::toQString(entry.info.vendor);
            }
            tooltip += QStringLiteral("\n来源：") + scopeLabel(entry.scope);
            tooltip += QStringLiteral("\n状态：") + stateExplanation(&entry);
            item->setToolTip(tooltip);

            // Not running for a reason that is not the user's choice: say so dimly.
            if (entry.skipped || !entry.enabled) {
                greyOut(item);
            }

            item->setHidden(!matches);
            if (matches && plugin_name == prev) {
                select_row = data->list->count() - 1;
            }
        }
    }
    data->list->blockSignals(false);

    // Keep the selection on a row the filter still shows; otherwise take the first
    // visible one, so a filtered list never leaves the detail page on a stale plugin.
    if (select_row < 0) {
        for (int row = 0; row < data->list->count(); ++row) {
            if (!data->list->item(row)->isHidden()) {
                select_row = row;
                break;
            }
        }
    }

    if (select_row >= 0) {
        data->list->setCurrentRow(select_row);
    }
    else {
        showDetail({});
    }
}

void PluginManagerDialog::installPlugin(vine::appfw::PluginScope scope)
{
    auto* data = dptr();
    if (!data->manager) {
        return;
    }

    // The location may be a plugin library or a directory of them, anywhere on
    // disk: that is how a plugin installed outside the program directory is
    // registered. It takes effect at the next start, so nothing is loaded here.
    const QString file = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择插件库或插件目录"), QString(), QStringLiteral("插件库 (*.dll *.so *.dylib)"));
    if (file.isEmpty()) {
        return;
    }

    const String path = Convert::fromQString(file);
    const String id   = data->manager->installPlugin(path, scope);
    if (!id.empty()) {
        data->message_label->setText(QStringLiteral("已注册（") + scopeLabel(scope) + QStringLiteral("）：") + file
                                     + QStringLiteral("，重启后加载。"));
    }
    else if (scope == vine::appfw::PluginScope::AllUsers) {
        data->message_label->setText(QStringLiteral("为所有用户注册失败（需要管理员权限，或文件不存在）：") + file
                                     + QStringLiteral("；可改用“仅当前用户”安装。"));
    }
    else {
        data->message_label->setText(QStringLiteral("注册失败（文件不存在或无法写入注册目录）：") + file);
    }
    refresh();
}

void PluginManagerDialog::uninstallSelectedPlugin()
{
    auto* data = dptr();
    if (!data->manager || !data->list->currentItem()) {
        return;
    }

    const String name    = Convert::fromQString(data->list->currentItem()->data(Qt::UserRole).toString());
    const auto   entries = data->manager->pluginEntries();
    const auto*  entry   = findEntry(entries, name);
    if (entry == nullptr) {
        return;
    }

    // Only a registered plugin can be uninstalled: what ships with the
    // application is removed by the application, not by the user. The
    // registration list is kept in a named local so the pointer stays valid.
    const auto  registrations = data->manager->pluginRegistrations();
    const auto* registration  = findRegistration(registrations, entry->path);
    if (registration == nullptr) {
        data->message_label->setText(QStringLiteral("该插件由程序自带，不能卸载。"));
        return;
    }

    // Removing the registration takes effect at the next start; a running plugin
    // keeps running until then (see PluginManager::uninstallPlugin).
    const bool removed = data->manager->uninstallPlugin(registration->id, registration->scope);
    data->message_label->setText(removed ? QStringLiteral("已移除注册（") + scopeLabel(registration->scope)
                                               + QStringLiteral("）：重启后不再加载。")
                                        : QStringLiteral("移除注册失败：注册文件不存在或不可写。"));
    refresh();
}

void PluginManagerDialog::togglePluginEnabled()
{
    auto* data = dptr();
    if (!data->manager || !data->list->currentItem()) {
        return;
    }

    const String name    = Convert::fromQString(data->list->currentItem()->data(Qt::UserRole).toString());
    const auto   entries = data->manager->pluginEntries();
    const auto*  entry   = findEntry(entries, name);
    if (entry == nullptr) {
        return;
    }

    // Enabling and disabling are persisted preferences, not runtime loads: the
    // running state is untouched and the change applies at the next start. An
    // application-provided plugin is refused (see PluginManager::setPluginEnabled).
    if (data->manager->setPluginEnabled(name, !entry->enabled)) {
        data->message_label->setText(entry->enabled ? QStringLiteral("已禁用，重启后生效。")
                                                    : QStringLiteral("已启用，重启后生效。"));
    }
    else {
        data->message_label->setText(QStringLiteral("程序自带的插件不能禁用；它由程序决定是否随包发布。"));
    }
    refresh();
    showDetail(name);
}

void PluginManagerDialog::showDetail(const vine::String& name)
{
    auto* data = dptr();
    if (name.empty() || !data->manager) {
        data->stack->setCurrentIndex(0);
        data->toggle_btn->setVisible(false);
        data->uninstall_btn->setVisible(false);
        data->current_repo.clear();
        return;
    }

    const auto  entries = data->manager->pluginEntries();
    const auto* entry   = findEntry(entries, name);

    // The registration list is kept in a named local: findRegistration() would
    // return a pointer into a temporary otherwise.
    const auto  registrations = data->manager->pluginRegistrations();
    const auto* registration  = entry != nullptr ? findRegistration(registrations, entry->path) : nullptr;

    const QString id      = Convert::toQString(name);
    const QString display = entry != nullptr && !entry->info.display_name.empty()
                                ? Convert::toQString(entry->info.display_name)
                                : id;

    // ---- Header: icon, title, identity, vendor contact, state badge ----
    data->icon_label->setPixmap(data->iconFor(entry != nullptr ? entry->info.icon : vine::String{}, 56));
    data->title_label->setText(display);

    QString subtitle = id;
    if (entry != nullptr && !entry->info.version.empty()) {
        subtitle += QStringLiteral("  ·  v") + Convert::toQString(entry->info.version);
    }
    data->subtitle_label->setText(subtitle);

    // Vendor and mail address as one clickable line; both are optional.
    QString meta;
    const auto append_meta = [&meta](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        if (!meta.isEmpty()) {
            meta += QStringLiteral("  ·  ");
        }
        meta += text;
    };
    if (entry != nullptr) {
        append_meta(Convert::toQString(entry->info.vendor));
        if (!entry->info.email.empty()) {
            const QString mail = Convert::toQString(entry->info.email).toHtmlEscaped();
            append_meta(QStringLiteral("<a href=\"mailto:%1\">%1</a>").arg(mail));
        }
    }
    data->meta_label->setText(meta);
    data->meta_label->setVisible(!meta.isEmpty());

    data->status_badge->setText(stateLabel(entry));
    data->status_badge->setStyleSheet(badgeStyle(entry));
    data->status_label->setText(stateExplanation(entry));

    // ---- Actions: only what can take effect is shown ----
    const bool toggleable = entry != nullptr && entry->scope != vine::appfw::PluginScope::BuiltIn && !entry->skipped;
    data->toggle_btn->setVisible(toggleable);
    if (toggleable) {
        data->toggle_btn->setText(entry->enabled ? QStringLiteral("禁用插件（重启后生效）")
                                                 : QStringLiteral("启用插件（重启后生效）"));
    }

    data->uninstall_btn->setVisible(registration != nullptr);
    if (registration != nullptr) {
        data->uninstall_btn->setText(QStringLiteral("卸载插件（") + scopeLabel(registration->scope) + QStringLiteral("）"));
    }

    data->current_repo = entry != nullptr && !entry->info.repo.empty() ? Convert::toQString(entry->info.repo) : QString();

    // ---- Info tab: the description leads, then the metadata ----
    data->desc_label->setText(entry != nullptr && !entry->info.description.empty()
                                  ? Convert::toQString(entry->info.description)
                                  : QStringLiteral("—"));
    const auto value = [](const vine::String& text) { return text.empty() ? QStringLiteral("—") : Convert::toQString(text); };

    data->id_label->setText(display == id ? id : display + QStringLiteral("（") + id + QStringLiteral("）"));
    data->version_label->setText(entry != nullptr ? value(entry->info.version) : QStringLiteral("—"));
    data->scope_label->setText(entry != nullptr ? scopeLabel(entry->scope)
                                                       + (registration != nullptr ? QStringLiteral("  ·  可卸载")
                                                                                  : QStringLiteral("  ·  不可禁用或卸载"))
                                                : QStringLiteral("—"));
    data->dep_label->setText(entry != nullptr ? joinStrings(entry->info.dependencies) : QStringLiteral("—"));
    data->uuid_label->setText(entry != nullptr && !entry->info.uuid.isNull() ? Convert::toQString(entry->info.uuid.toString())
                                                                            : QStringLiteral("—"));
    data->path_label->setText(entry != nullptr && !entry->path.empty() ? QString::fromUtf8(entry->path.u8string().c_str())
                                                                      : QStringLiteral("—"));
    data->path_label->setToolTip(data->path_label->text());
    // The version the library was built with (PluginAbi): a plugin compiled against
    // another framework build is usually the first thing to check when it misbehaves.
    data->built_label->setText(entry != nullptr ? value(entry->framework_version) : QStringLiteral("—"));
    data->built_label->setToolTip(QStringLiteral("本程序内置的框架版本：%1").arg(QStringLiteral(V_APPFW_VERSION)));
    data->email_label->setText(entry != nullptr && !entry->info.email.empty()
                                   ? QStringLiteral("<a href=\"mailto:%1\">%1</a>")
                                         .arg(Convert::toQString(entry->info.email).toHtmlEscaped())
                                   : QStringLiteral("—"));
    data->repo_label->setText(!data->current_repo.isEmpty()
                                  ? QStringLiteral("<a href=\"%1\">%2</a>").arg(data->current_repo, data->current_repo.toHtmlEscaped())
                                  : QStringLiteral("—"));

    // Commands reported by the plugin.
    data->cmd_table->setRowCount(0);
    for (const auto& ci : data->manager->commandInfosForPlugin(name)) {
        const int row = data->cmd_table->rowCount();
        data->cmd_table->insertRow(row);
        data->cmd_table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(ci.name)));
        data->cmd_table->setItem(row, 1, new QTableWidgetItem(Convert::toQString(ci.group)));
        data->cmd_table->setItem(row, 2, new QTableWidgetItem(Convert::toQString(ci.description)));
    }

    // Config items reported by the plugin.
    data->cfg_table->setRowCount(0);
    for (const auto* item : data->manager->configItemsForPlugin(name)) {
        if (item == nullptr) {
            continue;
        }
        const int row = data->cfg_table->rowCount();
        data->cfg_table->insertRow(row);
        data->cfg_table->setItem(row, 0, new QTableWidgetItem(Convert::toQString(item->key())));
        data->cfg_table->setItem(row, 1, new QTableWidgetItem(Convert::toQString(item->label())));
        data->cfg_table->setItem(row, 2, new QTableWidgetItem(Convert::toQString(item->description())));
    }

    data->stack->setCurrentIndex(1);
}

inline auto PluginManagerDialog::dptr() -> Impl*
{
    return static_cast<Impl*>(UIElement::d);
}

inline auto PluginManagerDialog::dptr() const -> const Impl*
{
    return static_cast<const Impl*>(UIElement::d);
}

V_APPFWGUI_NS_END
