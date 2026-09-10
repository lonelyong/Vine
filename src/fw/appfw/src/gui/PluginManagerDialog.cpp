#include <vine/appfw/gui/PluginManagerDialog.hpp>

#include <QAbstractItemView>
#include <QAction>
#include <QBrush>
#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDialog>
#include <QFileDialog>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
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
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/gui/UIElementData.hpp>

#include "Convert.hpp"

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
    vine::appfw::PluginManager* manager       = nullptr;
    QLineEdit*                  filter        = nullptr;
    QListWidget*                list          = nullptr;
    QPushButton*                loadBtn       = nullptr;
    QToolButton*                installBtn    = nullptr;
    QStackedWidget*             stack         = nullptr;
    QLabel*                     iconLabel     = nullptr;
    QLabel*                     titleLabel    = nullptr;
    QLabel*                     subtitleLabel = nullptr;
    QLabel*                     metaLabel     = nullptr;
    QLabel*                     statusBadge   = nullptr;
    QLabel*                     statusLabel   = nullptr;
    QLabel*                     descLabel     = nullptr;
    QLabel*                     idLabel       = nullptr;
    QLabel*                     versionLabel  = nullptr;
    QLabel*                     scopeLabel    = nullptr;
    QLabel*                     depLabel      = nullptr;
    QLabel*                     uuidLabel     = nullptr;
    QLabel*                     pathLabel     = nullptr;
    QLabel*                     emailLabel    = nullptr;
    QLabel*                     repoLabel     = nullptr;
    QLabel*                     messageLabel  = nullptr;
    QTableWidget*               cmdTable      = nullptr;
    QTableWidget*               cfgTable      = nullptr;
    QPushButton*                toggleBtn     = nullptr;
    QPushButton*                uninstallBtn  = nullptr;
    QPushButton*                repoBtn       = nullptr;

    /// Repository URL of the selected plugin; the "open repository" button uses it.
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
    auto* left    = new QWidget(splitter);
    auto* leftLay = new QVBoxLayout(left);
    leftLay->setContentsMargins(0, 0, 0, 0);
    leftLay->setSpacing(6);

    data->filter = new QLineEdit(left);
    data->filter->setPlaceholderText(QStringLiteral("筛选（名称 / 显示名 / 厂商 / 描述）"));
    data->filter->setClearButtonEnabled(true);
    leftLay->addWidget(data->filter);

    data->list = new QListWidget(left);
    data->list->setSelectionMode(QAbstractItemView::SingleSelection);
    data->list->setContextMenuPolicy(Qt::CustomContextMenu);
    data->list->setIconSize(QSize(20, 20));
    data->list->setUniformItemSizes(true);
    leftLay->addWidget(data->list, 1);

    data->loadBtn    = new QPushButton(QStringLiteral("加载插件…"), left);
    data->loadBtn->setToolTip(QStringLiteral("立即加载并运行，不写注册表（重启后不保留）。"));

    data->installBtn = new QToolButton(left);
    data->installBtn->setText(QStringLiteral("安装插件"));
    data->installBtn->setPopupMode(QToolButton::InstantPopup);
    data->installBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    data->installBtn->setToolTip(QStringLiteral("注册一个插件位置（库文件或目录，可在程序目录之外）；重启后加载。"));
    auto*    installMenu     = new QMenu(data->installBtn);
    QAction* installForUser  = installMenu->addAction(QStringLiteral("仅当前用户…"));
    QAction* installForUsers = installMenu->addAction(QStringLiteral("所有用户…"));
    data->installBtn->setMenu(installMenu);

    auto* listActions = new QHBoxLayout();
    listActions->setSpacing(6);
    listActions->addWidget(data->loadBtn);
    listActions->addWidget(data->installBtn);
    listActions->addStretch();
    leftLay->addLayout(listActions);

    splitter->addWidget(left);

    // ---- Right: placeholder + detail page ----
    data->stack = new QStackedWidget(splitter);

    // Placeholder: what the dialog is for, so an empty right pane is not a blank.
    auto* placeholder    = new QWidget(data->stack);
    auto* placeholderLay = new QVBoxLayout(placeholder);
    placeholderLay->addStretch();

    auto* placeholderIcon = new QLabel(placeholder);
    placeholderIcon->setPixmap(renderSvgIcon(QString::fromUtf8(s_default_plugin_icon_svg), 64));
    placeholderIcon->setAlignment(Qt::AlignCenter);

    auto* placeholderText = new QLabel(QStringLiteral("选择一个插件查看详情"), placeholder);
    placeholderText->setAlignment(Qt::AlignCenter);
    QFont placeholderFont = placeholderText->font();
    placeholderFont.setPointSizeF(placeholderFont.pointSizeF() + 1.0);
    placeholderText->setFont(placeholderFont);

    auto* placeholderHint = new QLabel(QStringLiteral("详情页显示来源、依赖、命令与配置，并给出可执行的操作。"), placeholder);
    placeholderHint->setAlignment(Qt::AlignCenter);
    placeholderHint->setStyleSheet(QStringLiteral("color: gray;"));

    placeholderLay->addWidget(placeholderIcon);
    placeholderLay->addSpacing(6);
    placeholderLay->addWidget(placeholderText);
    placeholderLay->addWidget(placeholderHint);
    placeholderLay->addStretch();
    data->stack->addWidget(placeholder);

    // The detail page scrolls: a header, four groups and two tables must not be
    // squashed by a small window (and the tables keep their own scrollbars).
    auto* detailScroll = new QScrollArea(data->stack);
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);

    auto* detail    = new QWidget(detailScroll);
    auto* detailLay = new QVBoxLayout(detail);
    detailLay->setContentsMargins(12, 4, 8, 4);
    detailLay->setSpacing(10);
    detailScroll->setWidget(detail);
    data->stack->addWidget(detailScroll);

    // Header: icon, display name, identity line, vendor contact and state badge.
    auto* header    = new QWidget(detail);
    auto* headerLay = new QHBoxLayout(header);
    headerLay->setContentsMargins(0, 0, 0, 0);
    headerLay->setSpacing(12);

    data->iconLabel = new QLabel(header);
    data->iconLabel->setFixedSize(56, 56);
    data->iconLabel->setAlignment(Qt::AlignCenter);
    headerLay->addWidget(data->iconLabel, 0, Qt::AlignTop);

    auto* titleCol = new QVBoxLayout();
    titleCol->setSpacing(2);

    data->titleLabel = new QLabel(header);
    QFont titleFont  = data->titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 3.0);
    titleFont.setBold(true);
    data->titleLabel->setFont(titleFont);
    data->titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    data->titleLabel->setWordWrap(true);

    data->subtitleLabel = new QLabel(header);
    data->subtitleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    data->subtitleLabel->setStyleSheet(QStringLiteral("color: gray;"));

    data->metaLabel = new QLabel(header);
    data->metaLabel->setTextFormat(Qt::RichText);
    data->metaLabel->setOpenExternalLinks(true);
    data->metaLabel->setWordWrap(true);

    titleCol->addWidget(data->titleLabel);
    titleCol->addWidget(data->subtitleLabel);
    titleCol->addWidget(data->metaLabel);
    titleCol->addStretch();
    headerLay->addLayout(titleCol, 1);

    data->statusBadge = new QLabel(header);
    data->statusBadge->setAlignment(Qt::AlignCenter);
    headerLay->addWidget(data->statusBadge, 0, Qt::AlignTop);
    detailLay->addWidget(header);

    // The badge is the glance; this line is the reason (and what to do about it).
    data->statusLabel = new QLabel(detail);
    data->statusLabel->setWordWrap(true);
    data->statusLabel->setFrameShape(QFrame::StyledPanel);
    data->statusLabel->setMargin(6);
    data->statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLay->addWidget(data->statusLabel);

    // Actions: showDetail() shows only the ones that can take effect.
    data->toggleBtn    = new QPushButton(QStringLiteral("禁用插件"), detail);
    data->uninstallBtn = new QPushButton(QStringLiteral("卸载插件"), detail);
    data->repoBtn      = new QPushButton(QStringLiteral("打开仓库"), detail);
    data->toggleBtn->setToolTip(QStringLiteral("只写偏好，不动本次运行；下次启动生效。"));
    data->uninstallBtn->setToolTip(QStringLiteral("移除注册文件，重启后不再加载。"));
    data->repoBtn->setToolTip(QStringLiteral("在浏览器中打开插件的仓库或问题跟踪地址。"));

    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(6);
    actionRow->addWidget(data->toggleBtn);
    actionRow->addWidget(data->uninstallBtn);
    actionRow->addWidget(data->repoBtn);
    actionRow->addStretch();
    detailLay->addLayout(actionRow);
    data->toggleBtn->setVisible(false);
    data->uninstallBtn->setVisible(false);
    data->repoBtn->setVisible(false);

    // Description.
    auto* descBox = new QGroupBox(QStringLiteral("描述"), detail);
    auto* descLay = new QVBoxLayout(descBox);
    descLay->setContentsMargins(8, 6, 8, 6);
    data->descLabel = new QLabel(descBox);
    data->descLabel->setWordWrap(true);
    data->descLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    descLay->addWidget(data->descLabel);
    detailLay->addWidget(descBox);

    // Metadata: identity, origin, what it needs, where it lives, who wrote it.
    auto* infoBox = new QGroupBox(QStringLiteral("信息"), detail);
    auto* form    = new QFormLayout(infoBox);
    form->setContentsMargins(8, 6, 8, 6);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    const auto makeValue = [infoBox] {
        auto* label = new QLabel(infoBox);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
        label->setWordWrap(true);
        return label;
    };

    data->idLabel      = makeValue();
    data->versionLabel = makeValue();
    data->scopeLabel   = makeValue();
    data->depLabel     = makeValue();
    data->uuidLabel    = makeValue();
    data->pathLabel    = makeValue();
    data->emailLabel   = makeValue();
    data->repoLabel    = makeValue();

    // Identifiers and paths read better in a monospaced font.
    QFont mono = data->uuidLabel->font();
    mono.setFamilies({ QStringLiteral("monospace"), QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono") });
    data->uuidLabel->setFont(mono);
    data->pathLabel->setFont(mono);

    form->addRow(QStringLiteral("标识"), data->idLabel);
    form->addRow(QStringLiteral("版本"), data->versionLabel);
    form->addRow(QStringLiteral("来源"), data->scopeLabel);
    form->addRow(QStringLiteral("依赖"), data->depLabel);
    form->addRow(QStringLiteral("UUID"), data->uuidLabel);
    form->addRow(QStringLiteral("库路径"), data->pathLabel);
    form->addRow(QStringLiteral("邮箱"), data->emailLabel);
    form->addRow(QStringLiteral("仓库"), data->repoLabel);
    detailLay->addWidget(infoBox);

    // Commands reported by the plugin.
    data->cmdTable = new QTableWidget(0, 3, detail);
    data->cmdTable->setHorizontalHeaderLabels({ QStringLiteral("命令"), QStringLiteral("分组"), QStringLiteral("描述") });
    data->cmdTable->horizontalHeader()->setStretchLastSection(true);
    data->cmdTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->cmdTable->setSelectionMode(QAbstractItemView::SingleSelection);
    data->cmdTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->cmdTable->setMinimumHeight(120);
    auto* cmdBox = new QGroupBox(QStringLiteral("命令"), detail);
    auto* cmdLay = new QVBoxLayout(cmdBox);
    cmdLay->setContentsMargins(8, 6, 8, 6);
    cmdLay->addWidget(data->cmdTable);
    detailLay->addWidget(cmdBox, 1);

    // Config items reported by the plugin.
    data->cfgTable = new QTableWidget(0, 3, detail);
    data->cfgTable->setHorizontalHeaderLabels({ QStringLiteral("键"), QStringLiteral("标签"), QStringLiteral("描述") });
    data->cfgTable->horizontalHeader()->setStretchLastSection(true);
    data->cfgTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    data->cfgTable->setSelectionMode(QAbstractItemView::SingleSelection);
    data->cfgTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    data->cfgTable->setMinimumHeight(120);
    auto* cfgBox = new QGroupBox(QStringLiteral("配置"), detail);
    auto* cfgLay = new QVBoxLayout(cfgBox);
    cfgLay->setContentsMargins(8, 6, 8, 6);
    cfgLay->addWidget(data->cfgTable);
    detailLay->addWidget(cfgBox, 1);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 300, 620 });

    // ---- Bottom: feedback on the left, dialog commands on the right ----
    auto* bottomLay = new QHBoxLayout();
    bottomLay->setSpacing(8);

    data->messageLabel = new QLabel(root);
    data->messageLabel->setWordWrap(true);
    data->messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottomLay->addWidget(data->messageLabel, 1);

    auto* refreshBtn = new QPushButton(QStringLiteral("刷新"), root);
    auto* closeBtn   = new QPushButton(QStringLiteral("关闭"), root);
    refreshBtn->setToolTip(QStringLiteral("重新扫描自带插件目录与已注册的位置。"));
    bottomLay->addWidget(refreshBtn);
    bottomLay->addWidget(closeBtn);
    outer->addLayout(bottomLay);

    QObject::connect(data->filter, &QLineEdit::textChanged, root, [this] { refresh(); });
    QObject::connect(data->loadBtn, &QPushButton::clicked, root, [this] { loadPlugin(); });
    QObject::connect(installForUser, &QAction::triggered, root, [this] { installPlugin(vine::appfw::PluginScope::User); });
    QObject::connect(installForUsers, &QAction::triggered, root, [this] { installPlugin(vine::appfw::PluginScope::AllUsers); });
    QObject::connect(data->toggleBtn, &QPushButton::clicked, root, [this] { togglePluginEnabled(); });
    QObject::connect(data->uninstallBtn, &QPushButton::clicked, root, [this] { uninstallSelectedPlugin(); });
    QObject::connect(data->repoBtn, &QPushButton::clicked, root, [this] {
        auto* data = dptr();
        if (!data->current_repo.isEmpty()) {
            QDesktopServices::openUrl(QUrl(data->current_repo));
        }
    });
    QObject::connect(refreshBtn, &QPushButton::clicked, root, [this] { refresh(); });
    QObject::connect(closeBtn, &QPushButton::clicked, root, [root] { root->close(); });

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
        QAction* viewAction     = menu.addAction(QStringLiteral("查看详情"));
        viewAction->setEnabled(entry != nullptr);
        QAction* toggleAction   = menu.addAction(entry != nullptr && entry->enabled ? QStringLiteral("禁用插件（重启后生效）")
                                                                                 : QStringLiteral("启用插件（重启后生效）"));
        toggleAction->setVisible(toggleable);
        QAction* uninstallAction = menu.addAction(QStringLiteral("卸载插件（移除注册，重启生效）"));
        uninstallAction->setVisible(registration != nullptr);
        QAction* copyPathAction  = menu.addAction(QStringLiteral("复制库路径"));
        copyPathAction->setVisible(entry != nullptr && !entry->path.empty());
        menu.addSeparator();
        QAction* loadAction            = menu.addAction(QStringLiteral("加载插件…"));
        QAction* installAction         = menu.addAction(QStringLiteral("安装插件（仅当前用户）…"));
        QAction* installAllUsersAction = menu.addAction(QStringLiteral("安装插件（所有用户）…"));
        QAction* refreshAction         = menu.addAction(QStringLiteral("刷新"));
        QAction* chosen                = menu.exec(data->list->viewport()->mapToGlobal(pos));

        if (chosen == viewAction) {
            if (item) {
                data->list->setCurrentItem(item);
            }
            showDetail(selected);
        } else if (chosen == toggleAction) {
            if (item) {
                data->list->setCurrentItem(item);
            }
            togglePluginEnabled();
        } else if (chosen == uninstallAction) {
            uninstallSelectedPlugin();
        } else if (chosen == copyPathAction && entry != nullptr) {
            QGuiApplication::clipboard()->setText(QString::fromUtf8(entry->path.u8string().c_str()));
            data->messageLabel->setText(QStringLiteral("已复制库路径。"));
        } else if (chosen == loadAction) {
            loadPlugin();
        } else if (chosen == installAction) {
            installPlugin(vine::appfw::PluginScope::User);
        } else if (chosen == installAllUsersAction) {
            installPlugin(vine::appfw::PluginScope::AllUsers);
        } else if (chosen == refreshAction) {
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
        data->messageLabel->setText(QStringLiteral("已加载并运行。"));
    }
    else {
        data->messageLabel->setText(QStringLiteral("加载失败：不是有效的 Vine 插件，或它已被禁用/被程序跳过（详见日志）。"));
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

    int selectRow = -1;
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
                selectRow = data->list->count() - 1;
            }
        }
    }
    data->list->blockSignals(false);

    // Keep the selection on a row the filter still shows; otherwise take the first
    // visible one, so a filtered list never leaves the detail page on a stale plugin.
    if (selectRow < 0) {
        for (int row = 0; row < data->list->count(); ++row) {
            if (!data->list->item(row)->isHidden()) {
                selectRow = row;
                break;
            }
        }
    }

    if (selectRow >= 0) {
        data->list->setCurrentRow(selectRow);
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
        data->messageLabel->setText(QStringLiteral("已注册（") + scopeLabel(scope) + QStringLiteral("）：") + file
                                    + QStringLiteral("，重启后加载。"));
    }
    else if (scope == vine::appfw::PluginScope::AllUsers) {
        data->messageLabel->setText(QStringLiteral("为所有用户注册失败（需要管理员权限，或文件不存在）：") + file
                                    + QStringLiteral("；可改用“仅当前用户”安装。"));
    }
    else {
        data->messageLabel->setText(QStringLiteral("注册失败（文件不存在或无法写入注册目录）：") + file);
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
        data->messageLabel->setText(QStringLiteral("该插件由程序自带，不能卸载。"));
        return;
    }

    // Removing the registration takes effect at the next start; a running plugin
    // keeps running until then (see PluginManager::uninstallPlugin).
    const bool removed = data->manager->uninstallPlugin(registration->id, registration->scope);
    data->messageLabel->setText(removed ? QStringLiteral("已移除注册（") + scopeLabel(registration->scope)
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
        data->messageLabel->setText(entry->enabled ? QStringLiteral("已禁用，重启后生效。")
                                                   : QStringLiteral("已启用，重启后生效。"));
    }
    else {
        data->messageLabel->setText(QStringLiteral("程序自带的插件不能禁用；它由程序决定是否随包发布。"));
    }
    refresh();
    showDetail(name);
}

void PluginManagerDialog::showDetail(const vine::String& name)
{
    auto* data = dptr();
    if (name.empty() || !data->manager) {
        data->stack->setCurrentIndex(0);
        data->toggleBtn->setVisible(false);
        data->uninstallBtn->setVisible(false);
        data->repoBtn->setVisible(false);
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
    data->iconLabel->setPixmap(data->iconFor(entry != nullptr ? entry->info.icon : vine::String{}, 56));
    data->titleLabel->setText(display);

    QString subtitle = id;
    if (entry != nullptr && !entry->info.version.empty()) {
        subtitle += QStringLiteral("  ·  v") + Convert::toQString(entry->info.version);
    }
    data->subtitleLabel->setText(subtitle);

    // Vendor and mail address as one clickable line; both are optional.
    QString meta;
    const auto appendMeta = [&meta](const QString& text) {
        if (text.isEmpty()) {
            return;
        }
        if (!meta.isEmpty()) {
            meta += QStringLiteral("  ·  ");
        }
        meta += text;
    };
    if (entry != nullptr) {
        appendMeta(Convert::toQString(entry->info.vendor));
        if (!entry->info.email.empty()) {
            const QString mail = Convert::toQString(entry->info.email).toHtmlEscaped();
            appendMeta(QStringLiteral("<a href=\"mailto:%1\">%1</a>").arg(mail));
        }
    }
    data->metaLabel->setText(meta);
    data->metaLabel->setVisible(!meta.isEmpty());

    data->statusBadge->setText(stateLabel(entry));
    data->statusBadge->setStyleSheet(badgeStyle(entry));
    data->statusLabel->setText(stateExplanation(entry));

    // ---- Actions: only what can take effect is shown ----
    const bool toggleable = entry != nullptr && entry->scope != vine::appfw::PluginScope::BuiltIn && !entry->skipped;
    data->toggleBtn->setVisible(toggleable);
    if (toggleable) {
        data->toggleBtn->setText(entry->enabled ? QStringLiteral("禁用插件（重启后生效）")
                                                : QStringLiteral("启用插件（重启后生效）"));
    }

    data->uninstallBtn->setVisible(registration != nullptr);
    if (registration != nullptr) {
        data->uninstallBtn->setText(QStringLiteral("卸载插件（") + scopeLabel(registration->scope) + QStringLiteral("）"));
    }

    data->current_repo = entry != nullptr && !entry->info.repo.empty() ? Convert::toQString(entry->info.repo) : QString();
    data->repoBtn->setVisible(!data->current_repo.isEmpty());

    // ---- Description ----
    data->descLabel->setText(entry != nullptr && !entry->info.description.empty()
                                 ? Convert::toQString(entry->info.description)
                                 : QStringLiteral("—"));

    // ---- Metadata ----
    const auto value = [](const vine::String& text) { return text.empty() ? QStringLiteral("—") : Convert::toQString(text); };

    data->idLabel->setText(display == id ? id : display + QStringLiteral("（") + id + QStringLiteral("）"));
    data->versionLabel->setText(entry != nullptr ? value(entry->info.version) : QStringLiteral("—"));
    data->scopeLabel->setText(entry != nullptr ? scopeLabel(entry->scope)
                                                      + (registration != nullptr ? QStringLiteral("  ·  可卸载")
                                                                                 : QStringLiteral("  ·  不可禁用或卸载"))
                                               : QStringLiteral("—"));
    data->depLabel->setText(entry != nullptr ? joinStrings(entry->info.dependencies) : QStringLiteral("—"));
    data->uuidLabel->setText(entry != nullptr && !entry->info.uuid.isNull() ? Convert::toQString(entry->info.uuid.toString())
                                                                           : QStringLiteral("—"));
    data->pathLabel->setText(entry != nullptr && !entry->path.empty() ? QString::fromUtf8(entry->path.u8string().c_str())
                                                                     : QStringLiteral("—"));
    data->pathLabel->setToolTip(data->pathLabel->text());
    data->emailLabel->setText(entry != nullptr && !entry->info.email.empty()
                                  ? QStringLiteral("<a href=\"mailto:%1\">%1</a>")
                                        .arg(Convert::toQString(entry->info.email).toHtmlEscaped())
                                  : QStringLiteral("—"));
    data->repoLabel->setText(!data->current_repo.isEmpty()
                                 ? QStringLiteral("<a href=\"%1\">%2</a>").arg(data->current_repo, data->current_repo.toHtmlEscaped())
                                 : QStringLiteral("—"));

    // Commands reported by the plugin.
    data->cmdTable->setRowCount(0);
    for (const auto& ci : data->manager->commandInfosForPlugin(name)) {
        const int row = data->cmdTable->rowCount();
        data->cmdTable->insertRow(row);
        data->cmdTable->setItem(row, 0, new QTableWidgetItem(Convert::toQString(ci.name)));
        data->cmdTable->setItem(row, 1, new QTableWidgetItem(Convert::toQString(ci.group)));
        data->cmdTable->setItem(row, 2, new QTableWidgetItem(Convert::toQString(ci.description)));
    }

    // Config items reported by the plugin.
    data->cfgTable->setRowCount(0);
    for (const auto* item : data->manager->configItemsForPlugin(name)) {
        if (item == nullptr) {
            continue;
        }
        const int row = data->cfgTable->rowCount();
        data->cfgTable->insertRow(row);
        data->cfgTable->setItem(row, 0, new QTableWidgetItem(Convert::toQString(item->key())));
        data->cfgTable->setItem(row, 1, new QTableWidgetItem(Convert::toQString(item->label())));
        data->cfgTable->setItem(row, 2, new QTableWidgetItem(Convert::toQString(item->description())));
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
