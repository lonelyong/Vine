// test_gui.cpp
//
// GUI 层单元测试：功能区（RibbonBar/Tab/Group/Button/DropDownItem）的构造、
// 属性与事件，以及停靠面板（DockPanelManager/DockPanel）的管理与状态切换。
//
// 运行前提：
//   - 需要真实显示环境（本地运行，会短暂弹出窗口）。
//   - 整个测试进程共享一个 GuiApplication（在全局 Environment 中创建，
//     因为 Application 单例只允许一个实例）。
//   - RibbonTab/Group/Button 与 DockPanel 包装对象刻意不释放（与 demo 一致）；
//     其 Qt impl 由 SARibbon/DockingPanes 持有，进程退出时统一回收。

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <SARibbon.h>

#include <vine/vi_global.hpp>

#include <vine/appfw/Command.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/ConfigStandard.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/UserIO.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/gui/ConfigWindow.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/appfw/gui/GuiAppBuilder.hpp>
#include <vine/appfw/gui/PluginManagerDialog.hpp>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/appfw/gui/ProgressPresenter.hpp>

#include <vine/progress/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

#include <vine/async/AsyncEvent.hpp>
#include <vine/async/Sleep.hpp>
#include <vine/async/SyncWait.hpp>

#include <any>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <thread>

#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>

namespace guifw = vine::appfw::gui;

namespace
{

// 整个测试进程共享一个 GuiApplication（必须在任何 QWidget 之前创建）。
class GuiEnv : public ::testing::Environment {
  public:
    void SetUp() override
    {
        guifw::GuiApplication::desc();

        // 用户数据/配置位置重定向到 Qt 测试目录：测试既不读也不写开发机上的
        // 真实配置（<user data>/<org>/<app>/... 的默认布局仍然成立）。
        QStandardPaths::setTestModeEnabled(true);

        static char  arg0[] = "test_gui";
        static char* argv[] = { arg0, nullptr };

        // 与真实应用一样走 builder：它会应用应用身份并启用默认配置文件。
        vine::appfw::AppConfig config;
        config.name = "test_gui";
        app         = guifw::createGuiApplication(config, 1, argv);

        // 从干净状态开始：上一次运行（或中途失败）可能留下插件注册文件。
        // Qt 测试模式已经把数据目录重定向到临时区，删掉它是安全的。
        std::error_code ec;
        std::filesystem::remove_all(app->dataDirectory(), ec);
    }

    void TearDown() override
    {
        app.reset();
    }

    static std::unique_ptr<guifw::GuiApplication> app;
};

std::unique_ptr<guifw::GuiApplication> GuiEnv::app;

// gtest_main 没有自定义 main 的钩子，用静态初始化注册全局环境即可。
::testing::Environment* const g_gui_env = ::testing::AddGlobalTestEnvironment(new GuiEnv());

// 最小具体命令，用于验证 CommandManager 的 owner 跟踪与按插件报告。
class DummyCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"dummy"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"dummy command"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }
    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }
};

V_OBJECT_META_IMPL(DummyCommand, vine::appfw::Command)

// 耗时命令：LongRunning 标志让 CommandManager 自动挂载环境进度宿主。
class LongCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"long"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"long running command"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::LongRunning; }
    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        co_await vine::async::sleepFor(std::chrono::milliseconds(120));
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }
};

V_OBJECT_META_IMPL(LongCommand, vine::appfw::Command)

// 嵌套子命令：LongRunning，复用父命令的取消/进度宿主。
class NestedChildCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"nested_child"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"nested child command"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::LongRunning; }
    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        // 子命令现在是独立宿主（前台栈顶），可上报自己的进度。
        if (auto* host = vine::progress::ProgressHost::current()) {
            host->setLabel("child");
            vine::progress::ProgressScope scope = host->scope("child", 20);
            for (int i = 0; i < 20; ++i) {
                if (context && context->isCancelled()) {
                    co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Cancelled);
                }
                scope.next(1);
                co_await vine::async::sleepFor(std::chrono::milliseconds(10));
            }
        }
        else {
            for (int i = 0; i < 20; ++i) {
                if (context && context->isCancelled()) {
                    co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Cancelled);
                }
                co_await vine::async::sleepFor(std::chrono::milliseconds(10));
            }
        }
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }
};

V_OBJECT_META_IMPL(NestedChildCommand, vine::appfw::Command)

// 嵌套父命令：LongRunning，报告进度并通过 context->executeChild 调用子命令。
class NestedProgressCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"nested_progress"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"nested progress command"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::LongRunning; }
    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* host = vine::progress::ProgressHost::current();
        if (!host || !context) {
            co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Failed);
        }
        host->setLabel("parent");
        vine::progress::ProgressScope root = host->scope("parent", 20);
        for (int i = 0; i < 8; ++i) {
            if (root.isCancelled()) {
                co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Cancelled);
            }
            root.next(1);
            co_await vine::async::sleepFor(std::chrono::milliseconds(5));
        }
        const auto child_result = co_await context->executeChild(u8"nested_child");
        if (!child_result.succeeded()) {
            co_return child_result;
        }
        for (int i = 0; i < 12; ++i) {
            if (root.isCancelled()) {
                co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Cancelled);
            }
            root.next(1);
            co_await vine::async::sleepFor(std::chrono::milliseconds(5));
        }
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }
};

V_OBJECT_META_IMPL(NestedProgressCommand, vine::appfw::Command)

} // namespace

// ---------------------------------------------------------------------------
// 每用例独立构建一个 MainWindow + 功能区 + 停靠面板，互不干扰。
// ---------------------------------------------------------------------------
class GuiTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        wnd = std::make_unique<guifw::MainWindow>();
        wnd->show();
        QCoreApplication::processEvents();
        buildRibbon();
        buildDock();
    }

    void TearDown() override
    {
        wnd.reset();
        QCoreApplication::processEvents();
    }

    // ---------- 功能区成员 ----------
    guifw::RibbonTab*    tabHome   = nullptr;
    guifw::RibbonGroup*  groupClip = nullptr;
    guifw::RibbonGroup*  groupView = nullptr;
    guifw::RibbonButton* btnPaste  = nullptr;
    guifw::RibbonButton* btnCut    = nullptr;
    guifw::RibbonButton* btnInsert = nullptr;
    guifw::RibbonButton* btnStyle  = nullptr;
    guifw::RibbonButton* btnBig    = nullptr;
    guifw::RibbonButton* btnMid    = nullptr;
    guifw::RibbonAction* miPic     = nullptr;
    guifw::RibbonAction* miTable   = nullptr;
    guifw::RibbonAction* miChart   = nullptr;

    void buildRibbon()
    {
        auto* bar = wnd->ribbonBar();

        // tab 1: Home
        tabHome = new guifw::RibbonTab();
        tabHome->setTitle(u8"Home");
        bar->addTab(tabHome);

        // 组 1: Clipboard —— 普通按钮 + 下拉按钮
        groupClip = new guifw::RibbonGroup();
        groupClip->setTitle(u8"Clipboard");
        tabHome->addGroup(groupClip);

        btnPaste = new guifw::RibbonButton();
        btnPaste->setText(u8"Paste");
        btnPaste->setIcon(guifw::Icon(u8":/img/docking_bitmaps/tab.png"));
        groupClip->addButton(btnPaste);

        btnCut = new guifw::RibbonButton();
        btnCut->setText(u8"Cut");
        groupClip->addButton(btnCut);

        btnInsert = new guifw::RibbonButton();
        btnInsert->setText(u8"Insert");

        miPic = new guifw::RibbonAction();
        miPic->setText(u8"Picture");
        miTable = new guifw::RibbonAction();
        miTable->setText(u8"Table");
        miChart = new guifw::RibbonAction();
        miChart->setText(u8"Chart");
        miChart->setIcon(guifw::Icon(u8":/img/docking_bitmaps/tab.png"));

        btnInsert->addDropDownItem(miPic);
        btnInsert->addDropDownItem(miTable);
        btnInsert->addDropDownItem(miChart);
        groupClip->addButton(btnInsert);

        // 组 2: View —— 样式/尺寸/事件测试按钮
        groupView = new guifw::RibbonGroup();
        groupView->setTitle(u8"View");
        tabHome->addGroup(groupView);

        btnStyle = new guifw::RibbonButton();
        btnStyle->setText(u8"Style");
        btnStyle->setIcon(guifw::Icon(u8":/img/docking_bitmaps/tab.png"));
        btnStyle->setStyle(guifw::RibbonButtonStyle::TextBesideIcon);
        btnStyle->setIconRightText(true);
        btnStyle->setTooltip(u8"style tooltip");
        btnStyle->setCheckable(true);
        btnStyle->setChecked(true);
        groupView->addButton(btnStyle);

        QPixmap bigPm(64, 64);
        bigPm.fill(QColor(42, 130, 218));
        btnBig = new guifw::RibbonButton();
        btnBig->setText(u8"Big");
        btnBig->setIcon(guifw::Icon(QIcon(bigPm)));
        btnBig->setButtonSize(guifw::RibbonItemSize::Large);
        btnBig->setLargeIconSize(guifw::Size(64, 64));
        groupView->addButton(btnBig);

        btnMid = new guifw::RibbonButton();
        btnMid->setText(u8"Mid");
        btnMid->setIcon(guifw::Icon(u8":/img/docking_bitmaps/tab.png"));
        btnMid->setButtonSize(guifw::RibbonItemSize::Medium);
        groupView->addButton(btnMid);

        // tab 2: Tools
        auto* tab2 = new guifw::RibbonTab();
        tab2->setTitle(u8"Tools");
        bar->addTab(tab2);

        auto* groupOpt = new guifw::RibbonGroup();
        groupOpt->setTitle(u8"Options");
        tab2->addGroup(groupOpt);

        auto* btn4 = new guifw::RibbonButton();
        btn4->setText(u8"Settings");
        groupOpt->addButton(btn4);
    }

    // ---------- 停靠面板成员 ----------
    guifw::DockPanel* panelLeft    = nullptr;
    guifw::DockPanel* panelRight   = nullptr;
    guifw::DockPanel* panelBottom  = nullptr;
    guifw::DockPanel* panelTop     = nullptr;
    guifw::DockPanel* panelBottom2 = nullptr;

    void buildDock()
    {
        auto* mgr = wnd->dockPanelManager();

        panelLeft = new guifw::DockPanel();
        panelLeft->setTitle(u8"Project");
        panelLeft->setId(u8"dock_project");
        panelLeft->setFeatures(guifw::DockFeatures::None);
        mgr->addDockPanel(panelLeft, guifw::DockAreas::Left);

        panelRight = new guifw::DockPanel();
        panelRight->setTitle(u8"Properties");
        panelRight->setId(u8"dock_properties");
        panelRight->setFeatures(guifw::DockFeatures::Closable);
        mgr->addDockPanel(panelRight, guifw::DockAreas::Right);

        panelBottom = new guifw::DockPanel();
        panelBottom->setTitle(u8"Output");
        panelBottom->setId(u8"dock_output");
        panelBottom->setFeatures(guifw::DockFeatures::Closable);
        mgr->addDockPanel(panelBottom, guifw::DockAreas::Bottom);

        panelTop = new guifw::DockPanel();
        panelTop->setTitle(u8"Toolbox");
        panelTop->setId(u8"dock_toolbox");
        panelTop->setFeatures(guifw::DockFeatures::All);
        mgr->addDockPanel(panelTop, guifw::DockAreas::Top);

        panelBottom2 = new guifw::DockPanel();
        panelBottom2->setTitle(u8"Log");
        panelBottom2->setId(u8"dock_log");
        panelBottom2->setFeatures(guifw::DockFeatures::None);
        mgr->addDockPanel(panelBottom2, guifw::DockAreas::Bottom);

        QCoreApplication::processEvents();
    }

    std::unique_ptr<guifw::MainWindow> wnd;
};

// ============================ 功能区 ============================

TEST_F(GuiTest, RibbonBar_Tabs)
{
    auto* bar = wnd->ribbonBar();
    ASSERT_EQ(bar->numTabs(), 2);
    EXPECT_TRUE(bar->tabAt(0)->title() == u8"Home");
    EXPECT_TRUE(bar->tabAt(1)->title() == u8"Tools");
    EXPECT_EQ(bar->currentIndex(), 0);
}

TEST_F(GuiTest, RibbonBar_GlobalSettings)
{
    auto* bar = wnd->ribbonBar();

    // 全局风格往返
    bar->setRibbonStyle(guifw::RibbonStyle::TwoRowCompact);
    EXPECT_EQ(bar->ribbonStyle(), guifw::RibbonStyle::TwoRowCompact);
    bar->setRibbonStyle(guifw::RibbonStyle::ThreeRowLoose);
    EXPECT_EQ(bar->ribbonStyle(), guifw::RibbonStyle::ThreeRowLoose);

    // 折叠模式
    bar->setMinimumMode(true);
    EXPECT_TRUE(bar->minimumMode());
    bar->setMinimumMode(false);
    EXPECT_FALSE(bar->minimumMode());

    // 面板标题
    bar->setPanelTitleVisible(false);
    EXPECT_FALSE(bar->panelTitleVisible());
    bar->setPanelTitleVisible(true);
    EXPECT_TRUE(bar->panelTitleVisible());

    // 全局批量换行 / 图标右侧
    bar->setWordWrap(true);
    EXPECT_TRUE(bar->wordWrap());
    bar->setIconRightText(true);
    EXPECT_TRUE(bar->iconRightText());
}

TEST_F(GuiTest, RibbonBar_ApplicationButton)
{
    auto* bar = wnd->ribbonBar();

    // 可见性往返
    bar->setApplicationButtonVisible(false);
    EXPECT_FALSE(bar->applicationButtonVisible());
    bar->setApplicationButtonVisible(true);
    EXPECT_TRUE(bar->applicationButtonVisible());

    // 图标 / 文字
    bar->setApplicationIcon(guifw::Icon(u8":/img/docking_bitmaps/tab.png"));
    EXPECT_FALSE(bar->applicationIcon().isNull());
    bar->setApplicationText(u8"File");
    EXPECT_TRUE(bar->applicationText() == u8"File");
}

TEST_F(GuiTest, RibbonBar_QuickAccess)
{
    auto* bar = wnd->ribbonBar();

    auto* act = new guifw::RibbonAction();
    act->setText(u8"Save");
    bar->addQuickAccessItem(act);
    bar->addQuickAccessSeparator();

    // 快捷栏里应能找到 Save 动作与一条分隔线（经 Qt API 定位 QToolBar 子控件）
    auto* root = bar->impl<QWidget>();
    ASSERT_NE(root, nullptr);
    bool foundSave = false, foundSep = false;
    for (QToolBar* tb : root->findChildren<QToolBar*>()) {
        for (QAction* a : tb->actions()) {
            if (a->text() == QStringLiteral("Save")) {
                foundSave = true;
            }
            if (a->isSeparator()) {
                foundSep = true;
            }
        }
    }
    EXPECT_TRUE(foundSave);
    EXPECT_TRUE(foundSep);

    // 可见性往返
    bar->setQuickAccessVisible(false);
    EXPECT_FALSE(bar->quickAccessVisible());
    bar->setQuickAccessVisible(true);
    EXPECT_TRUE(bar->quickAccessVisible());
}

TEST_F(GuiTest, RibbonGroups_InCategory)
{
    // 注意：这里不直接链接 tp::SARibbon，因此不能用 SARibbonCategory::panelCount()
    // /panelByName()（内部 qobject_cast 在跨 DLL 边界会因静态库双份元对象而失败）。
    // 改用纯 Qt API（QLayout/QWidget/QLabel）断言分组的存在与标题。
    auto* cat = tabHome->impl<SARibbonCategory>();
    ASSERT_NE(cat, nullptr);
    auto* lay = cat->layout();
    ASSERT_NE(lay, nullptr);

    // Home 页有两个组
    ASSERT_EQ(lay->count(), 2);

    QStringList titles;
    for (int i = 0; i < lay->count(); ++i) {
        if (auto* w = lay->itemAt(i)->widget()) {
            if (auto* label = w->findChild<QLabel*>()) {
                titles << label->text();
            }
        }
    }
    EXPECT_TRUE(titles.contains(QStringLiteral("Clipboard")));
    EXPECT_TRUE(titles.contains(QStringLiteral("View")));
    EXPECT_EQ(titles.size(), 2);
}

TEST_F(GuiTest, RibbonTab_Groups)
{
    EXPECT_EQ(tabHome->numGroups(), 2);
    ASSERT_NE(tabHome->groupAt(0), nullptr);
    EXPECT_TRUE(tabHome->groupAt(0)->title() == u8"Clipboard");
    EXPECT_TRUE(tabHome->groupAt(1)->title() == u8"View");
    EXPECT_EQ(tabHome->groupAt(99), nullptr);
}

TEST_F(GuiTest, RibbonTab_PanelSettings)
{
    // 该页所有面板的布局模式往返
    tabHome->setPanelLayoutMode(guifw::RibbonPanelLayoutMode::TwoRow);
    EXPECT_EQ(tabHome->panelLayoutMode(), guifw::RibbonPanelLayoutMode::TwoRow);
    tabHome->setPanelLayoutMode(guifw::RibbonPanelLayoutMode::ThreeRow);
    EXPECT_EQ(tabHome->panelLayoutMode(), guifw::RibbonPanelLayoutMode::ThreeRow);

    // 面板标题 / 间距
    tabHome->setPanelTitleVisible(false);
    EXPECT_FALSE(tabHome->panelTitleVisible());
    tabHome->setPanelTitleVisible(true);
    EXPECT_TRUE(tabHome->panelTitleVisible());

    tabHome->setPanelSpacing(4);
    EXPECT_EQ(tabHome->panelSpacing(), 4);
}

TEST_F(GuiTest, RibbonButton_Properties)
{
    EXPECT_TRUE(btnPaste->text() == u8"Paste");
    EXPECT_FALSE(btnPaste->icon().isNull());
    EXPECT_EQ(btnPaste->buttonSize(), guifw::RibbonItemSize::Small);
    EXPECT_TRUE(btnPaste->enabled());
    EXPECT_FALSE(btnPaste->checkable());
    EXPECT_FALSE(btnPaste->checked());
    EXPECT_TRUE(btnPaste->tooltip().empty());
}

TEST_F(GuiTest, RibbonButton_StyleAndState)
{
    EXPECT_EQ(btnStyle->buttonSize(), guifw::RibbonItemSize::Small);
    EXPECT_EQ(btnStyle->style(), guifw::RibbonButtonStyle::TextBesideIcon);
    EXPECT_TRUE(btnStyle->iconRightText());
    EXPECT_TRUE(btnStyle->tooltip() == u8"style tooltip");
    EXPECT_TRUE(btnStyle->checkable());
    EXPECT_TRUE(btnStyle->checked());
    EXPECT_TRUE(btnStyle->enabled());
}

TEST_F(GuiTest, RibbonButton_ButtonSizes)
{
    EXPECT_EQ(btnBig->buttonSize(), guifw::RibbonItemSize::Large);
    EXPECT_EQ(btnMid->buttonSize(), guifw::RibbonItemSize::Medium);
    EXPECT_EQ(btnPaste->buttonSize(), guifw::RibbonItemSize::Small);
    EXPECT_FALSE(btnBig->icon().isNull());
    // largeIconSize 可往返
    auto ls = btnBig->largeIconSize();
    EXPECT_EQ(ls.x, 64);
    EXPECT_EQ(ls.y, 64);
}

TEST_F(GuiTest, RibbonButton_DropDown)
{
    EXPECT_EQ(btnInsert->dropDownItemCount(), 3u);
    EXPECT_TRUE(btnInsert->dropDownItemAt(0)->text() == u8"Picture");
    EXPECT_TRUE(btnInsert->dropDownItemAt(1)->text() == u8"Table");
    EXPECT_TRUE(btnInsert->dropDownItemAt(2)->text() == u8"Chart");
    EXPECT_FALSE(btnInsert->dropDownItemAt(2)->icon().isNull());
    EXPECT_EQ(btnInsert->dropDownItemAt(99), nullptr);

    // 加入下拉项后按钮应挂上 QMenu，且包含全部项对应的 QAction
    auto* tb = btnInsert->impl<SARibbonToolButton>();
    ASSERT_NE(tb, nullptr);
    auto* menu = tb->menu();
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->actions().size(), 3);
}

TEST_F(GuiTest, RibbonButton_DropDownSeparator)
{
    // 分隔线不计入下拉项
    size_t before = btnInsert->dropDownItemCount();
    btnInsert->addSeparator();
    EXPECT_EQ(btnInsert->dropDownItemCount(), before);

    auto* tb = btnInsert->impl<SARibbonToolButton>();
    ASSERT_NE(tb, nullptr);
    auto* menu = tb->menu();
    ASSERT_NE(menu, nullptr);
    EXPECT_EQ(menu->actions().size(), static_cast<int>(before) + 1);
    EXPECT_TRUE(menu->actions().last()->isSeparator());

    // 移除一项后重建，顺序与分隔线仍正确
    btnInsert->removeDropDownItem(miTable);
    EXPECT_EQ(btnInsert->dropDownItemCount(), before - 1);
    EXPECT_EQ(menu->actions().size(), static_cast<int>(before)); // 2 项 + 1 分隔线

    // 清理后可重新加入，分隔线被释放、菜单只剩真实项
    btnInsert->clearDropDownItems();
    EXPECT_EQ(btnInsert->dropDownItemCount(), 0u);
    btnInsert->addDropDownItem(miPic);
    btnInsert->addDropDownItem(miTable);
    btnInsert->addDropDownItem(miChart);
    EXPECT_EQ(btnInsert->dropDownItemCount(), 3u);
    EXPECT_EQ(menu->actions().size(), 3);
    EXPECT_FALSE(menu->actions().last()->isSeparator());

    // ---- 全条目索引接口（项与分隔线统一计数）----
    btnInsert->clearDropDownItems();
    btnInsert->addDropDownItem(miPic);   // entry 0
    btnInsert->addSeparator();           // entry 1
    btnInsert->addDropDownItem(miTable); // entry 2
    btnInsert->addDropDownItem(miChart); // entry 3
    EXPECT_EQ(btnInsert->dropDownEntryCount(), 4u);
    EXPECT_EQ(btnInsert->dropDownItemCount(), 3u);

    // 按全条目索引移除分隔线（entry 1）
    btnInsert->removeDropDownEntryAt(1);
    EXPECT_EQ(btnInsert->dropDownEntryCount(), 3u);
    EXPECT_EQ(btnInsert->dropDownItemCount(), 3u);
    EXPECT_EQ(menu->actions().size(), 3);
    EXPECT_FALSE(menu->actions().at(1)->isSeparator());

    // 移除真实项（entry 0 = miPic）
    btnInsert->removeDropDownEntryAt(0);
    EXPECT_EQ(btnInsert->dropDownEntryCount(), 2u);
    EXPECT_EQ(btnInsert->dropDownItemCount(), 2u);

    // 越界安全
    btnInsert->removeDropDownEntryAt(99);
    EXPECT_EQ(btnInsert->dropDownEntryCount(), 2u);
}

TEST_F(GuiTest, RibbonButton_ClickedEvent)
{
    int  clicks = 0;
    auto id     = btnStyle->clicked.addHandler([&clicks](guifw::RibbonButton&, vine::EventArgs&) { ++clicks; });

    auto* tb = btnStyle->impl<SARibbonToolButton>();
    ASSERT_NE(tb, nullptr);
    tb->click();
    EXPECT_EQ(clicks, 1);
    tb->click();
    tb->click();
    EXPECT_EQ(clicks, 3);

    btnStyle->clicked.removeHandler(id);
    tb->click();
    EXPECT_EQ(clicks, 3); // 移除后不再触发
}

TEST_F(GuiTest, RibbonGroup_Methods)
{
    // ---- 布局模式（默认值取决于 SARibbon 全局风格，显式设置后再读回）----
    groupClip->setLayoutMode(guifw::RibbonPanelLayoutMode::TwoRow);
    EXPECT_EQ(groupClip->layoutMode(), guifw::RibbonPanelLayoutMode::TwoRow);
    groupClip->setLayoutMode(guifw::RibbonPanelLayoutMode::ThreeRow);
    EXPECT_EQ(groupClip->layoutMode(), guifw::RibbonPanelLayoutMode::ThreeRow);

    // ---- 扩展 / 自定义 ----
    groupClip->setExpanding(true);
    EXPECT_TRUE(groupClip->expanding());
    groupClip->setExpanding(false);

    groupClip->setCanCustomize(false);
    EXPECT_FALSE(groupClip->canCustomize());
    groupClip->setCanCustomize(true);
    EXPECT_TRUE(groupClip->canCustomize());

    // ---- 面板级图标尺寸往返 ----
    groupClip->setLargeIconSize(guifw::Size(40, 40));
    groupClip->setSmallIconSize(guifw::Size(16, 16));
    auto ls = groupClip->largeIconSize();
    auto ss = groupClip->smallIconSize();
    EXPECT_EQ(ls.x, 40);
    EXPECT_EQ(ls.y, 40);
    EXPECT_EQ(ss.x, 16);
    EXPECT_EQ(ss.y, 16);

    // ---- 批量样式 ----
    groupClip->setIconRightText(true);
    EXPECT_TRUE(groupClip->iconRightText());
    groupClip->setWordWrap(true);
    EXPECT_TRUE(groupClip->wordWrap());

    // ---- 分隔线 ----
    auto* pnl = groupClip->impl<SARibbonPanel>();
    ASSERT_NE(pnl, nullptr);
    int before = pnl->layout()->count();
    groupClip->addSeparator();
    EXPECT_EQ(pnl->layout()->count(), before + 1);

    // ---- 选项按钮 ----
    auto* mi = new guifw::RibbonAction();
    mi->setText(u8"More");
    groupClip->setOptionAction(mi);
    EXPECT_EQ(groupClip->optionAction(), mi);
    groupClip->setOptionAction(nullptr);
    EXPECT_EQ(groupClip->optionAction(), nullptr);
}

TEST_F(GuiTest, RibbonGroup_AddWidget)
{
    // 通用控件容器：包一个 QComboBox 放进组
    auto* combo = new QComboBox();
    combo->addItem(u8"10");
    combo->addItem(u8"12");
    combo->addItem(u8"14");
    auto* we = new guifw::Control(combo);
    groupClip->addControl(we, guifw::RibbonItemSize::Medium);

    // 控件已被面板接管（父变为面板）
    ASSERT_NE(combo->parentWidget(), nullptr);

    // 移除后控件被置空父并延迟释放
    groupClip->removeControl(we);
    EXPECT_EQ(combo->parentWidget(), nullptr);
}

TEST_F(GuiTest, Control_CommonProps)
{
    // 通用控件容器暴露 QWidget 级属性（enabled/tooltip/size 等）
    auto* combo = new QComboBox();
    auto* ctrl  = new guifw::Control(combo);

    ctrl->setEnabled(false);
    EXPECT_FALSE(ctrl->enabled());
    ctrl->setEnabled(true);
    EXPECT_TRUE(ctrl->enabled());

    ctrl->setVisible(false);
    EXPECT_FALSE(ctrl->visible());

    ctrl->setTooltip(u8"hi");
    EXPECT_TRUE(ctrl->tooltip() == u8"hi");

    ctrl->setSize(guifw::Size(400, 40));
    EXPECT_EQ(ctrl->width(), 400);
    EXPECT_EQ(ctrl->height(), 40);
    EXPECT_EQ(ctrl->size().x, 400);
    EXPECT_EQ(ctrl->size().y, 40);

    delete ctrl;
}

TEST_F(GuiTest, ConfigManager_Basic)
{
    auto* cfg = new vine::appfw::ConfigManager();

    // 标量
    cfg->setString(u8"name", u8"Vine");
    cfg->setInt(u8"max", 100);
    cfg->setBool(u8"flag", true);
    cfg->setDouble(u8"ratio", 1.5);
    EXPECT_TRUE(cfg->contains(u8"name"));
    EXPECT_TRUE(cfg->getString(u8"name") == u8"Vine");
    EXPECT_EQ(cfg->getInt(u8"max"), 100);
    EXPECT_TRUE(cfg->getBool(u8"flag"));
    EXPECT_DOUBLE_EQ(cfg->getDouble(u8"ratio"), 1.5);
    EXPECT_EQ(cfg->getInt(u8"missing"), 0); // 缺省返回默认值
    EXPECT_FALSE(cfg->contains(u8"missing"));

    // 数组
    cfg->setIntArray(u8"nums", { 1, 2, 3 });
    cfg->setStringArray(u8"names", { u8"a", u8"b" });
    auto nums = cfg->getIntArray(u8"nums");
    ASSERT_EQ(nums.size(), 3u);
    EXPECT_EQ(nums[0], 1);
    EXPECT_EQ(nums[2], 3);
    auto names = cfg->getStringArray(u8"names");
    ASSERT_EQ(names.size(), 2u);
    EXPECT_TRUE(names[1] == u8"b");

    // JSON 往返（类型无损）
    auto* cfg2 = new vine::appfw::ConfigManager();
    EXPECT_TRUE(cfg2->loadJson(cfg->toJson()));
    EXPECT_TRUE(cfg2->getString(u8"name") == u8"Vine");
    EXPECT_EQ(cfg2->getInt(u8"max"), 100);
    EXPECT_TRUE(cfg2->getBool(u8"flag"));
    EXPECT_DOUBLE_EQ(cfg2->getDouble(u8"ratio"), 1.5);
    auto nums2 = cfg2->getIntArray(u8"nums");
    ASSERT_EQ(nums2.size(), 3u);
    EXPECT_EQ(nums2[2], 3);
    EXPECT_EQ(cfg2->getInt(u8"max"), 100);

    // 层级 key（A.B.C）
    cfg->setInt(u8"window.x", 100);
    cfg->setInt(u8"window.y", 200);
    cfg->setBool(u8"window.state.maximized", true);
    cfg->setString(u8"editor.font.name", u8"Consolas");
    EXPECT_TRUE(cfg->contains(u8"window.x"));
    EXPECT_EQ(cfg->getInt(u8"window.x"), 100);
    EXPECT_TRUE(cfg->getBool(u8"window.state.maximized"));
    EXPECT_TRUE(cfg->getString(u8"editor.font.name") == u8"Consolas");

    // 序列化为嵌套 JSON 对象（非点分扁平 key）
    const auto          json = cfg->toJson();
    QJsonParseError     pe;
    const QJsonDocument nested = QJsonDocument::fromJson(QByteArray(reinterpret_cast<const char*>(json.data()), static_cast<int>(json.size())), &pe);
    ASSERT_EQ(pe.error, QJsonParseError::NoError);
    const QJsonObject root = nested.object();
    EXPECT_TRUE(root.contains(QStringLiteral("window")));
    EXPECT_TRUE(root.value(QStringLiteral("window")).toObject().contains(QStringLiteral("x")));
    EXPECT_TRUE(root.value(QStringLiteral("window")).toObject().value(QStringLiteral("state")).toObject().contains(QStringLiteral("maximized")));
    EXPECT_FALSE(root.contains(QStringLiteral("window.x"))); // 不出现扁平点分 key

    // 层级 JSON 往返
    auto* cfg3 = new vine::appfw::ConfigManager();
    EXPECT_TRUE(cfg3->loadJson(json));
    EXPECT_EQ(cfg3->getInt(u8"window.x"), 100);
    EXPECT_EQ(cfg3->getInt(u8"window.y"), 200);
    EXPECT_TRUE(cfg3->getBool(u8"window.state.maximized"));
    EXPECT_TRUE(cfg3->getString(u8"editor.font.name") == u8"Consolas");
    delete cfg3;

    // 非法 JSON
    EXPECT_FALSE(cfg2->loadJson(u8"not json"));

    delete cfg;
    delete cfg2;

    // Application 持有引用
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    ASSERT_NE(app->configManager(), nullptr);
    app->configManager()->setInt(u8"appmax", 42);
    EXPECT_EQ(app->configManager()->getInt(u8"appmax"), 42);
}

// ============================ 可显示配置 ============================

// 保存必须报告真实结果：写失败不能返回 true，失败也不能破坏磁盘上已有的配置。
TEST_F(GuiTest, ConfigManager_SaveReportsFailureAndKeepsTheOldFile)
{
    using vine::appfw::ConfigManager;

    auto* cfg = new ConfigManager();
    cfg->setString(u8"name", u8"Vine");
    cfg->setInt(u8"max", 100);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vine_config_save_test";
    std::error_code             ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / "config.json";
    const vine::String          file_path(file.u8string());

    // 成功路径：返回 true、内容可读回、原子写不留临时文件
    ASSERT_TRUE(cfg->save(file_path));
    EXPECT_TRUE(std::filesystem::exists(file));
    size_t entries = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        (void)e;
        ++entries;
    }
    EXPECT_EQ(entries, 1u);

    auto* loaded = new ConfigManager();
    ASSERT_TRUE(loaded->load(file_path));
    EXPECT_TRUE(loaded->getString(u8"name") == u8"Vine");
    EXPECT_EQ(loaded->getInt(u8"max"), 100);
    delete loaded;

    // 父目录不存在 / 目标是目录：报失败，而不是无声成功
    EXPECT_FALSE(cfg->save(vine::String((dir / "missing" / "config.json").u8string())));
    EXPECT_FALSE(cfg->save(vine::String(dir.u8string())));

    // 目录不可写：报失败，且磁盘上的旧配置原样保留
    std::filesystem::permissions(dir, std::filesystem::perms::owner_write, std::filesystem::perm_options::remove, ec);
    QFile      probe(QString::fromStdString((dir / "probe.tmp").string()));
    const bool can_write = probe.open(QIODevice::WriteOnly);
    if (can_write) {
        probe.close();
        probe.remove();
    }
    if (!can_write) { // 特权进程无视权限位，那就没有可断言的了
        cfg->setInt(u8"max", 999);
        EXPECT_FALSE(cfg->save(file_path));
        auto* kept = new ConfigManager();
        ASSERT_TRUE(kept->load(file_path));
        EXPECT_EQ(kept->getInt(u8"max"), 100) << "保存失败时磁盘上的旧配置不能被破坏";
        delete kept;
    }
    std::filesystem::permissions(dir, std::filesystem::perms::owner_write, std::filesystem::perm_options::add, ec);

    std::filesystem::remove_all(dir, ec);
    delete cfg;
}

// 变更事件只在值真的改变时发出；loadJson 的整体替换同样要通知（空 key）。
TEST_F(GuiTest, ConfigManager_NotifiesOnlyOnRealChanges)
{
    using vine::appfw::ConfigChangedEventArgs;
    using vine::appfw::ConfigManager;

    auto*     cfg = new ConfigManager();
    int       events    = 0;
    bool      saw_empty = false;
    vine::String last_key;
    cfg->changed.addHandler([&](ConfigManager&, ConfigChangedEventArgs& e) {
        ++events;
        last_key  = e.key();
        saw_empty = e.key().empty();
    });

    // 同值写入 = 无变化 = 无事件：处理函数里回写同值不会自激
    cfg->setInt(u8"max", 100);
    EXPECT_EQ(events, 1);
    cfg->setInt(u8"max", 100);
    EXPECT_EQ(events, 1);
    cfg->setInt(u8"max", 101);
    EXPECT_EQ(events, 2);
    EXPECT_TRUE(last_key == u8"max");

    cfg->setIntArray(u8"nums", { 1, 2 });
    EXPECT_EQ(events, 3);
    cfg->setIntArray(u8"nums", { 1, 2 });
    EXPECT_EQ(events, 3);
    cfg->setString(u8"name", u8"Vine");
    cfg->setString(u8"name", u8"Vine");
    EXPECT_EQ(events, 4);

    // 删除不存在的键不算变化
    cfg->remove(u8"missing");
    EXPECT_EQ(events, 4);
    cfg->remove(u8"max");
    EXPECT_EQ(events, 5);

    // loadJson：内容变了 → 一次事件（空 key = 整个配置变了）；内容相同 → 无事件
    auto*     other       = new ConfigManager();
    other->setInt(u8"a", 1);
    const int before_load = events;
    ASSERT_TRUE(cfg->loadJson(other->toJson()));
    EXPECT_EQ(events, before_load + 1);
    EXPECT_TRUE(saw_empty);
    const int before_same = events;
    ASSERT_TRUE(cfg->loadJson(other->toJson()));
    EXPECT_EQ(events, before_same);

    // clear：有值才通知，且用空 key 表示整体变化
    saw_empty = false;
    const int before_clear = events;
    cfg->clear();
    EXPECT_EQ(events, before_clear + 1);
    EXPECT_TRUE(saw_empty);
    const int before_empty_clear = events;
    cfg->clear();
    EXPECT_EQ(events, before_empty_clear);

    delete other;
    delete cfg;
}

// JSON 往返：每种类型都无损读回；超范围整数被夹取而不是被静默改写。
TEST_F(GuiTest, ConfigManager_JsonRoundTripKeepsTypes)
{
    using vine::appfw::ConfigManager;

    auto* a = new ConfigManager();
    a->setString(u8"name", u8"Vine");
    a->setBool(u8"flag", true);
    a->setInt(u8"max", 2147483647);
    a->setDouble(u8"ratio", 1.5);
    a->setIntArray(u8"nums", { 1, -2, 3 });
    a->setStringArray(u8"names", { u8"a", u8"b" });

    auto* b = new ConfigManager();
    ASSERT_TRUE(b->loadJson(a->toJson()));
    EXPECT_TRUE(b->getString(u8"name") == u8"Vine");
    EXPECT_TRUE(b->getBool(u8"flag"));
    EXPECT_EQ(b->getInt(u8"max"), 2147483647);
    EXPECT_DOUBLE_EQ(b->getDouble(u8"ratio"), 1.5);
    const auto nums = b->getIntArray(u8"nums");
    ASSERT_EQ(nums.size(), 3u);
    EXPECT_EQ(nums[1], -2);
    EXPECT_EQ(b->getStringArray(u8"names").size(), 2u);

    // 旧格式（整数写成 double）仍读得回来
    ASSERT_TRUE(b->loadJson(u8R"({"legacy":{"type":"int","value":100.0}})"));
    EXPECT_EQ(b->getInt(u8"legacy"), 100);

    // 超出 int 范围：夹取（并记日志），而不是静默改成一个别的值
    ASSERT_TRUE(b->loadJson(u8R"({"big":{"type":"int","value":9999999999}})"));
    EXPECT_EQ(b->getInt(u8"big"), 2147483647);

    delete a;
    delete b;
}

// 不符合格式的条目被挑出来忽略，其余照常生效；文本不是 JSON 才算失败。
TEST_F(GuiTest, ConfigManager_BadEntriesAreIgnoredNotFatal)
{
    using vine::appfw::ConfigManager;

    auto* cfg = new ConfigManager();
    ASSERT_TRUE(cfg->loadJson(u8R"({
        "plain": 42,
        "text": "hello",
        "unknown": { "type": "text", "value": "x" },
        "good": { "type": "int", "value": 7 }
    })"));
    EXPECT_EQ(cfg->getInt(u8"good"), 7);
    EXPECT_FALSE(cfg->contains(u8"plain"));
    EXPECT_FALSE(cfg->contains(u8"text"));
    EXPECT_FALSE(cfg->contains(u8"unknown"));

    EXPECT_FALSE(cfg->loadJson(u8"not json"));
    delete cfg;
}

// 所有权跟着树走：项被删掉（无论走哪条删除路径）之后，它的 owner 不能残留，
// 否则别的插件卸载时会连它一起删。
TEST_F(GuiTest, ConfigRegistry_OwnershipDoesNotOutliveTheItem)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::StandardCategory;
    using vine::appfw::StandardGroup;

    ConfigRegistry reg;
    ASSERT_TRUE(reg.addItem(StandardCategory::General, StandardGroup::Behavior, ConfigItem(u8"p.first", u8"1", ConfigItemType::Int), u8"plugin_a"));
    ASSERT_TRUE(reg.addItem(StandardCategory::General, StandardGroup::Behavior, ConfigItem(u8"p.second", u8"2", ConfigItemType::Int), u8"plugin_a"));
    EXPECT_EQ(reg.itemsForPlugin(u8"plugin_a").size(), 2u);

    // 删单项：所有权同步消失
    EXPECT_TRUE(reg.removeItem(u8"p.first"));
    EXPECT_EQ(reg.itemsForPlugin(u8"plugin_a").size(), 1u);

    // 删分类：其下所有项的所有权一起消失
    EXPECT_TRUE(reg.removeCategory(u8"general"));
    EXPECT_TRUE(reg.itemsForPlugin(u8"plugin_a").empty());

    // 同一个 key 重新注册且不再声明 owner：旧的 owner 不能残留
    ASSERT_TRUE(reg.addItem(StandardCategory::General, StandardGroup::Behavior, ConfigItem(u8"p.second", u8"2", ConfigItemType::Int)));
    EXPECT_TRUE(reg.itemsForPlugin(u8"plugin_a").empty());
    EXPECT_NE(reg.item(u8"p.second"), nullptr);
}

// 编辑器按自己的 key 取值：运行期注册的新项（哪怕是排在前面的新分组）不能把值
// 塞进别的编辑器。
TEST(ConfigWindowTest, RefreshKeepsEachValueWithItsKey)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigManager;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::gui::ConfigWindow;

    ConfigRegistry reg;
    auto*          cat = reg.addCategory(u8"C1");
    auto*          g   = cat->addGroup(u8"G");
    ASSERT_TRUE(g->addItem(ConfigItem(u8"a", u8"A", ConfigItemType::String)));

    auto* cfg = new ConfigManager();
    cfg->setString(u8"a", u8"AAA");

    auto* win  = new ConfigWindow(&reg, cfg);
    auto* root = win->impl<QDialog>();
    ASSERT_NE(root, nullptr);
    auto editors = root->findChildren<QLineEdit*>();
    ASSERT_EQ(editors.size(), 1);
    EXPECT_EQ(editors.value(0)->text(), QStringLiteral("AAA"));

    // 运行期注册：新分组的 order=-1，排到已有分组之前，遍历顺序被改变
    auto* g0 = cat->getOrAddGroup(u8"G0");
    g0->order(-1);
    ASSERT_TRUE(g0->addItem(ConfigItem(u8"z", u8"Z", ConfigItemType::String)));
    cfg->setString(u8"z", u8"ZZZ");

    win->refresh();
    EXPECT_EQ(editors.value(0)->text(), QStringLiteral("AAA")) << "每个编辑器只能显示自己 key 的值";
    EXPECT_EQ(root->findChildren<QLineEdit*>().size(), 1) << "窗口构建后注册的项不会凭空多出编辑器";

    delete win;
    delete cfg;
}

// Choice 编辑器：显示存储值（键没写过时显示 item 默认值）；不在选项里就显示为
// 未选中，而不是谎报第一个选项。
TEST(ConfigWindowTest, ChoiceWithoutMatchShowsNoSelection)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigManager;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::gui::ConfigWindow;

    ConfigRegistry reg;
    auto*          cat = reg.addCategory(u8"C1");
    auto*          g   = cat->addGroup(u8"G");
    ConfigItem     theme(u8"theme", u8"主题", ConfigItemType::Choice);
    theme.choices({ { 1, u8"深色" }, { 2, u8"浅色" } });
    theme.defaultValue(2);
    ASSERT_TRUE(g->addItem(theme));

    auto* cfg   = new ConfigManager();
    auto* win   = new ConfigWindow(&reg, cfg);
    auto* combo = win->impl<QDialog>()->findChildren<QComboBox*>().value(0);
    ASSERT_NE(combo, nullptr);

    // 键从未写过：显示 item 默认值对应的选项（reset() 会恢复的那个）
    EXPECT_EQ(combo->currentIndex(), 1);

    cfg->setInt(u8"theme", 1);
    win->refresh();
    EXPECT_EQ(combo->currentIndex(), 0);

    // 存储值不在选项里：未选中
    cfg->setInt(u8"theme", 9);
    win->refresh();
    EXPECT_EQ(combo->currentIndex(), -1);

    delete win;
    delete cfg;
}

// 没有声明 range 的数值项：编辑器不能把它要显示的值夹到某个任意区间里。
TEST(ConfigWindowTest, UnboundedNumbersAreNotClamped)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigManager;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::gui::ConfigWindow;

    ConfigRegistry reg;
    auto*          cat = reg.addCategory(u8"C1");
    auto*          g   = cat->addGroup(u8"G");
    ASSERT_TRUE(g->addItem(ConfigItem(u8"offset", u8"偏移", ConfigItemType::Int)));
    ASSERT_TRUE(g->addItem(ConfigItem(u8"bounded", u8"上限", ConfigItemType::Int).range(0, 10)));
    ASSERT_TRUE(g->addItem(ConfigItem(u8"scale", u8"比例", ConfigItemType::Double)));

    auto* cfg = new ConfigManager();
    cfg->setInt(u8"offset", -7);
    cfg->setInt(u8"bounded", 5);
    cfg->setDouble(u8"scale", -2.5);

    auto* win   = new ConfigWindow(&reg, cfg);
    auto* root  = win->impl<QDialog>();
    auto  spins = root->findChildren<QSpinBox*>();
    auto  dbls  = root->findChildren<QDoubleSpinBox*>();
    ASSERT_EQ(spins.size(), 2);
    ASSERT_EQ(dbls.size(), 1);

    // 没有声明 range 的项：不夹取
    EXPECT_EQ(spins.value(0)->value(), -7) << "没有声明 range 时不能把负数夹成 0";
    EXPECT_DOUBLE_EQ(dbls.value(0)->value(), -2.5);

    // 声明了 range 的项：仍按 range 约束
    EXPECT_EQ(spins.value(1)->maximum(), 10);
    EXPECT_EQ(spins.value(1)->value(), 5);

    delete win;
    delete cfg;
}

TEST_F(GuiTest, ConfigItem_Descriptor)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;

    ConfigItem item(u8"editor.font.size", u8"字号", ConfigItemType::Int);
    item.description(u8"编辑器字号").range(8, 72).defaultValue(14).step(2);
    EXPECT_TRUE(item.key() == u8"editor.font.size");
    EXPECT_TRUE(item.label() == u8"字号");
    EXPECT_TRUE(item.description() == u8"编辑器字号");
    EXPECT_EQ(item.type(), ConfigItemType::Int);
    EXPECT_TRUE(item.hasRange());
    EXPECT_EQ(item.minInt(), 8);
    EXPECT_EQ(item.maxInt(), 72);
    EXPECT_EQ(item.step(), 2.0);
    EXPECT_TRUE(item.hasDefault());
    EXPECT_EQ(item.defaultInt(), 14);

    // 默认值重载：char8_t* 必须走 String 而非 bool
    ConfigItem s(u8"name", u8"名称", ConfigItemType::String);
    s.defaultValue(u8"Vine");
    EXPECT_TRUE(s.hasDefault());
    EXPECT_TRUE(s.defaultString() == u8"Vine");

    ConfigItem c(u8"theme", u8"主题", ConfigItemType::Choice);
    c.choices({ u8"浅色", u8"深色" });
    EXPECT_EQ(c.choices().size(), 2u);
    EXPECT_TRUE(c.choices()[1].description == u8"深色");
    EXPECT_FALSE(c.hasDefault());
}

TEST_F(GuiTest, ConfigItem_DefaultTypeCheck)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;

    // No default set -> hasDefault() false; wrong-type getter throws.
    ConfigItem none(u8"k", u8"K", ConfigItemType::Int);
    EXPECT_FALSE(none.hasDefault());
    EXPECT_THROW(none.defaultInt(), std::bad_any_cast);

    // Correct type accepted.
    ConfigItem i(u8"size", u8"大小", ConfigItemType::Int);
    i.defaultValue(10);
    EXPECT_TRUE(i.hasDefault());
    EXPECT_EQ(i.defaultInt(), 10);

    // String default is valid for Choice too.
    ConfigItem c(u8"theme", u8"主题", ConfigItemType::Choice);
    c.defaultValue(u8"浅色");
    EXPECT_TRUE(c.hasDefault());
    EXPECT_TRUE(c.defaultString() == u8"浅色");

    // Wrong-type setter throws std::invalid_argument.
    ConfigItem b(u8"flag", u8"开关", ConfigItemType::Bool);
    EXPECT_THROW(b.defaultValue(3), std::invalid_argument);
    EXPECT_THROW(ConfigItem(u8"x", u8"X", ConfigItemType::Double).defaultValue(true), std::invalid_argument);
}

TEST_F(GuiTest, ConfigItem_RangeAny)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;

    // No range -> hasRange() false; min/max getters throw; step() falls back to 1.0.
    ConfigItem none(u8"k", u8"K", ConfigItemType::Int);
    EXPECT_FALSE(none.hasRange());
    EXPECT_THROW(none.minInt(), std::bad_any_cast);
    EXPECT_EQ(none.step(), 1.0);

    // Int range with default step, then override.
    ConfigItem i(u8"size", u8"大小", ConfigItemType::Int);
    i.range(8, 72);
    EXPECT_TRUE(i.hasRange());
    EXPECT_EQ(i.minInt(), 8);
    EXPECT_EQ(i.maxInt(), 72);
    EXPECT_EQ(i.step(), 1.0);
    i.step(2);
    EXPECT_EQ(i.step(), 2.0);

    // Double range.
    ConfigItem d(u8"ratio", u8"比例", ConfigItemType::Double);
    d.range(0.0, 10.0);
    EXPECT_DOUBLE_EQ(d.minDouble(), 0.0);
    EXPECT_DOUBLE_EQ(d.maxDouble(), 10.0);

    // Wrong usage throws.
    ConfigItem s(u8"name", u8"名称", ConfigItemType::String);
    EXPECT_THROW(s.range(1, 2), std::invalid_argument);
    EXPECT_THROW(ConfigItem(u8"x", u8"X", ConfigItemType::Int).step(0.5), std::invalid_argument);
}

TEST_F(GuiTest, ConfigItem_TypedChoices)
{
    using vine::String;
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;

    // Int-valued choices with descriptions.
    ConfigItem i(u8"theme", u8"主题", ConfigItemType::Choice);
    i.choices({
                { 0, u8"浅色"     },
                { 1, u8"深色"     },
                { 2, u8"跟随系统" }
    })
        .defaultValue(1);
    ASSERT_EQ(i.choices().size(), 3u);
    EXPECT_TRUE(i.choices()[0].description == u8"浅色");
    EXPECT_EQ(*std::any_cast<int>(&i.choices()[1].value), 1);
    EXPECT_TRUE(i.hasDefault());
    EXPECT_EQ(i.defaultType(), ConfigItemType::Int);
    EXPECT_EQ(i.defaultInt(), 1);

    // Double-valued choices.
    ConfigItem d(u8"size", u8"大小", ConfigItemType::Choice);
    d.choices({
      { 12.0, u8"小" },
      { 14.0, u8"中" }
    });
    ASSERT_EQ(d.choices().size(), 2u);
    EXPECT_EQ(*std::any_cast<double>(&d.choices()[1].value), 14.0);

    // String-valued choices (value + description).
    ConfigItem s(u8"enc", u8"编码", ConfigItemType::Choice);
    s.choices({
      { u8"utf8", u8"UTF-8" },
      { u8"gbk",  u8"GBK"   }
    });
    EXPECT_TRUE(*std::any_cast<String>(&s.choices()[0].value) == u8"utf8");
    EXPECT_TRUE(s.choices()[0].description == u8"UTF-8");
}

TEST_F(GuiTest, ConfigRegistry_Register)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigRegistry;

    ConfigRegistry reg;
    auto*          cat1 = reg.addCategory(u8"C1");
    ASSERT_NE(cat1, nullptr);
    auto* g1 = cat1->addGroup(u8"G1");
    ASSERT_NE(g1, nullptr);
    EXPECT_TRUE(g1->addItem(ConfigItem(u8"a", u8"A", ConfigItemType::String)));
    EXPECT_TRUE(g1->addItem(ConfigItem(u8"c", u8"C", ConfigItemType::Int)));
    auto* g2 = cat1->addGroup(u8"G2");
    ASSERT_NE(g2, nullptr);
    EXPECT_TRUE(g2->addItem(ConfigItem(u8"b", u8"B", ConfigItemType::Bool)));

    // Duplicate name rejected
    EXPECT_EQ(cat1->addGroup(u8"G1"), nullptr);
    EXPECT_EQ(reg.addCategory(u8"C1"), nullptr);

    // Duplicate key rejected (whole tree)
    EXPECT_FALSE(g2->addItem(ConfigItem(u8"a", u8"A2", ConfigItemType::String)));

    EXPECT_EQ(reg.itemCount(), 3);

    // Whole-tree lookup
    EXPECT_TRUE(reg.item(u8"b")->label() == u8"B");
    EXPECT_EQ(reg.item(u8"nope"), nullptr);
    EXPECT_TRUE(reg.item(u8"c")->key() == u8"c");
    // In-group lookup
    EXPECT_TRUE(g1->item(u8"a") != nullptr);
    EXPECT_EQ(g1->item(u8"b"), nullptr);

    // Category/group order preserved
    ASSERT_EQ(reg.categories().size(), 1u);
    auto groups = cat1->groups();
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_TRUE(groups[0]->name() == u8"G1");
    EXPECT_TRUE(groups[1]->name() == u8"G2");

    // Removal / clear
    EXPECT_TRUE(reg.removeItem(u8"b"));
    EXPECT_FALSE(reg.removeItem(u8"b"));
    EXPECT_EQ(reg.itemCount(), 2);
    EXPECT_EQ(reg.item(u8"b"), nullptr);
    EXPECT_TRUE(cat1->removeGroup(u8"G2"));
    EXPECT_FALSE(cat1->removeGroup(u8"G2"));
    EXPECT_TRUE(reg.removeCategory(u8"C1"));
    EXPECT_FALSE(reg.removeCategory(u8"C1"));
    EXPECT_EQ(reg.itemCount(), 0);
    reg.clear();
    EXPECT_EQ(reg.categories().size(), 0u);
}

TEST_F(GuiTest, ConfigRegistry_MetaAndOrder)
{
    using vine::appfw::ConfigRegistry;

    ConfigRegistry reg;
    auto*          a = reg.addCategory(u8"a");
    ASSERT_NE(a, nullptr);
    a->label(u8"AA").description(u8"desc").order(3);
    EXPECT_TRUE(a->label() == u8"AA");
    EXPECT_TRUE(a->description() == u8"desc");
    EXPECT_EQ(a->order(), 3);

    auto* g = a->addGroup(u8"g");
    ASSERT_NE(g, nullptr);
    g->label(u8"GG").order(1);
    EXPECT_TRUE(g->label() == u8"GG");
    EXPECT_EQ(g->order(), 1);

    // Display order: smaller order first (ties keep insertion order)
    auto* b = reg.addCategory(u8"b");
    ASSERT_NE(b, nullptr);
    b->order(0);
    auto cats = reg.categories();
    ASSERT_EQ(cats.size(), 2u);
    EXPECT_TRUE(cats[0]->name() == u8"b");
    EXPECT_TRUE(cats[1]->name() == u8"a");
}

TEST_F(GuiTest, ConfigManager_ChangedEvent)
{
    auto*        cfg   = new vine::appfw::ConfigManager();
    int          fired = 0;
    vine::String lastKey;
    auto         id = cfg->changed.addHandler([&](vine::appfw::ConfigManager&, vine::appfw::ConfigChangedEventArgs& args) {
        ++fired;
        lastKey = args.key();
    });

    cfg->setInt(u8"window.x", 1);
    cfg->setBool(u8"flag", true);
    cfg->setString(u8"name", u8"a");
    cfg->setDouble(u8"ratio", 0.5);
    cfg->setIntArray(u8"nums", { 1, 2 });
    EXPECT_EQ(fired, 5);
    EXPECT_TRUE(lastKey == u8"nums");

    cfg->remove(u8"window.x");
    EXPECT_EQ(fired, 6);
    EXPECT_TRUE(lastKey == u8"window.x");

    cfg->changed.removeHandler(id);
    cfg->setInt(u8"after", 9);
    EXPECT_EQ(fired, 6); // 已移除 handler

    delete cfg;
}

TEST_F(GuiTest, PluginLoadContext_Configs)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    vine::appfw::PluginLoadContext ctx(app);
    EXPECT_EQ(ctx.application(), app);
    ASSERT_NE(ctx.configRegistry(), nullptr);
    EXPECT_EQ(ctx.configRegistry(), app->configRegistry());
    EXPECT_NE(ctx.eventBus(), nullptr);
    EXPECT_EQ(ctx.eventBus(), app->eventBus());

    // Registering through the context targets the same registry as Application
    auto* pluginCat = ctx.configRegistry()->addCategory(u8"插件");
    ASSERT_NE(pluginCat, nullptr);
    pluginCat->addGroup(u8"常规")->addItem(vine::appfw::ConfigItem(u8"plugin.opt", u8"插件选项", vine::appfw::ConfigItemType::Bool));
    EXPECT_NE(app->configRegistry()->item(u8"plugin.opt"), nullptr);
    EXPECT_TRUE(ctx.configRegistry()->removeItem(u8"plugin.opt"));
    EXPECT_TRUE(ctx.configRegistry()->removeCategory(u8"插件"));
}

TEST_F(GuiTest, CommandManager_RegistrationOwner)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto pluginName = vine::String(u8"testPlugin");
    const auto otherName  = vine::String(u8"myCommand");
    const auto ownedName  = vine::String(u8"pluginCommand");

    // 清理可能残留的注册，保证用例可重复运行。
    cm->unregisterCommand(otherName);
    cm->unregisterCommand(ownedName);

    // 主机命令：无 owner。
    cm->setRegistrationOwner({});
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(otherName));

    // 插件命令：注册期间打上 owner。
    cm->setRegistrationOwner(pluginName);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(ownedName));
    cm->setRegistrationOwner({});

    // 报告接口：只返回该插件注册的命令，且 owner 正确。
    const auto owned = cm->commandInfosForPlugin(pluginName);
    ASSERT_EQ(owned.size(), 1u);
    EXPECT_TRUE(owned[0].name == ownedName);
    EXPECT_TRUE(owned[0].owner == pluginName);

    const auto others = cm->commandInfosForPlugin(otherName);
    EXPECT_TRUE(others.empty());

    // 全量枚举中 owner 字段按注册来源标记。
    for (const auto& info : cm->commandInfos()) {
        if (info.name == ownedName) {
            EXPECT_TRUE(info.owner == pluginName);
        } else if (info.name == otherName) {
            EXPECT_TRUE(info.owner.empty());
        }
    }

    // 清理注册，避免影响其它用例。
    cm->unregisterCommand(otherName);
    cm->unregisterCommand(ownedName);
}

TEST_F(GuiTest, ConfigRegistry_StandardCategories)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::StandardCategory;
    using vine::appfw::StandardGroup;

    ConfigRegistry reg;

    // standardCategory 是 create-or-get：重复调用返回同一节点，规范名 + label + order。
    auto* log1 = reg.standardCategory(StandardCategory::Logging);
    auto* log2 = reg.standardCategory(StandardCategory::Logging);
    ASSERT_NE(log1, nullptr);
    EXPECT_EQ(log1, log2);
    EXPECT_TRUE(log1->name() == u8"logging");
    EXPECT_TRUE(log1->label() == u8"日志");

    // standardGroup 是 create-or-get：规范名 + label。
    auto* console = reg.standardGroup(StandardCategory::Logging, StandardGroup::Console);
    ASSERT_NE(console, nullptr);
    EXPECT_TRUE(console->name() == u8"console");
    EXPECT_TRUE(console->label() == u8"控制台");
    EXPECT_EQ(reg.standardGroup(StandardCategory::Logging, StandardGroup::Console), console);

    // addItem 便捷接口
    EXPECT_TRUE(reg.addItem(StandardCategory::Logging, StandardGroup::Console,
                            ConfigItem(u8"logging.console_enabled", u8"日志输出到控制台", ConfigItemType::Bool), u8"app_shell"));
    EXPECT_NE(reg.item(u8"logging.console_enabled"), nullptr);

    // 重复 key 拒绝
    EXPECT_FALSE(reg.addItem(StandardCategory::Logging, StandardGroup::Console,
                             ConfigItem(u8"logging.console_enabled", u8"x", ConfigItemType::Bool), u8"other"));
}

TEST_F(GuiTest, ConfigRegistry_Ownership)
{
    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::ConfigRegistry;
    using vine::appfw::StandardCategory;
    using vine::appfw::StandardGroup;

    ConfigRegistry reg;
    reg.addItem(StandardCategory::Logging, StandardGroup::Console,
                ConfigItem(u8"logging.console_enabled", u8"日志输出到控制台", ConfigItemType::Bool), u8"app_shell");
    reg.addItem(StandardCategory::Appearance, StandardGroup::Theme,
                ConfigItem(u8"appearance.theme", u8"主题", ConfigItemType::String), u8"app_shell");
    reg.addItem(StandardCategory::General, StandardGroup::Startup,
                ConfigItem(u8"general.maximize", u8"最大化启动", ConfigItemType::Bool), u8"other_plugin");

    auto app_items = reg.itemsForPlugin(u8"app_shell");
    ASSERT_EQ(app_items.size(), 2u);
    auto other_items = reg.itemsForPlugin(u8"other_plugin");
    ASSERT_EQ(other_items.size(), 1u);
    EXPECT_TRUE(other_items[0]->key() == u8"general.maximize");

    // removeItemsForPlugin 只删该插件的项
    EXPECT_TRUE(reg.removeItemsForPlugin(u8"app_shell"));
    EXPECT_TRUE(reg.itemsForPlugin(u8"app_shell").empty());
    EXPECT_EQ(reg.item(u8"logging.console_enabled"), nullptr);
    EXPECT_EQ(reg.item(u8"appearance.theme"), nullptr);
    EXPECT_NE(reg.item(u8"general.maximize"), nullptr); // 其它插件不受影响

    EXPECT_FALSE(reg.removeItemsForPlugin(u8"app_shell")); // 已无项可删
}

TEST_F(GuiTest, PluginLoadContext_RegisterConfigItem)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    vine::appfw::PluginLoadContext ctx(app, u8"my_plugin");
    EXPECT_TRUE(ctx.pluginName() == u8"my_plugin");

    using vine::appfw::ConfigItem;
    using vine::appfw::ConfigItemType;
    using vine::appfw::StandardCategory;
    using vine::appfw::StandardGroup;

    EXPECT_TRUE(ctx.registerConfigItem(StandardCategory::Logging, StandardGroup::Console,
                                       ConfigItem(u8"logging.file_path", u8"日志文件", ConfigItemType::String)));

    auto items = ctx.registeredConfigs();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items[0]->key() == u8"logging.file_path");

    // 清理共享 registry，避免污染其它测试。
    app->configRegistry()->removeItemsForPlugin(u8"my_plugin");
}

// ============================ 停靠面板 ============================

TEST_F(GuiTest, DockPanelManager_CountAndLookup)
{
    auto* mgr = wnd->dockPanelManager();
    EXPECT_EQ(mgr->count(), 5);
    EXPECT_EQ(mgr->panels().size(), 5u);

    auto* p = mgr->findById(u8"dock_project");
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->title() == u8"Project");
    EXPECT_EQ(p->features(), guifw::DockFeatures::None);

    EXPECT_NE(mgr->findByTitle(u8"Properties"), nullptr);
    EXPECT_EQ(mgr->findByTitle(u8"NotThere"), nullptr);
}

TEST_F(GuiTest, DockPanel_Features)
{
    EXPECT_FALSE(vine::testFlag(panelLeft->features(), guifw::DockFeatures::Closable));
    EXPECT_TRUE(vine::testFlag(panelRight->features(), guifw::DockFeatures::Closable));
    EXPECT_TRUE(vine::testFlag(panelTop->features(), guifw::DockFeatures::Closable));
}

TEST_F(GuiTest, DockPanel_Area)
{
    EXPECT_EQ(panelLeft->dockArea(), guifw::DockAreas::Left);
    EXPECT_EQ(panelRight->dockArea(), guifw::DockAreas::Right);
    EXPECT_EQ(panelTop->dockArea(), guifw::DockAreas::Top);
    EXPECT_EQ(panelBottom->dockArea(), guifw::DockAreas::Bottom);
    EXPECT_EQ(panelBottom2->dockArea(), guifw::DockAreas::Bottom);
}

TEST_F(GuiTest, DockPanel_CollapseRestore)
{
    auto* p = panelLeft;
    EXPECT_FALSE(p->isCollapsed());
    EXPECT_FALSE(p->isPinned());
    EXPECT_FALSE(p->isFloating());
    EXPECT_FALSE(p->isTabbed());

    p->collapse();
    QCoreApplication::processEvents();
    EXPECT_TRUE(p->isCollapsed());

    p->restore();
    QCoreApplication::processEvents();
    EXPECT_FALSE(p->isCollapsed());
    EXPECT_EQ(p->dockArea(), guifw::DockAreas::Left); // 恢复到原区域
}

TEST_F(GuiTest, DockPanel_PinUnpin)
{
    auto* p = panelRight;
    p->pin();
    QCoreApplication::processEvents();
    EXPECT_TRUE(p->isPinned());

    p->unpin();
    QCoreApplication::processEvents();
    EXPECT_FALSE(p->isPinned());
}

// ============================ 进度与串联 ============================

namespace
{

/// 泵送 Qt 事件循环一段时长（用于驱动 QTimer 轮询）。
void pumpEventsFor(int ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

TEST_F(GuiTest, Application_IsBusyReflectsActiveHost)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    EXPECT_FALSE(app->isBusy());

    {
        vine::progress::ProgressHost host;
        host.setForeground(true); // 只有前台宿主才让应用处于忙态
        EXPECT_TRUE(vine::progress::ProgressHost::isActive());
        EXPECT_TRUE(app->isBusy());
        EXPECT_TRUE(vine::progress::ProgressHost::current() == &host);
    }
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());
}

TEST_F(GuiTest, CommandManager_LongRunningCreatesAmbientHost)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"longCmd");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<LongCommand>(name));

    EXPECT_FALSE(vine::progress::ProgressHost::isActive());

    // Task 是惰性的：放到工作线程上跑，主线程轮询观察宿主生命周期。
    auto                 task = cm->executeCommandAsync(name);
    std::atomic<bool>    done{ false };
    vine::appfw::CommandResult result;
    std::thread runner([&] {
        result = vine::async::syncWait(std::move(task));
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!vine::progress::ProgressHost::isActive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 长命令运行期间，环境宿主已挂载、应用处于忙态。
    EXPECT_TRUE(vine::progress::ProgressHost::isActive());
    EXPECT_TRUE(app->isBusy());

    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner.join();
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);
    // 链结束后宿主释放、应用恢复空闲。
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());

    cm->unregisterCommand(name);
}

TEST_F(GuiTest, CommandManager_BusyGateRejectsNewCommand)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"busyDummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    // 空闲时可执行。
    auto ok = cm->executeCommand(name);
    EXPECT_EQ(ok.status(), vine::appfw::CommandStatus::Success);

    {
        // 外部前台长任务持有宿主（模拟用户触发的耗时操作）。
        vine::progress::ProgressHost host;
        host.setForeground(true);
        EXPECT_TRUE(app->isBusy());

        // 忙时新命令被拒并提示。
        auto busy = cm->executeCommand(name);
        EXPECT_EQ(busy.status(), vine::appfw::CommandStatus::Failed);
        EXPECT_EQ(busy.message(), vine::String(u8"Another operation is in progress"));
    }

    // 宿主释放后可再次执行。
    auto after = cm->executeCommand(name);
    EXPECT_EQ(after.status(), vine::appfw::CommandStatus::Success);

    cm->unregisterCommand(name);
}

TEST(ProgressPresenterTest, ShowsBarForActiveHostAndHidesAfter)
{
    guifw::ProgressPresenter presenter;

    // 无宿主：不忙、隐藏。
    EXPECT_FALSE(presenter.isBusy());
    EXPECT_FALSE(presenter.visible());

    {
        vine::progress::ProgressHost host;
        host.setForeground(true); // 前台操作驱动主进度条
        std::thread worker([&] {
            vine::progress::ProgressScope scope(host.range(), "Exporting", 30);
            for (int i = 0; i < 30; ++i) {
                scope.next(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });

        // 超过显示阈值后应出现进度条。
        pumpEventsFor(700);
        EXPECT_TRUE(presenter.isBusy());
        EXPECT_TRUE(presenter.visible());

        worker.join();
    }

    // 宿主结束后，延迟隐藏。
    pumpEventsFor(500);
    EXPECT_FALSE(presenter.isBusy());
    EXPECT_FALSE(presenter.visible());
}

TEST(ProgressPresenterTest, CancelButtonRequestsStop)
{
    guifw::ProgressPresenter     presenter;
    vine::progress::ProgressHost host;
    host.setForeground(true); // 取消按钮只作用于前台操作

    // 包装类不是 QObject：取消按钮在原生控件上查找。
    auto* cancel = presenter.impl<QWidget>()->findChild<QPushButton*>();
    ASSERT_NE(cancel, nullptr);

    pumpEventsFor(150); // 让 presenter 观察到宿主
    EXPECT_TRUE(presenter.isBusy());

    cancel->click();
    pumpEventsFor(50);

    EXPECT_TRUE(host.cancelSource().stop_requested());
    EXPECT_TRUE(host.indicator().isCancelled());
}

TEST_F(GuiTest, CommandManager_NestedProgressRunsChild)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"nestedProgressCmd");
    cm->unregisterCommand(name);
    cm->unregisterCommand(u8"nested_child");
    ASSERT_TRUE(cm->registerCommand<NestedChildCommand>(u8"nested_child"));
    ASSERT_TRUE(cm->registerCommand<NestedProgressCommand>(name));

    EXPECT_FALSE(vine::progress::ProgressHost::isActive());

    // 父命令为 LongRunning → 前台宿主；内部经 executeChild 运行子命令（绕过串联门）。
    auto                 task = cm->executeCommandAsync(name);
    std::atomic<bool>    done{ false };
    vine::appfw::CommandResult result;
    std::thread runner([&] {
        result = vine::async::syncWait(std::move(task));
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!vine::progress::ProgressHost::isActive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 父命令运行期间：前台宿主挂载、应用忙。
    EXPECT_TRUE(vine::progress::ProgressHost::isActive());
    EXPECT_TRUE(app->isBusy());

    // 方案B：子命令压栈顶替父命令 → 前台栈出现深度 2。
    bool saw_depth2 = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (vine::progress::ProgressHost::foregroundStack().size() >= 2) {
            saw_depth2 = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(saw_depth2);

    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner.join();

    // 父命令成功 ⇒ 嵌套子命令未被串联门拦截（executeChild 绕过）。
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);

    // 链结束后宿主释放。
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());

    cm->unregisterCommand(name);
}

// ── CommandManager：执行链、取消与历史 ──────────────────────────────────────

// 可取消命令：睡眠期间响应取消请求。用 None 标志 ⇒ 不挂进度宿主、不触发串联门，
// 因此多个实例可以作为互相独立的链并发运行。
class CancellableSleepCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    CancellableSleepCommand(vine::String name, std::chrono::milliseconds duration)
      : name_(std::move(name))
      , duration_(duration)
    {}

    vine::String name() const override { return name_; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"cancellable sleep"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        co_await vine::async::sleepFor(duration_, context->stopToken());
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

  private:
    vine::String              name_;
    std::chrono::milliseconds duration_;
};

V_OBJECT_META_IMPL(CancellableSleepCommand, vine::appfw::Command)

// 抛异常的命令（挂起后抛，走协程的真实异常路径），用于验证异常不会穿出到
// DetachedTask/事件循环（旧行为是 std::terminate）。
class ThrowingCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"throwing"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"throws after a suspension"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        co_await vine::async::sleepFor(std::chrono::milliseconds(1));
        throw std::runtime_error(s_empty_message.load() ? "" : "boom");
    }

    /// 为 true 时抛出的异常 what() 为空，用于验证结果消息的兜底。
    inline static std::atomic<bool> s_empty_message{ false };
};

V_OBJECT_META_IMPL(ThrowingCommand, vine::appfw::Command)

// Undoable 命令：记录是否真的执行过，用于验证快照回调的先后与拦截。
class UndoableProbeCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"undoableProbe"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"records that it ran"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::Undoable; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        s_ran = true;
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_ran{ false };
    inline static std::atomic<int>  s_snapshots{ 0 };
};

V_OBJECT_META_IMPL(UndoableProbeCommand, vine::appfw::Command)

// 父命令：内部直接用 CommandManager 的顶层入口（而不是 context->executeChild）
// 运行子命令，用于验证“顶层入口不可当嵌套用”。
class TopLevelCallingParentCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"topLevelCaller"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"calls a top-level entry from inside"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::LongRunning; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (!cm) {
            co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Failed, vine::String(u8"no manager"));
        }

        // 父命令持有串联门（LongRunning），顶层入口会被拒。
        const auto child      = cm->executeCommand(s_child_name);
        s_child_status        = child.status();
        s_child_message       = child.message();
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static vine::String              s_child_name{ u8"syncChildInsideParent" };
    inline static vine::appfw::CommandStatus s_child_status{ vine::appfw::CommandStatus::Success };
    inline static vine::String              s_child_message;
};

V_OBJECT_META_IMPL(TopLevelCallingParentCommand, vine::appfw::Command)

// 探针子命令：记录执行时“前台链里是谁”与深度，用于观察链的归属。
class ForegroundProbeChildCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"foregroundProbe"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"records the foreground chain"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (cm) {
            const auto* current    = cm->currentCommand();
            s_foreground_name      = current ? current->name() : vine::String(u8"<none>");
            s_foreground_depth     = cm->runningCount();
        }
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static vine::String s_foreground_name;
    inline static int          s_foreground_depth{ 0 };
};

V_OBJECT_META_IMPL(ForegroundProbeChildCommand, vine::appfw::Command)

// 父命令（非 LongRunning）：内部 co_await 异步顶层入口去跑“子命令”。
// 异步版是惰性的 ⇒ 链在 await 时才建立，且建立后会成为前台链。
class AsyncCallingParentCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"asyncCaller"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"awaits a top-level entry from inside"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (!cm) {
            co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Failed, vine::String(u8"no manager"));
        }

        const auto* before        = cm->currentCommand();
        s_parent_is_foreground    = before != nullptr && before->name() == name();

        const auto child = co_await cm->executeCommandAsync(s_child_name);
        s_child_status   = child.status();

        // 顶层入口把前台链换成了子链，子链结束后不会自动恢复成父链。
        const auto* after = cm->currentCommand();
        s_current_after   = after ? after->name() : vine::String(u8"<none>");
        s_depth_after     = cm->runningCount();
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static vine::String              s_child_name{ u8"foregroundProbe" };
    inline static bool                      s_parent_is_foreground{ false };
    inline static vine::appfw::CommandStatus s_child_status{ vine::appfw::CommandStatus::Success };
    inline static vine::String              s_current_after;
    inline static int                       s_depth_after{ 0 };
};

V_OBJECT_META_IMPL(AsyncCallingParentCommand, vine::appfw::Command)

// 持有串联门的 LongRunning 命令：一直等待直到测试释放，用来确定性地观察
// “门的检查与占用是原子的”。
class GateHoldCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"gateHold"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"holds the serialization gate"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::LongRunning; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        co_await s_release;
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    /// 测试通过 set() 释放；使用前先 reset() 恢复未触发态。
    inline static vine::async::AsyncEvent s_release{};
};

V_OBJECT_META_IMPL(GateHoldCommand, vine::appfw::Command)

// 自递归命令：每次通过 context->executeChild() 再进一层，用于验证嵌套深度上限。
class RecursiveCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"selfNesting"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"nests into itself"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        s_entries.fetch_add(1);
        co_return co_await context->executeChild(u8"selfNesting");
    }

    inline static std::atomic<int> s_entries{ 0 };
};

V_OBJECT_META_IMPL(RecursiveCommand, vine::appfw::Command)

// 排他命令：接管前台，旧链被取消。记录自身是否真的执行过。
class ExclusiveTakeOverCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"takeover"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"exclusive take over"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::Exclusive; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        s_ran = true;
        if (context && context->isCancelled()) {
            co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Cancelled);
        }
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_ran{ false };
};

V_OBJECT_META_IMPL(ExclusiveTakeOverCommand, vine::appfw::Command)

// 不配合取消的命令：睡眠时不传 token，因此取消请求对它无效，只能自然结束。
// 用来验证排他命令的等待是有界的（超时后继续，而不是无限等待）。
class UncooperativeSleepCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"uncooperative"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"ignores cancellation"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        s_running.store(true);
        co_await vine::async::sleepFor(std::chrono::milliseconds(2600));
        s_running.store(false);
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_running{ false };
};

V_OBJECT_META_IMPL(UncooperativeSleepCommand, vine::appfw::Command)

// 探针排他命令：执行时记录被接管的链是否仍在运行。
class ExclusiveProbeCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"probeTakeover"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"probe exclusive take over"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::Exclusive; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        s_saw_victim_running.store(UncooperativeSleepCommand::s_running.load());
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_saw_victim_running{ false };
};

V_OBJECT_META_IMPL(ExclusiveProbeCommand, vine::appfw::Command)

// 父命令：通过 context->executeChild() 嵌套一个可取消子命令，自身不捕获子命令的
// 取消异常（子命令的取消默认向上抛）。用来验证“子命令被取消时也要上报自己结束”。
class NestingCancellableParentCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"nestingParent"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"nests a cancellable child"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        co_return co_await context->executeChild(s_child_name);
    }

    inline static vine::String s_child_name{ u8"nestingChild" };
};

V_OBJECT_META_IMPL(NestingCancellableParentCommand, vine::appfw::Command)

// 历史记录的是执行结果快照，不是命令对象：命令实例在协程返回时即被销毁，
// 旧实现保存原始指针后 historyAt() 返回悬垂指针（ASan 下即 UAF）。
TEST_F(GuiTest, CommandManager_HistoryRecordsValueSnapshots)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"historyDummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    cm->clearHistory();
    ASSERT_EQ(cm->historyCount(), 0);

    const auto result = cm->executeCommand(name);
    ASSERT_EQ(result.status(), vine::appfw::CommandStatus::Success);

    ASSERT_EQ(cm->historyCount(), 1);
    const auto entry = cm->historyAt(0);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, vine::String(u8"dummy"));
    EXPECT_EQ(entry->command_class, DummyCommand::desc());
    EXPECT_EQ(entry->result.status(), vine::appfw::CommandStatus::Success);

    // 越界返回 nullopt（旧实现返回 nullptr，调用方无从区分“越界”与“命令已销毁”）。
    EXPECT_FALSE(cm->historyAt(1).has_value());
    EXPECT_FALSE(cm->historyAt(-1).has_value());

    // 记录与命令生命周期解耦：取消注册后仍可读。
    cm->unregisterCommand(name);
    EXPECT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(cm->historyAt(0).has_value());

    cm->clearHistory();
    EXPECT_EQ(cm->historyCount(), 0);
}

// 排他命令接管正在运行的链：旧链被取消并在有界时间内收尾，新命令在全新链上成功。
TEST_F(GuiTest, CommandManager_ExclusiveTakesOverRunningChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto victim_name = vine::String(u8"exclusiveVictim");
    const auto taker_name  = vine::String(u8"takeoverCmd");
    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(taker_name);
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), victim_name,
                                    [victim_name] {
                                        return new CancellableSleepCommand(victim_name, std::chrono::milliseconds(300));
                                    }));
    ASSERT_TRUE(cm->registerCommand<ExclusiveTakeOverCommand>(taker_name));
    ExclusiveTakeOverCommand::s_ran = false;

    // 后台链：可取消的长睡眠，放到工作线程上驱动，主线程观察。
    auto                        task = cm->executeCommandAsync(victim_name);
    vine::appfw::CommandResult  victim_result;
    std::atomic<bool>           victim_done{ false };
    std::thread                 runner([&] {
        victim_result = vine::async::syncWait(std::move(task));
        victim_done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 1);
    ASSERT_NE(cm->currentCommand(), nullptr);
    EXPECT_EQ(cm->currentCommand()->name(), vine::String(u8"exclusiveVictim"));

    // 接管：有界等待旧链收尾后，排他命令在自己的链上执行。
    const auto takeover = cm->executeCommand(taker_name);
    EXPECT_EQ(takeover.status(), vine::appfw::CommandStatus::Success);
    EXPECT_TRUE(ExclusiveTakeOverCommand::s_ran);

    while (!victim_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.join();

    // 旧链是被取消的（而不是无人取消地继续跑完），且接管后前台没有残留。
    EXPECT_TRUE(victim_done.load());
    EXPECT_EQ(victim_result.status(), vine::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(cm->runningCount(), 0);
    EXPECT_EQ(cm->currentCommand(), nullptr);
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());

    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(taker_name);
}

// 被接管的链拒绝配合取消时，排他命令不会被放行去与新链并发，而是以 Failed 拒绝：
// 旧实现只打一条日志就继续执行，两个链会同时操作共享状态。
TEST_F(GuiTest, CommandManager_ExclusiveIsRejectedWhenChainIgnoresCancellation)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto victim_name = vine::String(u8"uncooperative");
    const auto probe_name  = vine::String(u8"probeTakeover");
    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(probe_name);
    ASSERT_TRUE(cm->registerCommand<UncooperativeSleepCommand>(victim_name));
    ASSERT_TRUE(cm->registerCommand<ExclusiveProbeCommand>(probe_name));
    ExclusiveProbeCommand::s_saw_victim_running = false;

    auto                       task = cm->executeCommandAsync(victim_name);
    vine::appfw::CommandResult victim_result;
    std::thread                runner([&] { victim_result = vine::async::syncWait(std::move(task)); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!UncooperativeSleepCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(UncooperativeSleepCommand::s_running.load());

    // 受害者无视取消 ⇒ 排他命令等到上界后被拒绝，而不是与之并发执行。
    const auto takeover = cm->executeCommand(probe_name);
    EXPECT_EQ(takeover.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_FALSE(ExclusiveProbeCommand::s_saw_victim_running.load());

    runner.join();
    // 受害者并未被取消（它不配合），最终自然成功；它从未与新链重叠。
    EXPECT_EQ(victim_result.status(), vine::appfw::CommandStatus::Success);

    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(probe_name);
}

// cancelCurrent() 只作用于前台链：旧实现让并发链共享同一个 stop_source，
// 取消最近一条链会连带取消另一条（并且替换 stop_source 会让旧链彻底失去取消）。
TEST_F(GuiTest, CommandManager_CancelCurrentStopsOnlyForegroundChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name_a = vine::String(u8"chainA");
    const auto name_b = vine::String(u8"chainB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name_a,
                                    [name_a] {
                                        return new CancellableSleepCommand(name_a, std::chrono::milliseconds(300));
                                    }));
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name_b,
                                    [name_b] {
                                        return new CancellableSleepCommand(name_b, std::chrono::milliseconds(300));
                                    }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 链 A 先运行。
    auto                       task_a = cm->executeCommandAsync(name_a);
    vine::appfw::CommandResult result_a;
    std::thread                runner_a([&] { result_a = vine::async::syncWait(std::move(task_a)); });

    while ((cm->runningCount() == 0 || cm->currentCommand() == nullptr || cm->currentCommand()->name() != name_a)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), name_a);

    // 链 B 随后启动，成为前台链。
    auto                       task_b = cm->executeCommandAsync(name_b);
    vine::appfw::CommandResult result_b;
    std::thread                runner_b([&] { result_b = vine::async::syncWait(std::move(task_b)); });

    while ((cm->currentCommand() == nullptr || cm->currentCommand()->name() != name_b)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), name_b);

    // 只取消前台链 B。
    cm->cancelCurrent();

    runner_a.join();
    runner_b.join();

    EXPECT_EQ(result_b.status(), vine::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(result_a.status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
}

// 注册校验 + 列举元数据在注册时缓存（不再每次列举都实例化命令）。
TEST_F(GuiTest, CommandManager_RegisterValidatesAndCachesMetadata)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"validatedCmd");
    cm->unregisterCommand(name);

    // 空名与空工厂被拒绝，不会留下不可用的注册项。
    EXPECT_FALSE(cm->registerCommand(DummyCommand::desc(), vine::String{}, [] { return new DummyCommand; }));
    EXPECT_FALSE(cm->registerCommand(DummyCommand::desc(), name, std::function<vine::appfw::Command*()>{}));
    EXPECT_FALSE(cm->isRegistered(name));

    std::atomic<int> instantiations{ 0 };
    const auto       factory = [&instantiations] {
        ++instantiations;
        return new DummyCommand;
    };

    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, factory));
    EXPECT_FALSE(cm->registerCommand(DummyCommand::desc(), name, factory));
    // 注册时探测一次以缓存元数据。
    EXPECT_EQ(instantiations.load(), 1);

    for (int i = 0; i < 3; ++i) {
        const auto infos = cm->commandInfos();
        const auto it    = std::find_if(infos.begin(), infos.end(),
                                        [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
        ASSERT_NE(it, infos.end());
        EXPECT_EQ(it->group, vine::String(u8"Test"));
        EXPECT_EQ(it->description, vine::String(u8"dummy command"));
        EXPECT_EQ(it->owner, vine::String{});
    }
    // 旧实现每次 commandInfos() 都会实例化一次 ⇒ 这里会变成 4。
    EXPECT_EQ(instantiations.load(), 1);

    cm->unregisterCommand(name);
    EXPECT_FALSE(cm->isRegistered(name));
}

// 禁用是注册表里的一个标记，不是移除：命令仍在列表里、别名仍指向它，只是不能执行；
// 重新启用后立刻恢复。偏好同时写进配置，插件下次启动重新注册时会带着标记回来。
TEST_F(GuiTest, CommandManager_DisableIsAFlagAndIsPersisted)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"disableProbe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->setCommandEnabled(name, true)) << "先清掉可能残留的偏好";
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return new DummyCommand; }));
    ASSERT_TRUE(cm->registerAlias(vine::String(u8"disableProbeAlias"), name));

    // 禁用：仍是注册项、仍在列表里（enabled=false），但不可执行。
    ASSERT_TRUE(cm->setCommandEnabled(name, false));
    EXPECT_FALSE(cm->isCommandEnabled(name));
    EXPECT_FALSE(cm->isRegistered(name)) << "禁用的命令不能执行";
    EXPECT_FALSE(cm->isRegistered(vine::String(u8"disableProbeAlias"))) << "别名解析到禁用的命令，同样不可执行";

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(),
                                    [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end()) << "禁用不把命令从列表里移除";
    EXPECT_FALSE(it->enabled);

    const auto blocked = cm->executeCommand(name);
    EXPECT_EQ(blocked.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(blocked.message(), vine::String(u8"命令“disableProbe”已被禁用；可在「命令管理器」中启用。"))
        << "禁用与'未注册'要能区分，而且这句是要直接给用户看的";

    const auto disabled = cm->disabledCommands();
    EXPECT_NE(std::find(disabled.begin(), disabled.end(), name), disabled.end()) << "偏好应被记录";

    // 重启模拟：注销后重新注册（插件每次启动都会重新注册），标记仍在。
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return new DummyCommand; }));
    EXPECT_FALSE(cm->isCommandEnabled(name)) << "重新注册的命令应带着禁用偏好回来";

    // 启用后立刻恢复执行，并从偏好里移除。
    ASSERT_TRUE(cm->setCommandEnabled(name, true));
    EXPECT_TRUE(cm->isRegistered(name));
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Success);
    const auto after = cm->disabledCommands();
    EXPECT_EQ(std::find(after.begin(), after.end(), name), after.end());

    cm->unregisterAlias(vine::String(u8"disableProbeAlias"));
    cm->unregisterCommand(name);
}

// 调用方自己 new 的实例也必须挡住：实例按自己的 name() 运行，否则禁用就有一个后门。
TEST_F(GuiTest, CommandManager_DisableBlocksCallerSuppliedInstance)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    // DummyCommand::name() 就是 u8"dummy"：注册名与实例名一致才谈得上拦它。
    const auto name = vine::String(u8"dummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->setCommandEnabled(name, true));
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return new DummyCommand; }));

    EXPECT_EQ(cm->executeCommand(new DummyCommand).status(), vine::appfw::CommandStatus::Success);

    ASSERT_TRUE(cm->setCommandEnabled(name, false));
    const auto blocked = cm->executeCommand(new DummyCommand);
    EXPECT_EQ(blocked.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(blocked.message(), vine::String(u8"命令“dummy”已被禁用；可在「命令管理器」中启用。"));

    // 未注册的名字不受影响：临时造的命令照旧执行。
    ASSERT_TRUE(cm->setCommandEnabled(name, true));
    cm->unregisterCommand(name);
    EXPECT_EQ(cm->executeCommand(new DummyCommand).status(), vine::appfw::CommandStatus::Success);
}

// 工厂返回空指针不应让列举崩溃（旧实现在 commandInfos() 里直接解引用）。
TEST_F(GuiTest, CommandManager_NullFactoryIsTolerated)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"brokenFactory");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name,
                                    [] { return static_cast<vine::appfw::Command*>(nullptr); }));
    EXPECT_TRUE(cm->isRegistered(name));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(),
                                    [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_TRUE(it->group.empty());
    EXPECT_TRUE(it->description.empty());

    // 执行也返回 Failed，而不是解引用空指针。
    const auto result = cm->executeCommand(name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Failed);

    cm->unregisterCommand(name);
}

// 别名挂接：按名字索引挂到目标条目，目标不存在时忽略（registerAlias 允许先建别名）。
TEST_F(GuiTest, CommandManager_CommandInfosAttachesAliases)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"aliasTarget");
    cm->unregisterCommand(name);
    cm->unregisterAlias(vine::String(u8"aliasTargetAlias"));
    cm->unregisterAlias(vine::String(u8"aliasWithoutTarget"));

    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    ASSERT_TRUE(cm->registerAlias(vine::String(u8"aliasTargetAlias"), name));
    ASSERT_TRUE(cm->registerAlias(vine::String(u8"aliasWithoutTarget"), vine::String(u8"notRegisteredAtAll")));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(),
                                    [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    ASSERT_EQ(it->aliases.size(), 1u);
    EXPECT_EQ(it->aliases.front(), vine::String(u8"aliasTargetAlias"));

    // 结果按名字排序。
    EXPECT_TRUE(std::is_sorted(infos.begin(), infos.end(),
                               [](const vine::appfw::CommandInfo& a, const vine::appfw::CommandInfo& b) {
                                   return a.name < b.name;
                               }));

    // 别名可以启动目标命令。
    EXPECT_EQ(cm->executeCommand(vine::String(u8"aliasTargetAlias")).status(), vine::appfw::CommandStatus::Success);

    cm->unregisterAlias(vine::String(u8"aliasTargetAlias"));
    cm->unregisterAlias(vine::String(u8"aliasWithoutTarget"));
    cm->unregisterCommand(name);
}

// Undoable 命令的快照回调：先快照后执行；回调失败 ⇒ 命令不执行且返回 Failed；
// 清空回调后不再被调用（回调可在运行期从任何线程替换，读写都受锁保护）。
TEST_F(GuiTest, CommandManager_UndoableSnapshotHandlerRunsBeforeExecution)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"undoableProbe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<UndoableProbeCommand>(name));

    // 1) 正常路径：快照先于命令体。
    UndoableProbeCommand::s_ran       = false;
    UndoableProbeCommand::s_snapshots = 0;
    cm->setSnapshotHandler([] { UndoableProbeCommand::s_snapshots.fetch_add(1); });
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(UndoableProbeCommand::s_snapshots.load(), 1);
    EXPECT_TRUE(UndoableProbeCommand::s_ran.load());

    // 2) 快照失败：不得执行命令（否则之后的 undo 会回到不存在的状态）。
    cm->setSnapshotHandler([] { throw std::runtime_error("snapshot boom"); });
    UndoableProbeCommand::s_ran = false;
    const auto failed         = cm->executeCommand(name);
    EXPECT_EQ(failed.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(failed.message(), vine::String(u8"snapshot boom"));
    EXPECT_FALSE(UndoableProbeCommand::s_ran.load());

    // 3) 清空回调：不再调用，命令正常执行。
    cm->setSnapshotHandler({});
    UndoableProbeCommand::s_ran = false;
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Success);
    EXPECT_TRUE(UndoableProbeCommand::s_ran.load());
    EXPECT_EQ(UndoableProbeCommand::s_snapshots.load(), 1);

    cm->setSnapshotHandler({});
    cm->unregisterCommand(name);
}

// 顶层入口用错位置会被串联门拦住：LongRunning 父命令内部调 executeCommand() 属于新链，
// 得到 Failed（在命令内部跑子命令必须走 context->executeChild()）。
TEST_F(GuiTest, CommandManager_TopLevelCallInsideLongRunningParentIsRefused)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent_name = vine::String(u8"topLevelCaller");
    const auto child_name  = TopLevelCallingParentCommand::s_child_name;
    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(child_name));
    ASSERT_TRUE(cm->registerCommand<TopLevelCallingParentCommand>(parent_name));
    TopLevelCallingParentCommand::s_child_status  = vine::appfw::CommandStatus::Success;
    TopLevelCallingParentCommand::s_child_message = {};

    const auto result = cm->executeCommand(parent_name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(TopLevelCallingParentCommand::s_child_status, vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(TopLevelCallingParentCommand::s_child_message, vine::String(u8"Another operation is in progress"));

    // 门随父命令结束释放。
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
}

// 命令内部 co_await 异步顶层入口 = 真正新开一条链（Task 惰性 ⇒ 调用时不建链，await 时才建）：
// 子命令在新链上运行并自己成为前台链，父链被顶下去且结束后不会自动恢复。
TEST_F(GuiTest, CommandManager_AsyncTopLevelCallInsideCommandOpensNewChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent_name = vine::String(u8"asyncCaller");
    const auto child_name  = AsyncCallingParentCommand::s_child_name;
    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<ForegroundProbeChildCommand>(child_name));
    ASSERT_TRUE(cm->registerCommand<AsyncCallingParentCommand>(parent_name));
    ForegroundProbeChildCommand::s_foreground_name.clear();
    ForegroundProbeChildCommand::s_foreground_depth = 0;
    AsyncCallingParentCommand::s_parent_is_foreground = false;
    AsyncCallingParentCommand::s_child_status         = vine::appfw::CommandStatus::Failed;
    AsyncCallingParentCommand::s_current_after.clear();
    AsyncCallingParentCommand::s_depth_after = -1;

    const auto result = cm->executeCommand(parent_name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);

    // 父命令确实在前台链上（否则后面的对照没意义）。
    EXPECT_TRUE(AsyncCallingParentCommand::s_parent_is_foreground);
    // 子命令新链上跑成功，并且在它自己运行时它就是前台。
    EXPECT_EQ(AsyncCallingParentCommand::s_child_status, vine::appfw::CommandStatus::Success);
    EXPECT_EQ(ForegroundProbeChildCommand::s_foreground_name, vine::String(u8"foregroundProbe"));
    EXPECT_EQ(ForegroundProbeChildCommand::s_foreground_depth, 1);
    // 前台链不被恢复：子链已结束 ⇒ 父命令还在跑但前台查不到任何人。
    EXPECT_EQ(AsyncCallingParentCommand::s_current_after, vine::String(u8"<none>"));
    EXPECT_EQ(AsyncCallingParentCommand::s_depth_after, 0);

    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
}

// 别名可以指向别名：解析在“使用时”沿链走到已注册命令；环路不会死循环。
TEST_F(GuiTest, CommandManager_ResolvesAliasChains)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name   = vine::String(u8"chainTarget");
    const auto first  = vine::String(u8"chainAlias1");
    const auto second = vine::String(u8"chainAlias2");
    const auto loop_a = vine::String(u8"loopA");
    const auto loop_b = vine::String(u8"loopB");
    cm->unregisterCommand(name);
    cm->unregisterAlias(first);
    cm->unregisterAlias(second);
    cm->unregisterAlias(loop_a);
    cm->unregisterAlias(loop_b);

    // 先建别名、后注册命令：链式解析仍须生效（旧实现只解析一层 ⇒ 失败）。
    ASSERT_TRUE(cm->registerAlias(first, second));
    ASSERT_TRUE(cm->registerAlias(second, name));
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    EXPECT_TRUE(cm->isRegistered(first));
    EXPECT_EQ(cm->executeCommand(first).status(), vine::appfw::CommandStatus::Success);

    // 两个别名都挂在最终解析到的命令下。
    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(),
                                    [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_EQ(it->aliases.size(), 2u);

    // 环：不挂死，只是解析不到命令。
    ASSERT_TRUE(cm->registerAlias(loop_a, loop_b));
    ASSERT_TRUE(cm->registerAlias(loop_b, loop_a));
    EXPECT_FALSE(cm->isRegistered(loop_a));
    EXPECT_EQ(cm->executeCommand(loop_a).status(), vine::appfw::CommandStatus::Failed);

    cm->unregisterAlias(first);
    cm->unregisterAlias(second);
    cm->unregisterAlias(loop_a);
    cm->unregisterAlias(loop_b);
    cm->unregisterCommand(name);
}

// 嵌套深度有上界：自我递归的命令最多加到 maxChainDepth() 层，然后被拒。
TEST_F(GuiTest, CommandManager_RejectsTooDeepNesting)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"selfNesting");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<RecursiveCommand>(name));
    RecursiveCommand::s_entries = 0;

    const auto result = cm->executeCommand(name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(result.message(), vine::String(u8"Command nesting is too deep"));
    EXPECT_EQ(RecursiveCommand::s_entries.load(), vine::appfw::CommandManager::maxChainDepth());
    EXPECT_EQ(cm->runningCount(), 0);
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());

    cm->unregisterCommand(name);
}

// 历史有容量上限：超出后丢弃最旧的一条，长时间运行不会无限增长。
TEST_F(GuiTest, CommandManager_HistoryIsBounded)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"historyBound");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    cm->clearHistory();

    const auto cap   = static_cast<int>(vine::appfw::CommandManager::maxHistoryEntries());
    const auto total = cap + 3;
    for (int i = 0; i < total; ++i) {
        ASSERT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Success);
    }

    EXPECT_EQ(cm->historyCount(), cap);
    EXPECT_TRUE(cm->historyAt(0).has_value());
    EXPECT_TRUE(cm->historyAt(cap - 1).has_value());
    EXPECT_FALSE(cm->historyAt(cap).has_value());

    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 命令抛出的异常被收口为 Failed 结果：旧行为会穿到 DetachedTask 直接 std::terminate()
// （树内 VisualUserIO 的控制台命令正是走这条路径）。
TEST_F(GuiTest, CommandManager_CommandExceptionBecomesFailedResult)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"throwingCmd");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<ThrowingCommand>(name));
    cm->clearHistory();

    // 同步路径：不向调用方抛异常，而是返回 Failed 并带上原因。
    const auto result = cm->executeCommand(name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(result.message(), vine::String(u8"boom"));

    // detached 路径：旧实现会在 DetachedTask 里 terminate 掉整个进程。
    cm->executeDetached(name);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->historyCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 2);
    const auto entry = cm->historyAt(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, vine::String(u8"throwing"));
    EXPECT_EQ(entry->result.status(), vine::appfw::CommandStatus::Failed);

    // 异常本身没有描述时，结果消息要有兜底文本（否则 UI 只能显示一条无信息的失败）。
    ThrowingCommand::s_empty_message = true;
    const auto silent             = cm->executeCommand(name);
    EXPECT_EQ(silent.status(), vine::appfw::CommandStatus::Failed);
    EXPECT_FALSE(silent.message().empty());
    ThrowingCommand::s_empty_message = false;

    cm->unregisterCommand(name);
}

// 抛异常的工厂不会让注册/执行抛穿：注册仍成功（无缓存元数据），执行收口为 Failed。
TEST_F(GuiTest, CommandManager_ThrowingFactoryDoesNotEscape)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"throwingFactory");
    cm->unregisterCommand(name);

    const auto factory = []() -> vine::appfw::Command* {
        throw std::runtime_error("factory boom");
    };
    EXPECT_NO_THROW(EXPECT_TRUE(cm->registerCommand(DummyCommand::desc(), name, factory)));
    EXPECT_TRUE(cm->isRegistered(name));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(),
                                    [&name](const vine::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_TRUE(it->group.empty());
    EXPECT_TRUE(it->description.empty());

    EXPECT_NO_THROW({
        const auto result = cm->executeCommand(name);
        EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Failed);
        EXPECT_EQ(result.message(), vine::String(u8"factory boom"));
    });

    cm->unregisterCommand(name);
}

// cancelCurrent() 只取消前台链；cancelAll() 才能触到被顶出前台的 detached 链。
TEST_F(GuiTest, CommandManager_CancelAllReachesBackgroundChains)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name_a = vine::String(u8"backgroundA");
    const auto name_b = vine::String(u8"foregroundB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
    // A 睡很久，保证它在 B 结束后仍然活着；B 短，取消后很快落定。
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name_a,
                                    [name_a] {
                                        return new CancellableSleepCommand(name_a, std::chrono::milliseconds(3000));
                                    }));
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name_b,
                                    [name_b] {
                                        return new CancellableSleepCommand(name_b, std::chrono::milliseconds(200));
                                    }));
    cm->clearHistory();

    // detached 启动 ⇒ 两者都是独立链，B 后启动成为前台。
    cm->executeDetached(name_a);
    cm->executeDetached(name_b);
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), name_b);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 只取消前台：B 落定，A 仍在跑（历史里只有一条记录）。
    cm->cancelCurrent();
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->name, name_b);
    EXPECT_EQ(cm->historyAt(0)->result.status(), vine::appfw::CommandStatus::Cancelled);

    // cancelAll() 触到后台链 A。
    cm->cancelAll();
    while (cm->historyCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 2);
    ASSERT_TRUE(cm->historyAt(1).has_value());
    EXPECT_EQ(cm->historyAt(1)->name, name_a);
    EXPECT_EQ(cm->historyAt(1)->result.status(), vine::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(cm->runningCount(), 0);

    cm->clearHistory();
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
}

// 被取消的嵌套子命令也要上报自己结束：executing 已经发出，executed 与历史不能缺席
// （旧实现直接 throw 上抛，子命令既没有 executed 事件也没有历史记录）。
TEST_F(GuiTest, CommandManager_NestedCancelledChildIsReported)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent = vine::String(u8"nestingParent");
    const auto child  = vine::String(u8"nestingChild");
    cm->unregisterCommand(parent);
    cm->unregisterCommand(child);
    ASSERT_TRUE(cm->registerCommand<NestingCancellableParentCommand>(parent));
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), child, [child] {
        return new CancellableSleepCommand(child, std::chrono::milliseconds(5000));
    }));
    cm->clearHistory();

    std::atomic<int> child_executing{ 0 };
    std::atomic<int> child_executed{ 0 };

    const auto executing_id = cm->executing.addHandler(
        [&](vine::appfw::CommandManager&, vine::appfw::CommandExecutingEventArgs& args) {
            const auto* c = args.command();
            if (c != nullptr && c->name() == child) {
                ++child_executing;
            }
        });
    const auto executed_id = cm->executed.addHandler(
        [&](vine::appfw::CommandManager&, vine::appfw::CommandExecutedEventArgs& args) {
            const auto* c = args.command();
            if (c != nullptr && c->name() == child) {
                ++child_executed;
            }
        });

    auto                       task = cm->executeCommandAsync(parent);
    vine::appfw::CommandResult result;
    std::thread                runner([&] { result = vine::async::syncWait(std::move(task)); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->runningCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 2) << "父命令与嵌套子命令应在同一条链上";

    cm->cancelCurrent();
    runner.join();

    cm->executing.removeHandler(executing_id);
    cm->executed.removeHandler(executed_id);

    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(child_executing.load(), 1);
    EXPECT_EQ(child_executed.load(), 1) << "被取消的嵌套子命令必须发出 executed 事件";

    ASSERT_EQ(cm->historyCount(), 2) << "父子命令都应进历史";
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->name, child);
    EXPECT_EQ(cm->historyAt(0)->result.status(), vine::appfw::CommandStatus::Cancelled);
    ASSERT_TRUE(cm->historyAt(1).has_value());
    EXPECT_EQ(cm->historyAt(1)->name, parent);
    EXPECT_EQ(cm->historyAt(1)->result.status(), vine::appfw::CommandStatus::Cancelled);

    cm->clearHistory();
    cm->unregisterCommand(parent);
    cm->unregisterCommand(child);
}

// 排他命令必须停掉每一条活链，而不只是前台链：旧实现只取消前台链，若后台链
// 已被别的顶层命令顶出前台，排他命令就会与它并发执行。
TEST_F(GuiTest, CommandManager_ExclusiveStopsEveryLiveChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto victim = vine::String(u8"backgroundVictim");
    const auto quick  = vine::String(u8"quickForeground");
    const auto taker  = vine::String(u8"takeover");
    cm->unregisterCommand(victim);
    cm->unregisterCommand(quick);
    cm->unregisterCommand(taker);
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), victim, [victim] {
        return new CancellableSleepCommand(victim, std::chrono::milliseconds(3000));
    }));
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(quick));
    ASSERT_TRUE(cm->registerCommand<ExclusiveTakeOverCommand>(taker));
    ExclusiveTakeOverCommand::s_ran = false;
    cm->clearHistory();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 后台链先跑起来（她是前台），由工作线程驱动。
    auto                       task = cm->executeCommandAsync(victim);
    vine::appfw::CommandResult victim_result;
    std::atomic<bool>          victim_done{ false };
    std::thread                runner([&] {
        victim_result = vine::async::syncWait(std::move(task));
        victim_done.store(true);
    });

    while ((cm->currentCommand() == nullptr || cm->currentCommand()->name() != victim)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), victim);

    // 一条普通顶层命令把受害者顶出前台（她不是 LongRunning，门是开的）。
    ASSERT_EQ(cm->executeCommand(quick).status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(cm->currentCommand(), nullptr) << "前台已换成刚结束的 quick 链";

    // 排他命令：接管时必须连后台链一起收掉，并在放行前等到她真的结束。
    const auto takeover = cm->executeCommand(taker);
    EXPECT_EQ(takeover.status(), vine::appfw::CommandStatus::Success);
    EXPECT_TRUE(ExclusiveTakeOverCommand::s_ran);
    EXPECT_TRUE(victim_done.load()) << "排他命令运行时后台链必须已经收尾，不能并发";

    while (!victim_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.join();
    EXPECT_EQ(victim_result.status(), vine::appfw::CommandStatus::Cancelled);

    cm->clearHistory();
    cm->unregisterCommand(victim);
    cm->unregisterCommand(quick);
    cm->unregisterCommand(taker);
}

// 取消别名要落到命令本身上：执行路径都会解析别名，偏好只记在别名上就会静默失效。
TEST_F(GuiTest, CommandManager_DisablingAnAliasDisablesTheCommand)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name  = vine::String(u8"aliasDisableTarget");
    const auto alias = vine::String(u8"aliasDisableBridge");
    cm->unregisterCommand(name);
    cm->unregisterAlias(alias);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    ASSERT_TRUE(cm->registerAlias(alias, name));

    EXPECT_TRUE(cm->setCommandEnabled(alias, false));
    EXPECT_FALSE(cm->isCommandEnabled(alias));
    EXPECT_FALSE(cm->isCommandEnabled(name));
    EXPECT_EQ(cm->executeCommand(alias).status(), vine::appfw::CommandStatus::Failed);
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Failed);

    // 启用同样按别名落到命令上。
    EXPECT_TRUE(cm->setCommandEnabled(alias, true));
    EXPECT_TRUE(cm->isCommandEnabled(name));
    EXPECT_EQ(cm->executeCommand(alias).status(), vine::appfw::CommandStatus::Success);

    cm->unregisterAlias(alias);
    cm->unregisterCommand(name);
}

// 关停入口：取消全部活链并等到它们真的收尾（管理器被销毁前必须满足的前提）。
TEST_F(GuiTest, CommandManager_CancelAllAndWaitDrainsChains)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    // 空闲时立即成功。
    EXPECT_TRUE(cm->cancelAllAndWait());

    const auto name = vine::String(u8"drainTarget");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name, [name] {
        return new CancellableSleepCommand(name, std::chrono::milliseconds(3000));
    }));
    cm->clearHistory();

    cm->executeDetached(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 1);

    // 取消并等待：返回即意味着链已经收尾（历史已落定、无前台残留）。
    EXPECT_TRUE(cm->cancelAllAndWait());
    EXPECT_EQ(cm->runningCount(), 0);
    EXPECT_EQ(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->result.status(), vine::appfw::CommandStatus::Cancelled);

    // 空闲时再调用一次：幂等，立即成功。
    EXPECT_TRUE(cm->cancelAllAndWait());

    cm->clearHistory();
    cm->unregisterCommand(name);
}

// detached 的失败回写必须落在应用线程上：命令可能在定时器线程上结束，而 GUI 的
// UserIO 会写 QWidget，只有应用线程可以碰（旧实现直接调用 putString）。
TEST_F(GuiTest, CommandManager_DetachedFailureIsReportedOnTheApplicationThread)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"detachedThrow");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<ThrowingCommand>(name));
    cm->clearHistory();

    guifw::ConsolePanel panel;
    app->setConsolePanel(&panel);
    QWidget* root = panel.impl<QWidget>();
    ASSERT_NE(root, nullptr);
    auto* output = root->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    // 命令在定时器线程上抛异常 ⇒ 失败上报只能编组回应用线程。
    cm->executeDetached(name);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->result.status(), vine::appfw::CommandStatus::Failed);

    // 命令已经记录完失败，但消息还没写进面板：它被投递到应用线程，等事件循环取。
    EXPECT_FALSE(output->toPlainText().contains(u8"boom"))
        << "失败消息不得从工作线程直接写进控制台面板";
    ASSERT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
    EXPECT_TRUE(output->toPlainText().contains(u8"boom")) << "消息应已投递到应用线程";

    app->setConsolePanel(nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 串联门：门的检查与占用在同一临界区内完成 ⇒ 并发提交两个顶层 LongRunning 命令
// 恰有一个被接受（旧实现检查与占用分离，两者都可能通过）。
TEST_F(GuiTest, CommandManager_GateAdmitsAtMostOneTopLevelLongRunningCommand)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"gateHold");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<GateHoldCommand>(name));
    ASSERT_FALSE(vine::progress::ProgressHost::isActive());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 阶段 1：持有者挂着不放 ⇒ 第二个顶层命令被门拒绝。
    GateHoldCommand::s_release.reset();
    vine::appfw::CommandResult holder_result;
    std::thread                holder([&] { holder_result = cm->executeCommand(name); });

    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 1);
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Failed);

    GateHoldCommand::s_release.set();
    holder.join();
    EXPECT_EQ(holder_result.status(), vine::appfw::CommandStatus::Success);
    EXPECT_FALSE(vine::progress::ProgressHost::isActive());

    // 阶段 2：两个线程同时抢门 ⇒ 恰好一个成功（赢家一直持有门到我们释放）。
    GateHoldCommand::s_release.reset();
    std::atomic<int>  successes{ 0 };
    std::atomic<int>  rejections{ 0 };
    std::atomic<int>  ready{ 0 };
    std::atomic<bool> go{ false };
    const auto        racer = [&] {
        ready.fetch_add(1);
        while (!go.load()) {
            std::this_thread::yield();
        }
        const auto result = cm->executeCommand(name);
        if (result.succeeded()) {
            successes.fetch_add(1);
        }
        else {
            rejections.fetch_add(1);
        }
    };
    std::thread first(racer);
    std::thread second(racer);
    while (ready.load() < 2) {
        std::this_thread::yield();
    }
    go.store(true);

    // 赢家被接受后挂在 s_release 上（executeCommand 会阻塞），输家被门拒绝后立即返回。
    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(cm->runningCount(), 1);
    while (rejections.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_EQ(rejections.load(), 1);
    EXPECT_EQ(successes.load(), 0);

    GateHoldCommand::s_release.set();
    first.join();
    second.join();
    EXPECT_EQ(successes.load(), 1);

    EXPECT_FALSE(vine::progress::ProgressHost::isActive());
    EXPECT_EQ(cm->runningCount(), 0);
    cm->unregisterCommand(name);
}

// 注册表在并发访问下保持可用（注册/注销/列举/按名创建同时进行）。
TEST_F(GuiTest, CommandManager_RegistrySurvivesConcurrentAccess)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name_a = vine::String(u8"concurrentA");
    const auto name_b = vine::String(u8"concurrentB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);

    std::atomic<bool> go{ false };
    std::atomic<int>  failures{ 0 };
    const auto        worker = [&](const vine::String& name) {
        while (!go.load()) {
            std::this_thread::yield();
        }
        for (int i = 0; i < 50; ++i) {
            if (!cm->registerCommand<DummyCommand>(name) || !cm->isRegistered(name)) {
                failures.fetch_add(1);
                return;
            }
            (void)cm->names();
            (void)cm->commandInfos();
            (void)cm->executeCommand(name);
            if (!cm->unregisterCommand(name)) {
                failures.fetch_add(1);
                return;
            }
        }
    };

    std::thread first(worker, name_a);
    std::thread second(worker, name_b);
    go.store(true);
    first.join();
    second.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_FALSE(cm->isRegistered(name_a));
    EXPECT_FALSE(cm->isRegistered(name_b));
}

// 命令内部同步调用 executeCommand() 不会死锁（syncWait 在调用线程上驱动任务），
// 但会开一条新链——嵌套应当走 context->executeChild()。
TEST_F(GuiTest, CommandManager_NestedSyncExecuteCommandDoesNotDeadlock)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto child_name = vine::String(u8"syncChild");
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(child_name));

    const auto parent = [cm, child_name]() -> vine::async::Task<vine::appfw::CommandResult> {
        co_return cm->executeCommand(child_name);
    };

    const auto result = vine::async::syncWait(parent());
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(child_name);
}

// ============================ 插件生命周期 ============================
//
// PluginManager 从默认插件目录（<build>/plugins/vine）发现插件。测试环境里
// 一定存在 app_shell 与依赖它的 test_plugin（见 test_plugin/TestPlugin.cpp 的
// V_DECLARE_PLUGIN 依赖声明）。这些用例依赖该目录，缺失时直接跳过。
//
// 用例之间共享进程内的插件状态，必须按声明顺序执行：先验证"依赖被禁用则
// 依赖者不加载"，再验证"被禁用的插件仍可见但不加载"，最后验证卸载。

namespace
{

/// 在插件状态列表里按名字查找，未找到返回 nullptr。
const vine::appfw::PluginEntry* findPluginEntry(const std::vector<vine::appfw::PluginEntry>& entries,
                                                const vine::String&                        name)
{
    for (const auto& entry : entries) {
        if (entry.info.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// 把 vine::String 转成 std::string，用于在 JSON 文本里查找子串。
std::string toUtf8(const vine::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// 在注册列表里按 id 查找，未找到返回 nullptr。
const vine::appfw::PluginRegistration* findRegistration(const std::vector<vine::appfw::PluginRegistration>& registrations,
                                                        const vine::String& id)
{
    for (const auto& registration : registrations) {
        if (registration.id == id) {
            return &registration;
        }
    }
    return nullptr;
}

/// 找到一个插件库文件（用于"安装到别处"的测试），找不到返回空路径。
std::filesystem::path findPluginLibrary(const char* stem)
{
    std::error_code ec;
    const auto      dir = vine::appfw::PluginManager::builtInPluginDirectory();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().stem().string().rfind(stem, 0) == 0) {
            return entry.path();
        }
    }
    return {};
}

/// 临时改 PluginManager::builtInPluginDirectory()，离开作用域自动还原（断言失败也不漏还原）。
class BuiltInPluginDirectoryScope {
  public:
    explicit BuiltInPluginDirectoryScope(const std::filesystem::path& dir)
      : saved_(vine::appfw::PluginManager::builtInPluginDirectory())
    {
        vine::appfw::PluginManager::setBuiltInPluginDirectory(dir);
    }

    ~BuiltInPluginDirectoryScope() { vine::appfw::PluginManager::setBuiltInPluginDirectory(saved_); }

    BuiltInPluginDirectoryScope(const BuiltInPluginDirectoryScope&)            = delete;
    BuiltInPluginDirectoryScope& operator=(const BuiltInPluginDirectoryScope&) = delete;

  private:
    std::filesystem::path saved_;
};

/// 把插件库拷到程序目录之外、注册为"当前用户安装"，并把程序目录临时换成空目录。
///
/// 用来自出一个真实的用户安装场景：沙箱里的插件都是 PluginScope::User，因此可以
/// 禁用/卸载；析构时卸载并反注册、删目录、还原程序目录（断言失败也不漏）。
class UserPluginSandbox {
  public:
    UserPluginSandbox(vine::appfw::PluginManager* manager, const std::vector<const char*>& stems)
      : manager_(manager)
    {
        // Copy the libraries while the application directory is still the real
        // one: findPluginLibrary() looks in PluginManager::builtInPluginDirectory().
        std::error_code ec;
        std::filesystem::remove_all(directory(), ec);
        std::filesystem::create_directories(directory(), ec);

        for (const char* stem : stems) {
            const auto library = findPluginLibrary(stem);
            if (library.empty()) {
                continue;
            }
            std::filesystem::copy_file(library, directory() / library.filename(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }

        if (manager_ != nullptr) {
            registration_ = manager_->installPlugin(vine::String(directory().u8string()), vine::appfw::PluginScope::User);
        }

        // Only now hide the application directory, so the sandbox plugins are the
        // only ones a scan can find.
        directory_scope_ = std::make_unique<BuiltInPluginDirectoryScope>(emptyDirectory());
    }

    ~UserPluginSandbox()
    {
        std::error_code ec;
        if (manager_ != nullptr) {
            // Ignored on purpose: the sandbox only wants the instances gone and
            // has already reported failures through the test that ran.
            static_cast<void>(manager_->unloadAll());
            if (!registration_.empty()) {
                // Ignored on purpose: the sandbox is gone either way and the next
                // test installs its own registration.
                static_cast<void>(manager_->uninstallPlugin(registration_, vine::appfw::PluginScope::User));
            }
        }
        std::filesystem::remove_all(directory(), ec);
        std::filesystem::remove_all(emptyDirectory(), ec);
    }

    UserPluginSandbox(const UserPluginSandbox&)            = delete;
    UserPluginSandbox& operator=(const UserPluginSandbox&) = delete;

    const std::filesystem::path& directory() const { return directory_; }
    const vine::String&          registration() const { return registration_; }

  private:
    static std::filesystem::path emptyDirectory()
    {
        return std::filesystem::temp_directory_path() / "vine_sandbox_empty_plugin_dir";
    }

    vine::appfw::PluginManager*           manager_;
    std::unique_ptr<BuiltInPluginDirectoryScope> directory_scope_;
    std::filesystem::path       directory_{ std::filesystem::temp_directory_path() / "vine_test_plugins" };
    vine::String                registration_;
};

/// 进程内的宿主跳过列表作用域：构造时替换，析构时恢复，避免用例之间互相影响。
/// 注意 skipList() 返回的是内部列表的视图，必须先拷贝再 setSkipList()。
class SkipListScope {
  public:
    explicit SkipListScope(std::vector<vine::String> names)
      : saved_(vine::appfw::PluginManager::skipList())
    {
        vine::appfw::PluginManager::setSkipList(names);
    }

    ~SkipListScope() { vine::appfw::PluginManager::setSkipList(saved_); }

    SkipListScope(const SkipListScope&)            = delete;
    SkipListScope& operator=(const SkipListScope&) = delete;

  private:
    std::vector<vine::String> saved_;
};


constexpr char8_t s_shell_plugin[]     = u8"app_shell";   // 被依赖的插件
constexpr char8_t s_dependent_plugin[] = u8"test_plugin"; // 依赖 app_shell 的插件

/// 插件测试需要真实插件库（Debug 构建的文件名带 d 后缀，如 app_shelld.so）；
/// 没有构建插件时跳过，而不是失败或空跑。
bool builtInPluginDirectoryReady()
{
    std::error_code ec;
    const auto      dir = vine::appfw::PluginManager::builtInPluginDirectory();
    if (!std::filesystem::is_directory(dir, ec)) {
        return false;
    }

    const auto hasLibrary = [&dir, &ec](const char* stem) {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            if (entry.path().stem().string().rfind(stem, 0) == 0) {
                return true;
            }
        }
        return false;
    };

    return hasLibrary("app_shell") && hasLibrary("test_plugin");
}

} // namespace

// test_plugin 依赖 app_shell：被依赖方被禁用时，依赖它的插件不得加载（否则会
// 带着缺失依赖跑起来），并把它作为 "disabled dependency" 而不是 "missing" 报告。
//
// 程序自带的插件不可禁用，所以这里用真实场景：两个插件都从程序目录之外注册
// （User 作用域），程序目录临时换成空目录。
TEST(PluginLifecycleTest, DisabledDependencyBlocksDependents)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty()) << "沙箱插件的注册应成功";
    EXPECT_EQ(pm->count(), 0u) << "沙箱不加载任何东西";

    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", false));
    EXPECT_FALSE(pm->isPluginEnabled(u8"app_shell"));
    EXPECT_TRUE(pm->isPluginEnabled(u8"test_plugin"));

    EXPECT_FALSE(pm->loadAll()) << "依赖被禁用时必须整体报错";
    EXPECT_FALSE(pm->isLoaded(u8"app_shell"));
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));
    EXPECT_EQ(pm->count(), 0u);

    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    ASSERT_NE(shell, nullptr) << "被禁用的插件仍必须被发现并列出";
    EXPECT_EQ(shell->scope, vine::appfw::PluginScope::User);
    EXPECT_FALSE(shell->enabled);
    EXPECT_FALSE(shell->loaded);
    EXPECT_FALSE(shell->path.empty());
    EXPECT_TRUE(shell->info.version == u8"1.0.0");

    const auto* dependent = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(dependent, nullptr);
    EXPECT_TRUE(dependent->enabled);
    EXPECT_FALSE(dependent->loaded);
    EXPECT_EQ(dependent->info.dependencies.size(), 1u);

    // 恢复被依赖方：状态交给下一个用例（沙箱析构时才卸载）。
    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", true));
    EXPECT_TRUE(pm->isPluginEnabled(u8"app_shell"));
}

// 禁用的插件：不执行 load（不创建实例、不跑生命周期、不注册命令），但元数据
// 仍然被发现并列出，供插件信息页/管理器显示。
TEST(PluginLifecycleTest, DisabledPluginIsListedWithMetadataButNotLoaded)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    auto* cm = app->commandManager();
    ASSERT_NE(pm, nullptr);
    ASSERT_NE(cm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", false));
    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->isLoaded(u8"app_shell"));    // 其它插件不受影响
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin")); // 被禁用的不加载
    EXPECT_FALSE(pm->isPluginEnabled(u8"test_plugin"));

    const auto  entries   = pm->pluginEntries();
    const auto* dependent = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(dependent, nullptr) << "被禁用的插件仍必须被发现并列出";
    EXPECT_FALSE(dependent->enabled);
    EXPECT_FALSE(dependent->loaded);
    EXPECT_TRUE(dependent->info.display_name == u8"测试插件");
    EXPECT_TRUE(dependent->info.version == u8"1.0.0");
    EXPECT_FALSE(dependent->info.description.empty());
    EXPECT_FALSE(dependent->info.vendor.empty());
    EXPECT_FALSE(dependent->info.uuid.isNull()) << "插件身份由 V_DECLARE_PLUGIN 硬编码";
    EXPECT_FALSE(dependent->path.empty());
    EXPECT_FALSE(pm->libraryPath(u8"test_plugin").empty());

    // 未加载 = 生命周期没跑 = 没有注册任何命令。
    EXPECT_FALSE(cm->isRegistered(u8"test_hello"));

    // 也不能被算到别的插件头上：命令队列属于模块，命令的 owner 必须是注册它的
    // 那个插件。修复前这里会把 test_plugin 的命令全部算给 app_shell。
    const auto shell_commands = pm->commandInfosForPlugin(u8"app_shell");
    EXPECT_TRUE(std::none_of(shell_commands.begin(), shell_commands.end(), [](const vine::appfw::CommandInfo& info) {
        return info.name.rfind(u8"test_", 0) == 0;
    })) << "app_shell 名下不得出现 test_plugin 的命令";
    EXPECT_TRUE(pm->commandInfosForPlugin(u8"test_plugin").empty()) << "未加载的插件名下不得有任何命令";

    // 启用后重新加载等价于 "重启后" 的状态：命令才出现在注册表里。
    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));
    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->isLoaded(u8"test_plugin"));
    EXPECT_TRUE(cm->isRegistered(u8"test_hello"));

    const auto own_commands = pm->commandInfosForPlugin(u8"test_plugin");
    EXPECT_TRUE(std::any_of(own_commands.begin(), own_commands.end(), [](const vine::appfw::CommandInfo& info) {
        return info.name == u8"test_hello";
    })) << "加载后命令应归属于声明它的插件";
    for (const auto& info : own_commands) {
        EXPECT_TRUE(info.owner == u8"test_plugin");
    }
}

// 宿主跳过列表（setSkipList）：进程内的硬开关，优先于一切其它输入（包括用户
// 偏好）。被跳过的插件仍然被发现并列出（PluginEntry::skipped），只是不加载；
// 不持久化，所以移出列表后下一次 loadAll() 就能加载，不需要重启。
TEST(PluginLifecycleTest, SkippedPluginStaysVisibleAndIsNeverInstantiated)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    SkipListScope skip({ u8"test_plugin" });

    EXPECT_TRUE(pm->isSkipped(u8"test_plugin"));
    EXPECT_FALSE(pm->isPluginEnabled(u8"test_plugin")) << "跳过列表优先于用户偏好";
    EXPECT_TRUE(pm->isPluginEnabled(u8"app_shell"));

    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->isLoaded(u8"app_shell"));    // 未被跳过的插件不受影响
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin")); // 被跳过的插件不实例化
    // 不能用命令注册表判断：插件库只能在一个进程里实例化一次，命令一旦注册
    // 就不会撤销（宿主也不回收），所以已跑过的插件会留下痕迹。
    EXPECT_EQ(pm->plugin(u8"test_plugin"), nullptr);

    // 跳过不是 "看不见"：元数据、路径、跳过标记都还在，便于排查。
    const auto  entries = pm->pluginEntries();
    const auto* skipped = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(skipped, nullptr) << "被跳过的插件仍必须被发现并列出";
    EXPECT_TRUE(skipped->skipped);
    EXPECT_FALSE(skipped->enabled) << "跳过的插件不会加载，所以启用状态为 false";
    EXPECT_FALSE(skipped->loaded);
    EXPECT_FALSE(skipped->path.empty());
    EXPECT_TRUE(skipped->info.version == u8"1.0.0");
    EXPECT_FALSE(pm->libraryPath(u8"test_plugin").empty());

    // 显式 load() 也不得实例化被跳过的插件：名字即使能解析到库，也要先发现再拒绝
    // （跳过不是 "看不见"，而是 "列出来但永不运行"）。
    {
        BuiltInPluginDirectoryScope built_in(sandbox.directory());
        EXPECT_EQ(pm->load(u8"test_plugin"), nullptr) << "被跳过的插件即使给出名字也不得加载";
        EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));
        const auto  refreshed = pm->pluginEntries();
        const auto* after     = findPluginEntry(refreshed, u8"test_plugin");
        ASSERT_NE(after, nullptr);
        EXPECT_TRUE(after->skipped);
        EXPECT_FALSE(after->loaded);
        EXPECT_FALSE(after->path.empty()) << "显式 load() 也必须先发现，元数据才可见";
    }

    // 启用偏好照常写入配置，但跳过列表继续生效（同管理员策略的处理方式）。
    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));
    EXPECT_FALSE(pm->isPluginEnabled(u8"test_plugin"));

    // 移出跳过列表不需要重启：下一次 loadAll() 就能加载。
    vine::appfw::PluginManager::removeFromSkipList(u8"test_plugin");
    EXPECT_FALSE(pm->isSkipped(u8"test_plugin"));
    EXPECT_TRUE(pm->isPluginEnabled(u8"test_plugin"));
    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->isLoaded(u8"test_plugin"));
}

// 依赖被宿主跳过的插件：与 "依赖被禁用" 同样处理——整体报错且不加载，
// 被跳过的一方仍在发现列表里（可被 UI 解释为什么没运行）。
TEST(PluginLifecycleTest, SkippedDependencyBlocksDependents)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    SkipListScope skip({ u8"app_shell" });

    EXPECT_FALSE(pm->isPluginEnabled(u8"app_shell"));
    EXPECT_FALSE(pm->loadAll()) << "依赖被宿主跳过时必须整体报错";
    EXPECT_FALSE(pm->isLoaded(u8"app_shell"));
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));
    EXPECT_EQ(pm->count(), 0u);

    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    ASSERT_NE(shell, nullptr) << "被跳过的插件仍必须被发现并列出";
    EXPECT_TRUE(shell->skipped);
    EXPECT_FALSE(shell->enabled);
    EXPECT_FALSE(shell->loaded);

    const auto* dependent = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(dependent, nullptr);
    EXPECT_FALSE(dependent->skipped) << "依赖方自己没有被跳过";
    EXPECT_TRUE(dependent->enabled);
    EXPECT_FALSE(dependent->loaded);
}
// 禁用状态持久化在配置里（键 plugins.disabled，字符串数组），随 vine.json
// 一起重启后生效；重复禁用不产生重复项。
TEST(PluginLifecycleTest, DisableIsPersistedInConfig)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm  = app->pluginManager();
    auto* cfg = app->configManager();
    ASSERT_NE(pm, nullptr);
    ASSERT_NE(cfg, nullptr);

    const vine::String name(u8"no_such_plugin");
    const auto&        key = vine::appfw::PluginManager::disabledConfigKey();
    cfg->remove(key);

    EXPECT_TRUE(pm->setPluginEnabled(name, false));
    EXPECT_FALSE(pm->isPluginEnabled(name));
    auto disabled = cfg->getStringArray(key);
    ASSERT_EQ(disabled.size(), 1u);
    EXPECT_TRUE(disabled[0] == name);

    EXPECT_TRUE(pm->setPluginEnabled(name, false)); // 重复禁用
    EXPECT_EQ(cfg->getStringArray(key).size(), 1u);

    // 值确实写进了可持久化的 JSON（shutdown() 时写回配置文件）。
    EXPECT_NE(toUtf8(cfg->toJson()).find("no_such_plugin"), std::string::npos);

    EXPECT_TRUE(pm->setPluginEnabled(name, true));
    EXPECT_TRUE(pm->isPluginEnabled(name));
    EXPECT_TRUE(cfg->getStringArray(key).empty());

    cfg->remove(key); // 清理：不留空数组
}

// 目录布局：builder 默认把配置打开在 <用户数据>/<org>/<app>/config/<app>.json
// （日志同级在 logs/）；Application 构造时保证 org 非空。
TEST(PluginLifecycleTest, DefaultDataDirectoryLayout)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    const QString organization = QCoreApplication::organizationName();
    ASSERT_FALSE(organization.isEmpty()) << "Application 构造时应给出默认组织名";

    const auto dir = app->dataDirectory();
    EXPECT_TRUE(dir.is_absolute());
    EXPECT_EQ(dir.parent_path().filename(), std::filesystem::path(organization.toStdU16String()));
    EXPECT_EQ(dir.filename(), std::filesystem::path(QCoreApplication::applicationName().toStdU16String()));

    // 根目录必须来自平台标准位置，不能硬编码某个系统的路径：
    // Linux $XDG_DATA_HOME（默认 ~/.local/share）、Windows %LOCALAPPDATA%、
    // macOS ~/Library/Application Support。本进程开着 Qt 测试模式，两边取同一个值。
    const auto root = std::filesystem::path(
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdU16String());
    ASSERT_FALSE(root.empty());
    EXPECT_EQ(dir.parent_path().parent_path(), root);

    const auto file = app->defaultConfigFile();
    EXPECT_EQ(file.parent_path(), dir / "config");
    EXPECT_EQ(file.stem(), dir.filename()); // <app>.json
    EXPECT_EQ(file.extension().string(), ".json");

    // builder 默认启用默认配置文件；本进程从未 run()，所以它不会落盘。
    EXPECT_EQ(app->configFile(), file);
    EXPECT_FALSE(std::filesystem::exists(file));
}

// 配置持久化的入口：路径属于宿主（appfw 不决定配置放在哪），空路径 = 不持久化。
TEST(PluginLifecycleTest, ConfigFileIsOptIn)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    const auto path = std::filesystem::temp_directory_path() / "vine_test_config_never_created.json";
    std::filesystem::remove(path);

    EXPECT_TRUE(app->setConfigFile(path)) << "文件不存在不是错误：首次运行用默认值";
    EXPECT_EQ(app->configFile(), path);

    // 还原：测试进程不写配置文件。
    EXPECT_TRUE(app->setConfigFile({}));
    EXPECT_TRUE(app->configFile().empty());
    EXPECT_FALSE(std::filesystem::exists(path));
}

// 插件管理器对话框：列表来自"发现"而不是"加载"，因此被禁用的插件也在列表里
// （灰显 + 状态提示），详情页仍能显示它的元数据。
// 插件管理器对话框：列表来自 "发现" 而不是 "加载"，因此被禁用的插件也在列表里
// （灰显 + 状态提示），详情页仍能显示它的元数据、来源与可操作性。
TEST(PluginLifecycleTest, ManagerDialogListsDisabledPlugins)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    // 用户安装的插件可以被禁用（程序自带的不能，所以用沙箱的 User 作用域插件）。
    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());
    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", false));

    // 打开对话框（按本文件的约定，包装对象不释放）。
    auto* dialog = new guifw::PluginManagerDialog(pm);
    ASSERT_NE(dialog, nullptr);
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());

    // 被禁用的插件仍在发现列表里，且状态是 "已加载 + 已禁用"（本次继续运行）。
    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    ASSERT_NE(shell, nullptr);
    EXPECT_EQ(shell->scope, vine::appfw::PluginScope::User);
    EXPECT_FALSE(shell->enabled);
    EXPECT_TRUE(shell->loaded) << "禁用不改变本次运行状态：已加载的插件运行到退出";
    EXPECT_TRUE(shell->info.version == u8"1.0.0");

    // 详情页对 "未发现的名字" 与 "未加载的插件" 都不能崩。
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());

    // 恢复偏好：它写在共享的测试配置里，否则后面的用例会看到一个被禁用的 app_shell
    // （而且依赖它的 test_plugin 会因 "disabled dependency" 而整体加载失败）。
    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", true));
}

// 禁用/启用按钮只在偏好真能生效时才出现：程序自带的插件、以及被宿主跳过的插件都
// 不显示按钮（而不是显示一个点了没用的灰按钮）。
TEST(PluginLifecycleTest, ManagerDialogHidesToggleWhenItCannotTakeEffect)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    SkipListScope skip({ u8"test_plugin" });
    ASSERT_TRUE(pm->loadAll());

    auto* dialog = new guifw::PluginManagerDialog(pm);
    ASSERT_NE(dialog, nullptr);
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());

    auto* root = dialog->impl<QWidget>();
    ASSERT_NE(root, nullptr);
    auto* list = root->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr) << "插件列表";

    // 按钮按文本定位：它是唯一的 "禁用/启用插件（重启后生效）" 按钮。
    QPushButton* toggle = nullptr;
    for (QPushButton* button : root->findChildren<QPushButton*>()) {
        const QString text = button->text();
        if (text.startsWith(QStringLiteral("禁用插件")) || text.startsWith(QStringLiteral("启用插件"))) {
            toggle = button;
            break;
        }
    }
    ASSERT_NE(toggle, nullptr) << "找不到禁用/启用按钮";

    const auto selectPlugin = [&](const QString& name) {
        for (int row = 0; row < list->count(); ++row) {
            if (list->item(row)->data(Qt::UserRole).toString() == name) {
                list->setCurrentRow(row);
                return true;
            }
        }
        return false;
    };

    // 用户作用域的普通插件：按钮出现且可用。
    ASSERT_TRUE(selectPlugin(QStringLiteral("app_shell")));
    EXPECT_FALSE(toggle->isHidden());
    EXPECT_TRUE(toggle->isEnabled());

    // 被宿主跳过：动作无法生效 ⇒ 按钮重新隐藏（而不是变成灰按钮）。
    ASSERT_TRUE(selectPlugin(QStringLiteral("test_plugin")));
    EXPECT_TRUE(toggle->isHidden()) << "不能生效的动作不该显示成灰按钮";
}

// 插件信息与图标：PluginInfo 的 email/repo/icon 要一路送到 UI；声明了图标的插件用
// 自己那张，没声明的用宿主内置的默认图标（内联 SVG，无需外部资源文件）；
// 筛选框只保留匹配的行。
TEST(PluginLifecycleTest, ManagerDialogShowsMetadataAndIcons)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());
    ASSERT_TRUE(pm->loadAll());

    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    const auto* test    = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(shell, nullptr);
    ASSERT_NE(test, nullptr);

    // 声明了的字段原样带出来（宏参数顺序 = PluginInfo 字段顺序）。
    EXPECT_TRUE(test->info.email == u8"dev@vine.example");
    EXPECT_TRUE(test->info.repo == u8"https://github.com/vine/test_plugin");
    EXPECT_TRUE(test->info.icon.empty()) << "测试插件不声明图标，走默认图标分支";
    EXPECT_FALSE(shell->info.repo.empty());
    EXPECT_FALSE(shell->info.icon.empty()) << "应用外壳声明了自己的内联 SVG 图标";

    auto* dialog = new guifw::PluginManagerDialog(pm);
    ASSERT_NE(dialog, nullptr);
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());

    auto* root = dialog->impl<QWidget>();
    ASSERT_NE(root, nullptr);
    auto* list = root->findChild<QListWidget*>();
    ASSERT_NE(list, nullptr);
    ASSERT_GT(list->count(), 0);

    // 每行都有图标：SVG 由 QSvgRenderer 直接解析，不需要图片格式插件或资源文件。
    for (int row = 0; row < list->count(); ++row) {
        auto* item = list->item(row);
        ASSERT_NE(item, nullptr);
        EXPECT_FALSE(item->icon().isNull()) << "列表行缺少图标";
        EXPECT_FALSE(item->toolTip().isEmpty()) << "列表行缺少提示";
    }

    // 筛选：只剩匹配的行（名称/显示名/厂商/描述）。
    auto* filter = root->findChild<QLineEdit*>();
    ASSERT_NE(filter, nullptr);
    filter->setText(QStringLiteral("test_plugin"));

    int visible = 0;
    for (int row = 0; row < list->count(); ++row) {
        auto* item = list->item(row);
        if (item->isHidden()) {
            continue;
        }
        ++visible;
        EXPECT_TRUE(item->data(Qt::UserRole).toString() == QStringLiteral("test_plugin"));
    }
    EXPECT_EQ(visible, 1) << "筛选后应只剩 test_plugin";

    filter->clear();
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());
}
// 卸载顺序：按插件声明的依赖计算（依赖方先于被依赖方），与"加载列表里的位置"
// 无关——显式 load() 会把插件直接追加到末尾，可能排在它的依赖之前。
TEST(PluginLifecycleTest, UnloadOrderIsReverseDependencyOrder)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    // 真实集合：每个已加载插件恰好出现一次，且依赖方排在被依赖方之前。
    const auto entries = pm->pluginEntries();
    const auto order   = vine::appfw::PluginManager::unloadOrder(entries);

    std::size_t loaded = 0;
    for (const auto& entry : entries) {
        if (entry.loaded) {
            ++loaded;
        }
    }
    ASSERT_EQ(order.size(), loaded);

    const auto index_of = [&order](const vine::String& name) {
        const auto it = std::find(order.begin(), order.end(), name);
        return it == order.end() ? order.size() : static_cast<std::size_t>(it - order.begin());
    };
    for (const auto& entry : entries) {
        if (!entry.loaded) {
            continue;
        }
        for (const auto& dep : entry.info.dependencies) {
            if (std::find(order.begin(), order.end(), dep) != order.end()) {
                EXPECT_LT(index_of(entry.info.name), index_of(dep))
                    << "依赖方必须先卸载：" << toUtf8(entry.info.name);
            }
        }
    }

    // 合成集合：结论只由依赖声明决定，与传入顺序无关。
    const auto make_entry = [](const char8_t* name, std::vector<vine::String> deps, bool loaded_flag = true) {
        vine::appfw::PluginEntry entry;
        entry.info.name         = name;
        entry.info.dependencies = std::move(deps);
        entry.enabled           = true;
        entry.loaded            = loaded_flag;
        return entry;
    };

    // child 依赖 parent，但 child 排在前面（显式 load() 的典型结果）。
    const std::vector<vine::appfw::PluginEntry> dependent_first = {
        make_entry(u8"child", { u8"parent" }),
        make_entry(u8"parent", {}),
    };
    EXPECT_EQ(vine::appfw::PluginManager::unloadOrder(dependent_first),
              (std::vector<vine::String>{ u8"child", u8"parent" }));

    // 正常加载顺序也必须得到同样的结果（"列表倒序"在这里会给出错误答案）。
    const std::vector<vine::appfw::PluginEntry> dependency_first = {
        make_entry(u8"parent", {}),
        make_entry(u8"child", { u8"parent" }),
    };
    EXPECT_EQ(vine::appfw::PluginManager::unloadOrder(dependency_first),
              (std::vector<vine::String>{ u8"child", u8"parent" }));

    // 三层链 + 未加载项参与排序（未加载不算数）+ 已不在集合里的依赖不阻塞。
    const std::vector<vine::appfw::PluginEntry> chain = {
        make_entry(u8"grandchild", { u8"child" }),
        make_entry(u8"child", { u8"parent", u8"missing" }),
        make_entry(u8"parent", {}),
        make_entry(u8"not_loaded", { u8"grandchild" }, false),
    };
    EXPECT_EQ(vine::appfw::PluginManager::unloadOrder(chain),
              (std::vector<vine::String>{ u8"grandchild", u8"child", u8"parent" }));

    // 声明成环（只有显式 load() 能造出来）：不能死循环，回退为传入顺序。
    const std::vector<vine::appfw::PluginEntry> cycle = {
        make_entry(u8"a", { u8"b" }),
        make_entry(u8"b", { u8"a" }),
    };
    const auto cycle_order = vine::appfw::PluginManager::unloadOrder(cycle);
    ASSERT_EQ(cycle_order.size(), 2u);
    EXPECT_NE(std::find(cycle_order.begin(), cycle_order.end(), u8"a"), cycle_order.end());
    EXPECT_NE(std::find(cycle_order.begin(), cycle_order.end(), u8"b"), cycle_order.end());
}

// 每个插件一个独立的数据目录：<数据>/<org>/<app>/plugins/<插件名>，
// 首次调用才创建，键是插件名（插件被搬走/换位置也还是同一个目录）。
TEST(PluginLifecycleTest, PluginDataDirectoryIsPerPlugin)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    const auto root = app->pluginDataDirectory();
    EXPECT_EQ(root.parent_path(), app->dataDirectory());
    EXPECT_EQ(root.filename().string(), "plugins");

    vine::appfw::PluginLoadContext ctx(app, u8"my_plugin");
    const auto                    dir = ctx.ensureDataDirectory();
    EXPECT_EQ(dir, root / "my_plugin");
    EXPECT_TRUE(std::filesystem::is_directory(dir)) << "首次调用即创建";

    // 另一个插件拿到另一个目录，互不干扰。
    vine::appfw::PluginLoadContext other(app, u8"other_plugin");
    EXPECT_EQ(other.ensureDataDirectory(), root / "other_plugin");

    // 没有 Application / 没有插件名的上下文返回空路径，不崩。
    vine::appfw::PluginLoadContext headless(nullptr, u8"my_plugin");
    EXPECT_TRUE(headless.ensureDataDirectory().empty());
    vine::appfw::PluginLoadContext unnamed(app, {});
    EXPECT_TRUE(unnamed.ensureDataDirectory().empty());

    // 清理本用例写下的目录（Qt 测试模式已把用户数据目录重定向到临时区）。
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// 安装 = 在 installed.d 里写一个注册文件：位置可以是程序目录之外的库文件或目录，
// 没有 ConfigManager 之外的读-改-写，重复安装写同一个文件（幂等），卸载就是删文件。
TEST(PluginLifecycleTest, InstallWritesRegistrationFile)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    std::error_code ec;
    const auto      user_dir = std::filesystem::temp_directory_path() / "vine_user_plugins_test";
    std::filesystem::remove_all(user_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(user_dir, ec));

    // 注册目录属于应用的数据目录，路径与配置/日志同级。
    const auto registry = app->pluginRegistrationDirectory();
    EXPECT_EQ(registry.parent_path(), app->dataDirectory());
    EXPECT_EQ(registry.filename().string(), "installed.d");

    const auto id = pm->installPlugin(vine::String(user_dir.u8string()), vine::appfw::PluginScope::User);
    ASSERT_FALSE(id.empty()) << "注册成功应返回 id";
    EXPECT_TRUE(std::filesystem::is_directory(registry)) << "注册目录按需创建";

    auto registrations = pm->pluginRegistrations();
    ASSERT_EQ(registrations.size(), 1u);
    EXPECT_TRUE(registrations[0].id == id);
    EXPECT_TRUE(registrations[0].path == vine::String(user_dir.u8string()));
    EXPECT_EQ(registrations[0].scope, vine::appfw::PluginScope::User);
    EXPECT_TRUE(registrations[0].enabled);
    const std::string file_text(reinterpret_cast<const char*>(registrations[0].file.data()), registrations[0].file.size());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(std::u8string(file_text.begin(), file_text.end()))));
    EXPECT_NE(std::string(reinterpret_cast<const char*>(registrations[0].file.data()), registrations[0].file.size()).find("installed.d"),
              std::string::npos);

    // 重复安装同一位置：写同一个文件，仍然只有一条注册。
    EXPECT_TRUE(pm->installPlugin(vine::String(user_dir.u8string()), vine::appfw::PluginScope::User) == id);
    EXPECT_EQ(pm->pluginRegistrations().size(), 1u);

    // 相对路径必须落盘成绝对路径：注册文件是下次启动读的，那时的工作目录未必定是同一个。
    const auto relative_dir = std::filesystem::temp_directory_path() / "vine_relative_plugins_test";
    std::filesystem::remove_all(relative_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(relative_dir, ec));
    const auto previous_directory = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path(), ec);
    const auto relative_id = pm->installPlugin(u8"vine_relative_plugins_test", vine::appfw::PluginScope::User);
    std::filesystem::current_path(previous_directory, ec);
    ASSERT_FALSE(relative_id.empty()) << "相对路径也应能注册";
    const auto relative_entries = pm->pluginRegistrations();
    const auto* relative_entry   = findRegistration(relative_entries, relative_id);
    ASSERT_NE(relative_entry, nullptr);
    const std::filesystem::path stored(std::u8string_view(relative_entry->path.data(), relative_entry->path.size()));
    EXPECT_TRUE(stored.is_absolute()) << "注册文件里必须是绝对路径";
    EXPECT_EQ(stored, std::filesystem::weakly_canonical(relative_dir, ec));
    EXPECT_TRUE(pm->uninstallPlugin(relative_id, vine::appfw::PluginScope::User));
    std::filesystem::remove_all(relative_dir, ec);

    // 拒绝的输入：空路径、不存在的路径、BuiltIn 作用域。
    EXPECT_TRUE(pm->installPlugin({}, vine::appfw::PluginScope::User).empty());
    EXPECT_TRUE(pm->installPlugin(u8"/no/such/plugin/location.so", vine::appfw::PluginScope::User).empty());
    EXPECT_TRUE(pm->installPlugin(vine::String(user_dir.u8string()), vine::appfw::PluginScope::BuiltIn).empty());
    EXPECT_EQ(pm->pluginRegistrations().size(), 1u);

    EXPECT_TRUE(pm->uninstallPlugin(id, vine::appfw::PluginScope::User));
    EXPECT_TRUE(pm->pluginRegistrations().empty());
    EXPECT_FALSE(pm->uninstallPlugin(id, vine::appfw::PluginScope::User)) << "已删除的注册再删应返回 false";

    std::filesystem::remove_all(user_dir, ec);
}

// 系统级注册目录只读地参与合并：每个系统数据根下同一套 <org>/<app>/installed.d
// 布局，且不与用户目录重复。
TEST(PluginLifecycleTest, SystemRegistrationDirectoriesAreListed)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    const auto user_registry = app->pluginRegistrationDirectory();
    for (const auto& directory : app->allUsersPluginRegistrationDirectories()) {
        EXPECT_NE(directory, user_registry) << "系统目录不能就是用户目录";
        EXPECT_EQ(directory.filename().string(), "installed.d");
        EXPECT_EQ(directory.parent_path().filename(), app->dataDirectory().filename());
        EXPECT_EQ(directory.parent_path().parent_path().filename(), app->dataDirectory().parent_path().filename());
    }
}

// 手写注册文件（安装器/脚本的路径）：key=value，path 必需，enabled = false 是
// "对所有用户禁用"的管理员策略，优先级高于用户自己的启用。
TEST(PluginLifecycleTest, HandWrittenRegistrationCanDisableForAllUsers)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    const auto source = findPluginLibrary("test_plugin");
    if (source.empty()) {
        GTEST_SKIP() << "test_plugin library not built";
    }

    // 同一个插件库在一个进程里只能被加载一次（vine 类型注册是进程级且不可撤销），
    // 所以这里复用沙箱目录里那一份拷贝，而不是再拷一份。
    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    const auto copy = sandbox.directory() / source.filename();
    ASSERT_TRUE(std::filesystem::exists(copy));

    std::error_code ec;
    const auto      registry = app->pluginRegistrationDirectory();
    std::filesystem::create_directories(registry, ec);
    ASSERT_TRUE(std::filesystem::is_directory(registry));

    const auto file = registry / "manual.plugin";
    {
        std::ofstream stream(file);
        ASSERT_TRUE(static_cast<bool>(stream));
        stream << "# hand written\n";
        stream << "path = " << copy.string() << "\n";
        stream << "name = test_plugin\n";
        stream << "enabled = false\n";
    }

    const auto  registrations = pm->pluginRegistrations();
    const auto* manual        = findRegistration(registrations, u8"manual");
    ASSERT_NE(manual, nullptr) << "手写的注册文件必须被读到";
    EXPECT_FALSE(manual->enabled);
    EXPECT_TRUE(manual->name == u8"test_plugin");
    EXPECT_NE(std::string(reinterpret_cast<const char*>(manual->path.data()), manual->path.size()).find("test_plugind"),
              std::string::npos);

    // 策略必须在没有跑过 loadAll() 的进程里也生效：一次显式 load()（对话框的试用加载）
    // 不得把管理员对所有用户禁用的插件重新拉起来。
    EXPECT_EQ(pm->load(vine::String(copy.u8string())), nullptr) << "策略禁用的插件不得被显式加载";
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));

    ASSERT_TRUE(pm->loadAll());

    const auto  entries = pm->pluginEntries();
    const auto* entry   = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(entry, nullptr) << "被策略禁用的插件仍要能被发现（列表里可见）";
    EXPECT_EQ(entry->scope, vine::appfw::PluginScope::User);
    EXPECT_FALSE(entry->enabled) << "注册文件里的 enabled = false 是策略";
    EXPECT_FALSE(entry->loaded);

    // 用户可以存下"启用"偏好，但策略仍然赢。
    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));
    EXPECT_FALSE(pm->isPluginEnabled(u8"test_plugin")) << "管理员策略不能被用户开关冲掉";

    std::filesystem::remove(file, ec);
}

// 已注册的位置会参与发现，并且同一插件被多处提供时只出现一次（按插件名去重，
// 只实例化先找到的那一份）。
TEST(PluginLifecycleTest, InstalledLocationIsDiscoveredAndDeduplicated)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    const auto source = findPluginLibrary("test_plugin");
    if (source.empty()) {
        GTEST_SKIP() << "test_plugin library not built";
    }

    // 源 1：沙箱目录（同时把程序目录换成空目录，避免加载程序自带的同名插件）。
    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());
    const auto first_copy = sandbox.directory() / source.filename();
    ASSERT_TRUE(std::filesystem::exists(first_copy));

    // 源 2：另一个目录里的同一插件。它只会被发现，不会被实例化。
    std::error_code ec;
    const auto      second_dir = std::filesystem::temp_directory_path() / "vine_test_plugins_second";
    std::filesystem::remove_all(second_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(second_dir, ec));
    ASSERT_TRUE(std::filesystem::copy_file(source, second_dir / source.filename(),
                                           std::filesystem::copy_options::overwrite_existing, ec));
    const auto second_id = pm->installPlugin(vine::String(second_dir.u8string()), vine::appfw::PluginScope::User);
    ASSERT_FALSE(second_id.empty());

    // 只观察发现与去重，不加载：一个插件库在一个进程里只能创建一次实例（vine
    // 类型注册是进程级且不可撤销），前面的用例已经把这份拷贝创建过了，所以这里的
    // loadAll() 成功与否不是本用例的断言对象。
    static_cast<void>(pm->loadAll());

    const auto  entries = pm->pluginEntries();
    const auto  count   = std::count_if(entries.begin(), entries.end(), [](const vine::appfw::PluginEntry& e) {
        return e.info.name == u8"test_plugin";
    });
    EXPECT_EQ(count, 1) << "同一插件被多处提供时只能出现一次";

    const auto* entry = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->scope, vine::appfw::PluginScope::User);
    // 注册按文件名的顺序读取，所以先发现哪一个位置由注册文件名决定；关键是只有一份
    // 被发现，另一份不会产生第二条记录。
    EXPECT_TRUE(entry->path == first_copy || entry->path == second_dir / source.filename());

    EXPECT_TRUE(pm->uninstallPlugin(second_id, vine::appfw::PluginScope::User));
    std::filesystem::remove_all(second_dir, ec);
}
// 关闭时的反初始化：unload() 按依赖反序调用（依赖方先于被依赖方），
// 卸载后仍保留元数据（插件信息可见），并且可以重复调用。
TEST(PluginLifecycleTest, UnloadAllKeepsMetadataAndIsIdempotent)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    // 本套件里的插件都从沙箱（用户注册）加载，前面的用例结束时沙箱析构会卸载它们；
    // 这里验证"卸载后仍保留元数据"与幂等，两支都成立。
    const auto loaded_names = pm->names();

    EXPECT_TRUE(pm->unloadAll());
    EXPECT_EQ(pm->count(), 0u);
    EXPECT_TRUE(pm->names().empty());
    EXPECT_TRUE(pm->unloadAll()) << "再次卸载是幂等的";

    const auto entries = pm->pluginEntries();
    EXPECT_FALSE(entries.empty()) << "卸载后仍保留元数据（插件列表可见）";
    for (const auto& entry : entries) {
        EXPECT_FALSE(entry.loaded);
        EXPECT_FALSE(entry.path.empty());
        EXPECT_FALSE(entry.info.uuid.isNull());
    }
    for (const auto& name : loaded_names) {
        EXPECT_EQ(pm->plugin(name), nullptr) << "卸载后不应还能拿到实例";
    }
}

// 加载失败必须是原子的：插件在 load() 里抛异常时，本次 loadAll() 已经创建的实例
// 要逆序卸载掉，管理器不留下任何自己不知道的实例——否则重试会在同一个半初始化的
// 实例上重跑生命周期（插件库一个进程只能创建一个实例）。
// test_plugin 的测试钩子（plugins.test_plugin.fail_load）把它的 load() 变成抛异常。
TEST(PluginLifecycleTest, LoadAllRollsBackWhenPluginThrows)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm  = app->pluginManager();
    auto* cfg = app->configManager();
    ASSERT_NE(pm, nullptr);
    ASSERT_NE(cfg, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    // 顺序无关：前面的用例可能在共享配置里留下禁用偏好，而本用例只想验证"抛异常
    // 会回滚"，所以先把两个插件显式启用。
    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", true));
    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));

    const vine::String key(u8"plugins.test_plugin.fail_load");
    cfg->setBool(key, true);

    // 失败原因不作断言：在单独运行时是测试钩子把 test_plugin 的 load() 变成抛异常，
    // 在完整套件里也可能是"一个插件库一个进程只能实例化一次"导致的重复注册失败
    // （前面的用例已经从另一份拷贝创建过同一个插件）。两种情况走的是同一条回滚路径，
    // 所以这里只断言"报失败 + 不留下任何本次创建的实例"。
    EXPECT_FALSE(pm->loadAll()) << "插件抛异常时 loadAll() 必须报告失败";
    EXPECT_EQ(pm->count(), 0u) << "本次创建的实例必须全部回滚";
    EXPECT_EQ(pm->plugin(u8"app_shell"), nullptr) << "已创建的先序插件也要卸载";
    EXPECT_EQ(pm->plugin(u8"test_plugin"), nullptr);

    // 发现与加载分离：失败后元数据与路径仍可查询。
    const auto  entries = pm->pluginEntries();
    ASSERT_NE(findPluginEntry(entries, u8"test_plugin"), nullptr);
    EXPECT_FALSE(pm->libraryPath(u8"test_plugin").empty());

    cfg->remove(key);
}

// 注册 id 会变成 installed.d/<id>.plugin 的文件名，所以不能带路径分隔符或 "："，
// 也不能为空：否则可能写到注册目录之外，或者写出一个永远不会被扫描的文件。
TEST(PluginLifecycleTest, InstallRejectsUnusableRegistrationId)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell" });
    ASSERT_FALSE(sandbox.registration().empty());
    const auto baseline = pm->pluginRegistrations().size();

    std::error_code ec;

    // 以分隔符结尾的目录：取目录名作为 id，而不是因为 filename() 为空
    // （directory / ""）就拼出一个注册目录之外的文件。
    const std::filesystem::path trailing = std::filesystem::temp_directory_path() / "vine_plugins_trailing";
    std::filesystem::create_directories(trailing, ec);
    const vine::String id = pm->installPlugin(vine::String((trailing / "").u8string()), vine::appfw::PluginScope::User);
    EXPECT_TRUE(id == u8"vine_plugins_trailing") << "应当取目录名，而不是写出畸形文件名";
    if (!id.empty()) {
        EXPECT_TRUE(pm->uninstallPlugin(id, vine::appfw::PluginScope::User));
    }
    std::filesystem::remove_all(trailing, ec);
    EXPECT_EQ(pm->pluginRegistrations().size(), baseline) << "临时注册应当被清掉";

    // 名字含 ':'（Windows 上是盘符/数据流分隔符）：拒绝，且不留文件。
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "vine_bad:name";
    std::filesystem::create_directories(bad, ec);
    EXPECT_TRUE(pm->installPlugin(vine::String(bad.u8string()), vine::appfw::PluginScope::User).empty())
        << "含 ':' 的名字必须被拒绝";
    EXPECT_FALSE(pm->uninstallPlugin(vine::String(u8"bad/name"), vine::appfw::PluginScope::User))
        << "带分隔符的 id 也必须被拒绝";
    EXPECT_EQ(pm->pluginRegistrations().size(), baseline) << "被拒绝的注册不应留下文件";
    std::filesystem::remove_all(bad, ec);
}

// ABI 握手（PluginAbi）：宿主在读取插件声明的任何东西之前，先问它是用什么 SDK 编的。
// 两个夹具库分别模拟"比握手更旧"（没有入口点）与"比本宿主更新"（ABI 号对不上）的插件，
// 两者都必须被拒绝，而不是按当前布局去读它们的 PluginInfo —— 那正是会静默读到垃圾、
// 然后在别处崩掉的那条路径。夹具路径由 CMake 注入（tests/test_gui/CMakeLists.txt）。
TEST(PluginLifecycleTest, PluginsWithoutCompatibleAbiAreRefused)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    const std::filesystem::path legacy(VINE_LEGACY_ABI_PLUGIN);
    const std::filesystem::path future(VINE_FUTURE_ABI_PLUGIN);
    ASSERT_TRUE(std::filesystem::exists(legacy)) << legacy;
    ASSERT_TRUE(std::filesystem::exists(future)) << future;

    EXPECT_EQ(pm->load(vine::String(legacy.u8string())), nullptr) << "没有 ABI 握手的库必须被拒绝";
    EXPECT_EQ(pm->load(vine::String(future.u8string())), nullptr) << "ABI 号对不上的库必须被拒绝";
    EXPECT_FALSE(pm->isLoaded(u8"legacy_plugin"));
    EXPECT_FALSE(pm->isLoaded(u8"future_plugin"));

    // 被拒绝的库连"发现"都不做：宿主读不到可以信任的元数据，也就不该把它列进列表
    // （列表里的每一条都保证是能安全读取的）。日志里给出两边的 ABI 与框架版本，够定位。
    const auto entries = pm->pluginEntries();
    EXPECT_EQ(findPluginEntry(entries, u8"legacy_plugin"), nullptr);
    EXPECT_EQ(findPluginEntry(entries, u8"future_plugin"), nullptr);

    // 安装同样被拒：注册一个用不了的库，只会在以后每次启动多一条警告。
    EXPECT_TRUE(pm->installPlugin(vine::String(legacy.u8string())).empty());
}

// 握手带回来的"构建时框架版本"要一路走到 PluginEntry，插件管理器对话框的"信息"页才有
// 东西可显示。这里与编译期宏比较，能证明整条链路（V_DECLARE_PLUGIN → PluginAbi →
// queryLibrary → PluginEntry）没有丢值，而不只是"编译得过"。
TEST(PluginLifecycleTest, PluginEntryReportsTheFrameworkItWasBuiltWith)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);

    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell" });
    ASSERT_FALSE(sandbox.registration().empty());
    ASSERT_TRUE(pm->loadAll());

    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    ASSERT_NE(shell, nullptr);
    // V_APPFW_VERSION 是构建注入的窄字符串字面量（CMake 的 PROJECT_VERSION），
    // String 存 UTF-8 字节，所以按字节比较。
    const vine::String expected(std::u8string_view(reinterpret_cast<const char8_t*>(V_APPFW_VERSION)));
    EXPECT_TRUE(shell->framework_version == expected)
        << "插件报告的应当是它编译时的框架版本（" V_APPFW_VERSION "）";
}

// 依赖被禁用时，依赖它的插件也不会加载——直接依赖那层已由
// DisabledDependencyBlocksDependents 钉住，这里补上“传递”一层（chain_plugin →
// test_plugin → app_shell），以及与之相反的另一面：显式 load() 有意不解析依赖，
// 被依赖方缺位时只记一条警告就照常加载（见头文件对 load() 的说明）。
TEST(PluginLifecycleTest, DisabledDependencyBlocksDependentsTransitively)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);
    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    // 库路径要在沙箱**之前**取：沙箱会把内置目录换成空目录。
    const auto built_in_test = findPluginLibrary("test_plugin");
    ASSERT_FALSE(built_in_test.empty());

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    // 第三个夹具（依赖 test_plugin）摆进沙箱目录，它才在扫描范围内。
    std::error_code             ec;
    const std::filesystem::path chain_lib(VINE_CHAIN_PLUGIN);
    std::filesystem::copy_file(chain_lib, sandbox.directory() / chain_lib.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(std::filesystem::exists(sandbox.directory() / chain_lib.filename()));

    ASSERT_TRUE(pm->setPluginEnabled(u8"app_shell", false));
    EXPECT_FALSE(pm->loadAll()) << "有未满足依赖时 loadAll() 必须报失败";
    EXPECT_FALSE(pm->isLoaded(u8"app_shell"));
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));
    EXPECT_FALSE(pm->isLoaded(u8"chain_plugin")) << "传递依赖同样不得加载";

    // 三者都被发现并列入（“不加载”不等于“看不见”）；链尾的 enabled 仍为 true：
    // 它自己没被禁用，只是依赖不可用。
    const auto  entries = pm->pluginEntries();
    const auto* shell   = findPluginEntry(entries, u8"app_shell");
    const auto* test    = findPluginEntry(entries, u8"test_plugin");
    const auto* chain   = findPluginEntry(entries, u8"chain_plugin");
    ASSERT_NE(shell, nullptr);
    ASSERT_NE(test, nullptr);
    ASSERT_NE(chain, nullptr);
    EXPECT_FALSE(shell->enabled);
    EXPECT_TRUE(test->enabled);
    EXPECT_TRUE(chain->enabled);
    EXPECT_FALSE(chain->loaded);
    EXPECT_EQ(chain->info.dependencies.size(), 1u);

    // 显式 load() 不解析依赖：照常加载，只在日志里提醒依赖没在跑。
    const auto test_lib = sandbox.directory() / built_in_test.filename();
    ASSERT_TRUE(std::filesystem::exists(test_lib));
    EXPECT_NE(pm->load(vine::String(test_lib.u8string())), nullptr) << "显式 load() 不解析依赖";
    EXPECT_TRUE(pm->isLoaded(u8"test_plugin"));

    // 把偏好恢复成原样，不给后面的用例留状态。
    EXPECT_TRUE(pm->setPluginEnabled(u8"app_shell", true));
}

// 依赖不可满足只剪掉受影响的那些插件，其余照常加载：一个第三方插件的坏依赖不该
// 让应用连自带的外壳都没有（loadAll() 仍返回 false，宿主据此记录/告警）。
TEST(PluginLifecycleTest, UnresolvablePluginsAreSkippedWhileTheRestLoads)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);
    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell", "test_plugin" });
    ASSERT_FALSE(sandbox.registration().empty());

    std::error_code             ec;
    const std::filesystem::path chain_lib(VINE_CHAIN_PLUGIN);
    std::filesystem::copy_file(chain_lib, sandbox.directory() / chain_lib.filename(),
                               std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_TRUE(std::filesystem::exists(sandbox.directory() / chain_lib.filename()));

    // 禁用的是链中段：app_shell 不受影响，只有依赖它的那一截被剪掉。
    ASSERT_TRUE(pm->setPluginEnabled(u8"test_plugin", false));
    EXPECT_FALSE(pm->loadAll()) << "有插件被剪掉时仍要报 false";
    EXPECT_TRUE(pm->isLoaded(u8"app_shell")) << "与不可加载的那一簇无关的插件必须照常加载";
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));
    EXPECT_FALSE(pm->isLoaded(u8"chain_plugin")) << "依赖不可加载的插件被一并剪掉";

    const auto  entries = pm->pluginEntries();
    const auto* chain   = findPluginEntry(entries, u8"chain_plugin");
    ASSERT_NE(chain, nullptr);
    EXPECT_TRUE(chain->enabled) << "它自己没被禁用，只是依赖不可用";
    EXPECT_FALSE(chain->loaded);

    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));
}

// 声明成环（只有手写插件造得出来）：环那一簇被剪掉、无关插件照常加载，
// loadAll() 报 false。修复前这种情况是整批放弃（自带外壳也不加载）。
TEST(PluginLifecycleTest, DeclaredDependencyCycleIsPrunedNotFatal)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* pm = app->pluginManager();
    ASSERT_NE(pm, nullptr);
    if (!builtInPluginDirectoryReady()) {
        GTEST_SKIP() << "plugin directory not built";
    }

    UserPluginSandbox sandbox(pm, { "app_shell" });
    ASSERT_FALSE(sandbox.registration().empty());

    std::error_code ec;
    for (const char* fixture : { VINE_LOOP_A_PLUGIN, VINE_LOOP_B_PLUGIN }) {
        const std::filesystem::path library(fixture);
        std::filesystem::copy_file(library, sandbox.directory() / library.filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_TRUE(std::filesystem::exists(sandbox.directory() / library.filename()));
    }

    EXPECT_FALSE(pm->loadAll()) << "环里的插件被剪掉，仍要报 false";
    EXPECT_TRUE(pm->isLoaded(u8"app_shell")) << "无关插件不受影响";
    EXPECT_FALSE(pm->isLoaded(u8"loop_a"));
    EXPECT_FALSE(pm->isLoaded(u8"loop_b"));
}

// unloadOrder() 是公开的纯函数：声明成环的输入也必须终止（顺序无解时退回发现
// 顺序），并且只排已加载的插件。
TEST(PluginLifecycleTest, UnloadOrderTerminatesOnACycle)
{
    using vine::appfw::PluginEntry;
    using vine::appfw::PluginManager;

    auto entry = [](const char8_t* name, std::initializer_list<const char8_t*> dependencies, bool loaded = true) {
        PluginEntry e;
        e.info.name = name;
        for (const auto* dependency : dependencies) {
            e.info.dependencies.push_back(dependency);
        }
        e.loaded = loaded;
        return e;
    };

    // 依赖链：被依赖者最后卸载
    const std::vector<PluginEntry> chain{ entry(u8"base", {}), entry(u8"mid", { u8"base" }), entry(u8"top", { u8"mid" }) };
    const auto                     chain_order = PluginManager::unloadOrder(chain);
    ASSERT_EQ(chain_order.size(), 3u);
    EXPECT_TRUE(chain_order[0] == u8"top");
    EXPECT_TRUE(chain_order[1] == u8"mid");
    EXPECT_TRUE(chain_order[2] == u8"base");

    // 环 + 无关项：不挂死，且每个已加载的插件都被排进去
    const std::vector<PluginEntry> cyclic{ entry(u8"a", { u8"b" }), entry(u8"b", { u8"a" }), entry(u8"c", {}), entry(u8"off", {}, false) };
    const auto                     cyclic_order = PluginManager::unloadOrder(cyclic);
    ASSERT_EQ(cyclic_order.size(), 3u);
    EXPECT_TRUE(cyclic_order[0] == u8"c") << "无依赖的插件先卸载";
    EXPECT_TRUE(std::find(cyclic_order.begin(), cyclic_order.end(), vine::String(u8"a")) != cyclic_order.end());
    EXPECT_TRUE(std::find(cyclic_order.begin(), cyclic_order.end(), vine::String(u8"b")) != cyclic_order.end());
}

namespace
{

// 卸载必须发生在 GuiApplication 销毁之前（此时总线、管理器与窗口都还活着，
// 插件的 unload() 才能安全收尾）。环境按注册顺序反向拆卸，本环境在
// g_gui_env 之后注册，因此先于它被调用。
class PluginLifecycleEnv : public ::testing::Environment {
  public:
    void TearDown() override
    {
        if (auto* app = GuiEnv::app.get(); app != nullptr) {
            // Ignored on purpose: the test process is going down and a plugin that
            // threw while unloading has already been logged.
            static_cast<void>(app->pluginManager()->unloadAll());
        }
    }
};

} // namespace

::testing::Environment* const g_plugin_env = ::testing::AddGlobalTestEnvironment(new PluginLifecycleEnv());

// ════════════════════════════════════════════════════════════════════════════
// 第五轮审计：等用户输入的命令与"排空"的边界
// ════════════════════════════════════════════════════════════════════════════
namespace
{

// 停在等待用户输入上的命令：直到交互被取消才会返回。
class InteractiveWaitCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"interactiveWait"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"waits for user input"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* io  = app ? app->userIO() : nullptr;
        if (io == nullptr) {
            co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Failed);
        }

        s_parked.store(true);
        const auto input = co_await io->getStringAsync(u8"输入点什么> ");
        s_parked.store(false);
        co_return vine::appfw::CommandResult(
            input.has_value() ? vine::appfw::CommandStatus::Success : vine::appfw::CommandStatus::Cancelled);
    }

    inline static std::atomic<bool> s_parked{ false };
};

V_OBJECT_META_IMPL(InteractiveWaitCommand, vine::appfw::Command)

// 在命令内部调用"取消并排空"。
class DrainFromInsideCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"drainFromInside"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"drains from inside itself"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        s_drained = cm != nullptr && cm->cancelAllAndWait(std::chrono::milliseconds(50));
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static bool s_drained{ true };
};

V_OBJECT_META_IMPL(DrainFromInsideCommand, vine::appfw::Command)

// 不配合取消、时长可调的睡眠命令：用来把"排他命令正在等待旧链"这个窗口撑开。
class UncooperativeSleepForCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    explicit UncooperativeSleepForCommand(vine::String name, std::chrono::milliseconds duration)
      : name_(std::move(name))
      , duration_(duration)
    {}

    vine::String name() const override { return name_; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"ignores cancellation"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        s_running.store(true);
        co_await vine::async::sleepFor(duration_);
        s_running.store(false);
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_running{ false };

  private:
    vine::String              name_;
    std::chrono::milliseconds duration_;
};

V_OBJECT_META_IMPL(UncooperativeSleepForCommand, vine::appfw::Command)

} // namespace

// 等用户输入的链自己排不掉：读操作没有取消令牌，管理器无从唤醒它（旧实现里
// Application::shutdown() 只能白等满上界，然后带着活链销毁管理器）。
// 宿主必须先取消这次交互（UserIO::cancelPendingInput()）再排空。
TEST_F(GuiTest, CommandManager_PendingUserInputBlocksDrainUntilCancelled)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);
    auto* io = app->userIO();
    ASSERT_NE(io, nullptr);

    const auto name = vine::String(u8"interactiveWait");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<InteractiveWaitCommand>(name));
    cm->clearHistory();
    InteractiveWaitCommand::s_parked = false;

    cm->executeDetached(name);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!InteractiveWaitCommand::s_parked.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(InteractiveWaitCommand::s_parked.load());

    // 只取消链不够：命令停在读上，取消令牌传不进这次读。
    EXPECT_FALSE(cm->cancelAllAndWait(std::chrono::milliseconds(100)));
    EXPECT_TRUE(InteractiveWaitCommand::s_parked.load());

    // 取消挂起的交互后，命令以 Cancelled 收尾，排空成功。
    io->cancelPendingInput();
    EXPECT_TRUE(cm->cancelAllAndWait(std::chrono::seconds(5)));
    EXPECT_FALSE(InteractiveWaitCommand::s_parked.load());
    EXPECT_EQ(cm->runningCount(), 0);
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->result.status(), vine::appfw::CommandStatus::Cancelled);

    // 没有挂起交互时再取消一次是安全的（幂等）。
    io->cancelPendingInput();

    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 命令不能在自己身上"取消并排空"：它自己所在的链正是要排空的对象，只有它返回
// 之后那条链才会结束。文档里给出了正确做法（Application::quit() + 宿主收尾），
// 这里把"必定失败"这个事实钉住，避免被误当成可用路径。
TEST_F(GuiTest, CommandManager_CancelAllAndWaitFromInsideCommandCannotSucceed)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"drainFromInside");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DrainFromInsideCommand>(name));
    cm->clearHistory();
    DrainFromInsideCommand::s_drained = true;

    const auto result = cm->executeCommand(name);
    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);
    EXPECT_FALSE(DrainFromInsideCommand::s_drained) << "命令无法排空自己所在的链";
    EXPECT_EQ(cm->runningCount(), 0) << "命令返回后链仍然正常收尾";

    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 等待接管的排他命令本身也必须能被"停一切"打断：旧实现里 cancelAll() 够不到它
// （它还没建链、不在活跃链注册表里），只能等满 2s 的排空上界才失败。
TEST_F(GuiTest, CommandManager_CancelAllAbortsWaitingTakeOver)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto victim = vine::String(u8"uncooperativeTimed");
    const auto taker  = vine::String(u8"takeover");
    cm->unregisterCommand(victim);
    cm->unregisterCommand(taker);
    ASSERT_TRUE(cm->registerCommand(UncooperativeSleepForCommand::desc(), victim, [victim] {
        return new UncooperativeSleepForCommand(victim, std::chrono::milliseconds(800));
    }));
    ASSERT_TRUE(cm->registerCommand<ExclusiveTakeOverCommand>(taker));
    ExclusiveTakeOverCommand::s_ran            = false;
    UncooperativeSleepForCommand::s_running    = false;
    cm->clearHistory();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 旧链在跑，且它无视取消 ⇒ 排他命令会停在"等待旧链收尾"上（上界 2s）。
    auto                       victim_task = cm->executeCommandAsync(victim);
    vine::appfw::CommandResult victim_result;
    std::thread victim_runner([&] { victim_result = vine::async::syncWait(std::move(victim_task)); });
    while (!UncooperativeSleepForCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(UncooperativeSleepForCommand::s_running.load());

    auto                       taker_task = cm->executeCommandAsync(taker);
    vine::appfw::CommandResult taker_result;
    std::thread taker_runner([&] { taker_result = vine::async::syncWait(std::move(taker_task)); });

    // 让排他命令确实进入等待：它自己不执行，没有别的可观察点。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto start = std::chrono::steady_clock::now();
    cm->cancelAll();
    taker_runner.join();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // 被打断：立刻以 Cancelled 结束（而不是等满 2s 后 Failed），命令体从未执行。
    EXPECT_EQ(taker_result.status(), vine::appfw::CommandStatus::Cancelled);
    EXPECT_FALSE(ExclusiveTakeOverCommand::s_ran);
    EXPECT_LT(elapsed, 1000) << "cancelAll() 应立即结束等待中的接管";

    while (UncooperativeSleepForCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    victim_runner.join();
    EXPECT_EQ(victim_result.status(), vine::appfw::CommandStatus::Success) << "不配合取消的旧链自己跑完";

    cm->clearHistory();
    cm->unregisterCommand(victim);
    cm->unregisterCommand(taker);
}

// ════════════════════════════════════════════════════════════════════════════
// 第六轮审计：重入 + 多线程
// ════════════════════════════════════════════════════════════════════════════
namespace
{

// 带名字的极简命令：实例由探针工厂返回。
class ReentrancyProbeCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    explicit ReentrancyProbeCommand(vine::String name)
      : name_(std::move(name))
    {}

    vine::String name() const override { return name_; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"reentrancy probe"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::None; }
    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        s_runs.fetch_add(1);
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<int> s_runs{ 0 };

  private:
    vine::String name_;
};

V_OBJECT_META_IMPL(ReentrancyProbeCommand, vine::appfw::Command)

// 慢排他命令：记录自身并发数。
class SlowExclusiveProbeCommand : public vine::appfw::Command {
    V_OBJECT_META_DECL;

  public:
    vine::String name() const override { return u8"exclusiveSlow"; }
    vine::String group() const override { return u8"Test"; }
    vine::String description() const override { return u8"slow exclusive"; }
    vine::appfw::CommandFlags flags() const override { return vine::appfw::CommandFlags::Exclusive; }

    vine::async::Task<vine::appfw::CommandResult> execute(vine::appfw::CommandExecutionContext*) override
    {
        const int now  = s_in_flight.fetch_add(1) + 1;
        int       prev = s_max.load();
        while (prev < now && !s_max.compare_exchange_weak(prev, now)) {
        }
        co_await vine::async::sleepFor(std::chrono::milliseconds(200));
        s_in_flight.fetch_sub(1);
        co_return vine::appfw::CommandResult(vine::appfw::CommandStatus::Success);
    }

    inline static std::atomic<int> s_in_flight{ 0 };
    inline static std::atomic<int> s_max{ 0 };
};

V_OBJECT_META_IMPL(SlowExclusiveProbeCommand, vine::appfw::Command)

} // namespace

// 重入 1：工厂在注册过程中重入管理器的注册表（注册别的名字、列举、注销自己）。
// 工厂是在 registry_mutex 之外调用的，所以这些都不得死锁。
TEST_F(GuiTest, CommandManager_ReentrantFactoryIsSafe)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name  = vine::String(u8"reentrantFactory");
    const auto other = vine::String(u8"reentrantFactoryOther");
    cm->unregisterCommand(name);
    cm->unregisterCommand(other);
    ReentrancyProbeCommand::s_runs = 0;

    bool inside_registered_other = false;
    bool inside_infos_ok         = false;
    bool inside_unregistered_self = false;

    const bool registered = cm->registerCommand(ReentrancyProbeCommand::desc(), name, [&]() -> vine::appfw::Command* {
        inside_registered_other = cm->registerCommand(ReentrancyProbeCommand::desc(), other, [other] {
            return new ReentrancyProbeCommand(other);
        });
        inside_infos_ok          = !cm->commandInfos().empty();
        inside_unregistered_self = cm->unregisterCommand(name);
        return new ReentrancyProbeCommand(name);
    });

    EXPECT_TRUE(registered);
    EXPECT_TRUE(inside_registered_other);
    EXPECT_TRUE(inside_infos_ok);
    EXPECT_FALSE(inside_unregistered_self) << "探测时它还没注册成功";
    EXPECT_TRUE(cm->isRegistered(name));
    EXPECT_EQ(cm->executeCommand(name).status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(ReentrancyProbeCommand::s_runs.load(), 1);

    cm->unregisterCommand(name);
    cm->unregisterCommand(other);
}

// 重入 2：executed 处理函数里再跑一条命令、注销刚跑完的命令、增删处理函数。
// 事件是在所有锁之外同步触发的，Signal 又是"先取快照再查找"，因此这些都安全。
TEST_F(GuiTest, CommandManager_ReentrantEventHandlerIsSafe)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto first  = vine::String(u8"reentrantFirst");
    const auto second = vine::String(u8"reentrantSecond");
    cm->unregisterCommand(first);
    cm->unregisterCommand(second);
    ASSERT_TRUE(cm->registerCommand(ReentrancyProbeCommand::desc(), first, [first] {
        return new ReentrancyProbeCommand(first);
    }));
    ASSERT_TRUE(cm->registerCommand(ReentrancyProbeCommand::desc(), second, [second] {
        return new ReentrancyProbeCommand(second);
    }));
    cm->clearHistory();
    ReentrancyProbeCommand::s_runs = 0;

    std::atomic<int> notifications{ 0 };
    std::atomic<int> nested_status{ -1 };

    // 处理函数在通知里增删处理函数：本轮新增的不应被调用，注销的也不应。
    std::atomic<int>             late_handler_calls{ 0 };
    decltype(cm->executed)::HandlerId late_handler_id{};
    const auto                   id = cm->executed.addHandler(
        [&](vine::appfw::CommandManager& manager, vine::appfw::CommandExecutedEventArgs& args) {
            const auto* c = args.command();
            if (c == nullptr || c->name() != first) {
                return;
            }
            notifications.fetch_add(1);
            // 重入：通知里再跑一条命令（同步入口，当前线程就是主线程）。
            nested_status.store(static_cast<int>(manager.executeCommand(second).status()));
            // 重入：注销刚跑完的命令（实例与注册项是两回事，不应影响本次上报）。
            static_cast<void>(manager.unregisterCommand(first));
            // 重入：本轮新增的处理函数不应在本次通知里被调用。
            late_handler_id = manager.executed.addHandler(
                [&late_handler_calls](vine::appfw::CommandManager&, vine::appfw::CommandExecutedEventArgs&) {
                    late_handler_calls.fetch_add(1);
                });
        });

    const auto result = cm->executeCommand(first);
    cm->executed.removeHandler(id);

    // 触发期间新增的处理函数：本次不生效，但从下一条命令开始生效。
    const auto after = cm->executeCommand(second);

    // 它捕获的是本用例栈上的对象：用例结束前必须注销，否则会挂到下一条命令上。
    cm->executed.removeHandler(late_handler_id);

    EXPECT_EQ(result.status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(after.status(), vine::appfw::CommandStatus::Success);
    EXPECT_EQ(notifications.load(), 1);
    EXPECT_EQ(nested_status.load(), static_cast<int>(vine::appfw::CommandStatus::Success));
    EXPECT_EQ(ReentrancyProbeCommand::s_runs.load(), 3);
    EXPECT_EQ(cm->historyCount(), 3);
    EXPECT_FALSE(cm->isRegistered(first)) << "处理函数把命令注销了";
    EXPECT_EQ(late_handler_calls.load(), 1) << "后加的处理函数只对之后的命令生效";

    cm->clearHistory();
    cm->unregisterCommand(second);
}

// 多线程：两个排他命令从不同线程同时提交时，也必须是串行的——排他意味着接管
// 前台，两个同时跑就不叫排他了（旧实现里两者都绕过串联门 ⇒ 峰值为 2）。
TEST_F(GuiTest, CommandManager_ExclusiveCommandsAreSerialized)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vine::String(u8"exclusiveSlow");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<SlowExclusiveProbeCommand>(name));
    cm->clearHistory();
    SlowExclusiveProbeCommand::s_in_flight = 0;
    SlowExclusiveProbeCommand::s_max       = 0;

    vine::appfw::CommandResult result_a;
    vine::appfw::CommandResult result_b;
    std::thread                runner_a([&] { result_a = cm->executeCommand(name); });
    std::thread                runner_b([&] { result_b = cm->executeCommand(name); });
    runner_a.join();
    runner_b.join();

    EXPECT_EQ(SlowExclusiveProbeCommand::s_max.load(), 1) << "两个排他命令不得并发执行";

    // 两种正确结局：后到者接管（前者已收尾 ⇒ 串行成功），或在前者建链前抢先落败
    // 被串行门拒绝。至少有一个真的执行了。
    const bool a_ok = result_a.status() == vine::appfw::CommandStatus::Success;
    const bool b_ok = result_b.status() == vine::appfw::CommandStatus::Success;
    EXPECT_TRUE(a_ok || b_ok) << "至少有一个排他命令应该执行";
    EXPECT_EQ(cm->runningCount(), 0);

    cm->clearHistory();
    cm->unregisterCommand(name);
}
