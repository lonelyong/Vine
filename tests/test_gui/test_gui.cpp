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
#include <QDoubleSpinBox>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QSpinBox>
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
#include <vine/appfw/PluginLoadContext.hpp>

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
#include <memory>
#include <stdexcept>
#include <thread>

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
        static char  arg0[] = "test_gui";
        static char* argv[] = { arg0, nullptr };
        int          argc   = 1;
        app                 = std::make_unique<guifw::GuiApplication>(argc, argv);
        app->init(); // 创建 QApplication
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
    ASSERT_NE(ctx.configs(), nullptr);
    EXPECT_EQ(ctx.configs(), app->configRegistry());
    EXPECT_NE(ctx.eventBus(), nullptr);
    EXPECT_EQ(ctx.eventBus(), app->eventBus());

    // Registering through the context targets the same registry as Application
    auto* pluginCat = ctx.configs()->addCategory(u8"插件");
    ASSERT_NE(pluginCat, nullptr);
    pluginCat->addGroup(u8"常规")->addItem(vine::appfw::ConfigItem(u8"plugin.opt", u8"插件选项", vine::appfw::ConfigItemType::Bool));
    EXPECT_NE(app->configRegistry()->item(u8"plugin.opt"), nullptr);
    EXPECT_TRUE(ctx.configs()->removeItem(u8"plugin.opt"));
    EXPECT_TRUE(ctx.configs()->removeCategory(u8"插件"));
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
