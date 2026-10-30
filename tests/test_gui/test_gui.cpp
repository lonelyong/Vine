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

#include <QAbstractButton>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QWidget>

#include <SARibbon.h>

#include <vine/vn_global.hpp>

#include <vine/appfw/Command.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/ConfigItem.hpp>
#include <vine/appfw/ConfigManager.hpp>
#include <vine/appfw/ConfigRegistry.hpp>
#include <vine/appfw/ConfigStandard.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>
#include <vine/appfw/Plugin.hpp>
#include <vine/appfw/PluginLoadContext.hpp>
#include <vine/appfw/PluginManager.hpp>
#include <vine/appfw/UserIO.hpp>
#include <vine/appfw/gui/Backstage.hpp>
#include <vine/appfw/gui/AboutDialog.hpp>
#include <vine/appfw/gui/AboutPanel.hpp>
#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>
#include <vine/appfw/gui/ConfigWindow.hpp>
#include <vine/appfw/gui/ConsolePanel.hpp>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/DockPanel.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/DocumentManagerDialog.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/PluginManagerDialog.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include <DockingPaneGeometry.h>
#include <DockingPaneTheme.h>
#include <vine/appfw/ConsoleProgressReporter.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/ProgressPresenter.hpp>
#include <vine/appfw/gui/RibbonAction.hpp>
#include <vine/appfw/gui/RibbonBar.hpp>
#include <vine/appfw/gui/RibbonButton.hpp>
#include <vine/appfw/gui/RibbonGroup.hpp>
#include <vine/appfw/gui/RibbonPlacement.hpp>
#include <vine/appfw/gui/RibbonTab.hpp>
#include <vine/appfw/gui/VisualUserIO.hpp>

#include <vine/logging/Log.hpp>
#include <vine/logging/LogSink.hpp>

#include "ConsoleUserIO.hpp"

#include "fixtures/TestGuiApplication.hpp"

#include <vine/appfw/ProgressHost.hpp>
#include <vine/progress/ProgressRange.hpp>
#include <vine/progress/ProgressScope.hpp>

#include <vine/async/AsyncEvent.hpp>
#include <vine/async/Sleep.hpp>

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QAction>
#include <QFileDialog>
#include <QListWidget>
#include <QMenu>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBlock>
#include <QTextDocument>
#include <QTimer>

namespace guifw = vn::appfw::gui;

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

        // 与真实应用一样把 AppConfig 交给构造函数（builder 也只有这一步：本进程不设 built_in_plugin_dir）。
        // 用测试子类是为了拿到受保护的启动收尾钩子（见 fixtures/TestGuiApplication.hpp）。
        vn::appfw::AppConfig config;
        config.name = u8"test_gui";
        app         = std::make_unique<TestGuiApplication>(config, 1, argv);

        // 从干净状态开始：上一次运行（或中途失败）可能留下插件注册文件。
        // Qt 测试模式已经把数据目录重定向到临时区，删掉它是安全的。
        std::error_code ec;
        std::filesystem::remove_all(app->dataDirectory(), ec);
    }

    void TearDown() override
    {
        app.reset();
    }

    static std::unique_ptr<TestGuiApplication> app;
};

std::unique_ptr<TestGuiApplication> GuiEnv::app;

// gtest_main 没有自定义 main 的钩子，用静态初始化注册全局环境即可。
::testing::Environment* const g_gui_env = ::testing::AddGlobalTestEnvironment(new GuiEnv());

// 把控制台面板绑到可视 UserIO 上：宿主自己找它，框架不在 application 上转一手。
void bindConsole(guifw::GuiApplication* app, guifw::ConsolePanel* panel)
{
    if (auto* io = vn::obj_cast<guifw::VisualUserIO>(app != nullptr ? app->userIO() : nullptr)) {
        io->setConsolePanel(panel);
    }
}

// 控制台里含某段文字那一行的**前景色**（取块里第一个 fragment 的格式）。
//
// 提示与错误靠颜色区分，所以断言颜色比断言"用了哪个枚举"更接近用户看到的东西；
// 基准色取自面板自己刚写下的两行，而不是写死的色值（主题可能变）。
QColor consoleLineColor(QPlainTextEdit* output, const QString& needle)
{
    for (QTextBlock block = output->document()->begin(); block.isValid(); block = block.next()) {
        if (!block.text().contains(needle)) {
            continue;
        }
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            return it.fragment().charFormat().foreground().color();
        }
    }
    return {};
}

/// 一条捕获到的日志行：级别 + 文本。
using CapturedLine = std::pair<vn::logging::LogLevel, std::string>;

/// 日志行的落地点。
///
/// 只追加、不清理：logging 没有“摘掉 sink”的接口，所以 sink 挂上就留到进程结束。
/// 用例记住起点下标，只看自己那一段（`mark()` / `since()`）。
struct LogCapture {
    std::mutex                mutex;
    std::vector<CapturedLine> lines;

    /// @return 当前已捕获的行数，作为“我这段从这里开始”的起点。
    std::size_t mark()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return lines.size();
    }

    /// @param from 起点下标（来自 mark()）。
    /// @return 从起点到现在的那些行。
    std::vector<CapturedLine> since(std::size_t from)
    {
        std::lock_guard<std::mutex> lock(mutex);
        return { lines.begin() + std::min(from, lines.size()), lines.end() };
    }
};

/// @return 进程级的日志捕获缓冲区（只建不挂，挂载在 attachLogCapture() 里）。
LogCapture& logLines()
{
    static LogCapture capture;
    return capture;
}

/// 把日志捕获接到 VN_LOG* 宏写入的那个 logger 上（幂等：只挂一次）。
void attachLogCapture()
{
    static const bool attached = [] {
        vn::logging::defaultLogger().addSink(vn::logging::LogSink::function([](vn::logging::LogLevel level, const std::string& message) {
            auto&                       capture = logLines();
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.lines.emplace_back(level, message);
        }));
        return true;
    }();
    (void)attached;
}

// 最小具体命令，用于验证 CommandManager 的 owner 跟踪与按插件报告。
class DummyCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"dummy";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"dummy command";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(DummyCommand, vn::appfw::Command)

// 返回业务载荷的命令：用来钉住“调用方拿到载荷、历史里不留载荷”这条契约。
class PayloadCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"payload";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"command returning a payload";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        vn::appfw::CommandResult result(vn::appfw::CommandStatus::Success);
        result.setData(std::any(7));
        co_return result;
    }
};

VN_OBJECT_META_IMPL(PayloadCommand, vn::appfw::Command)

// 耗时命令：LongRunning 标志让 CommandManager 自动挂载环境进度宿主。
class LongCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"long";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"long running command";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_await vn::async::sleepFor(std::chrono::milliseconds(120));
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(LongCommand, vn::appfw::Command)

// 声明"我需要一份文档"的命令：缺目标时框架在命令体之前就拒绝。命令体只会返回 Success，
// 所以"结果不是 Success"一定是框架给的。
class NeedsDocumentCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"needs_document";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"needs a document";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::NeedsDocument;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(NeedsDocumentCommand, vn::appfw::Command)

// 声明"我需要一块 3D 视图"的命令（同上：体的返回值证明不了什么，结果只能来自框架）。
class NeedsViewCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"needs_view";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"needs a view";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::NeedsView;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(NeedsViewCommand, vn::appfw::Command)

// 嵌套子命令：LongRunning，复用父命令的取消/进度宿主。
class NestedChildCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"nested_child";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nested child command";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        // 子命令现在是独立宿主（前台栈顶），可上报自己的进度。
        if (auto* host = vn::appfw::ProgressHost::current()) {
            host->setLabel(u8"child");
            vn::progress::ProgressScope scope = host->scope(u8"child", 20);
            for (int i = 0; i < 20; ++i) {
                if (context && context->isCancelled()) {
                    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
                }
                scope.next(1);
                co_await vn::async::sleepFor(std::chrono::milliseconds(10));
            }
        }
        else {
            for (int i = 0; i < 20; ++i) {
                if (context && context->isCancelled()) {
                    co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
                }
                co_await vn::async::sleepFor(std::chrono::milliseconds(10));
            }
        }
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(NestedChildCommand, vn::appfw::Command)

// 嵌套父命令：LongRunning，报告进度并通过 context->executeChild 调用子命令。
class NestedProgressCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"nested_progress";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nested progress command";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* host = vn::appfw::ProgressHost::current();
        if (!host || !context) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed);
        }
        host->setLabel(u8"parent");
        vn::progress::ProgressScope root = host->scope(u8"parent", 20);
        for (int i = 0; i < 8; ++i) {
            if (root.isCancelled()) {
                co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
            }
            root.next(1);
            co_await vn::async::sleepFor(std::chrono::milliseconds(5));
        }
        const auto child_result = co_await context->executeChild(u8"nested_child");
        if (!child_result.succeeded()) {
            co_return child_result;
        }
        for (int i = 0; i < 12; ++i) {
            if (root.isCancelled()) {
                co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
            }
            root.next(1);
            co_await vn::async::sleepFor(std::chrono::milliseconds(5));
        }
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }
};

VN_OBJECT_META_IMPL(NestedProgressCommand, vn::appfw::Command)

// ---- 功能区"命令状态"用例的夹具：两种文档（一个有视图、一个没有）----
constexpr const char8_t* kRibbonStateTypeWithView = u8"ribbon_state_with_view";
constexpr const char8_t* kRibbonStateTypeBlind    = u8"ribbon_state_blind";

/// 只报类型 id 的最小文档：绑定用例只关心"当前目标有没有文档/视图"。
class RibbonStateDocument final : public vn::appfw::Document {
  public:
    explicit RibbonStateDocument(vn::String type_id)
      : type_id_(std::move(type_id))
    {}

    vn::String typeId() const override
    {
        return type_id_;
    }

    vn::String title() const override
    {
        return u8"功能区状态文档";
    }

    bool isDirty() const override
    {
        return false;
    }

  private:
    vn::String type_id_;
};

/// 带一块 3D 区的用例视图：绑定用例只问 renderControl() 是不是空，控件本身没有挂进任何控件树。
class RibbonProbeView final : public guifw::DocumentView {
  public:
    explicit RibbonProbeView(vn::appfw::Document& document)
      : DocumentView(document, new QWidget())
      , control_(new guifw::RenderControl())
    {}

    ~RibbonProbeView() override
    {
        delete control_;
    }

    guifw::UIElement* content() override
    {
        return this;
    }

    guifw::RenderControl* renderControl() const override
    {
        return control_;
    }

  private:
    guifw::RenderControl* control_ = nullptr;
};

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

// 入口就是 SARibbon 的应用按钮：隐藏必须落到控件上，而不只是记住一个开关。
TEST_F(GuiTest, RibbonBar_ApplicationButtonHidesTheEntry)
{
    auto* bar = wnd->ribbonBar();

    // 不直接链接 tp::SARibbon（见 RibbonMenusShowCommandTooltips），按对象名用纯 Qt 的查找取应用按钮。
    auto* entry = bar->impl<SARibbonBar>()->findChild<QToolButton*>(QStringLiteral("SARibbonApplicationButton"));
    ASSERT_NE(entry, nullptr) << "File 应用按钮";

    EXPECT_TRUE(entry->isVisible());
    bar->setApplicationButtonVisible(false);
    EXPECT_FALSE(entry->isVisible()) << "隐藏 = File 入口从 Ribbon 上消失";
    bar->setApplicationButtonVisible(true);
    EXPECT_TRUE(entry->isVisible());
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

namespace
{

/// 逻辑顺序里的下标（找不到 = -1），用于断言放置结果。
int ribbonTabIndex(guifw::RibbonBar* bar, guifw::RibbonTab* tab)
{
    for (int i = 0; i < bar->numTabs(); ++i) {
        if (bar->tabAt(i) == tab) {
            return i;
        }
    }
    return -1;
}

} // namespace

TEST_F(GuiTest, RibbonBar_RolesAndPlacement)
{
    auto* bar = wnd->ribbonBar();

    auto* plugins = new guifw::RibbonTab();
    plugins->setId(u8"test.tab.plugins");
    plugins->setRole(guifw::RibbonTabRole::Plugins);
    plugins->setTitle(u8"Plugins2");
    EXPECT_EQ(bar->addTab(plugins, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::Placed);

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    EXPECT_EQ(bar->addTab(help, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::Placed);
    ASSERT_EQ(ribbonTabIndex(bar, help), bar->numTabs() - 1);

    // 选一个“当前 tab”，插入别的 tab 后选择必须还在它身上。
    ASSERT_GE(ribbonTabIndex(bar, plugins), 0);
    bar->setCurrentIndex(ribbonTabIndex(bar, plugins));

    auto* model = new guifw::RibbonTab();
    model->setId(u8"test.tab.model");
    model->setTitle(u8"Model2");
    EXPECT_EQ(bar->addTab(model, guifw::RibbonPlacement::before(u8"test.tab.help")).status, guifw::RibbonPlaceStatus::Placed);

    EXPECT_LT(ribbonTabIndex(bar, model), ribbonTabIndex(bar, help));
    EXPECT_EQ(ribbonTabIndex(bar, model) + 1, ribbonTabIndex(bar, help));
    EXPECT_EQ(bar->tabAt(bar->currentIndex()), plugins) << "重排后当前 tab 不能变";

    // 角色参与默认顺序：后追加的 View 排在 Plugins 之前。
    auto* view = new guifw::RibbonTab();
    view->setId(u8"test.tab.view");
    view->setRole(guifw::RibbonTabRole::View);
    view->setTitle(u8"View2");
    EXPECT_EQ(bar->addTab(view, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::Placed);
    EXPECT_LT(ribbonTabIndex(bar, view), ribbonTabIndex(bar, plugins));
    EXPECT_EQ(bar->tabAt(bar->numTabs() - 1), help);
}

TEST_F(GuiTest, RibbonBar_TerminalTabStaysLast)
{
    auto* bar = wnd->ribbonBar();

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    bar->addTab(help);

    auto* legacy = new guifw::RibbonTab();
    legacy->setId(u8"test.tab.legacy");
    legacy->setTitle(u8"Legacy");
    bar->addTab(legacy); // 旧接口：追加

    EXPECT_EQ(bar->tabAt(bar->numTabs() - 1), help);
    EXPECT_EQ(ribbonTabIndex(bar, legacy), bar->numTabs() - 2);
}

TEST_F(GuiTest, RibbonBar_UnknownAnchorFallsBack)
{
    auto* bar = wnd->ribbonBar();

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    bar->addTab(help);

    auto* lost = new guifw::RibbonTab();
    lost->setId(u8"test.tab.lost");
    lost->setTitle(u8"Lost");
    const auto result = bar->addTab(lost, guifw::RibbonPlacement::before(u8"test.tab.missing"));
    EXPECT_EQ(result.status, guifw::RibbonPlaceStatus::PlacedByFallback);
    EXPECT_EQ(result.index, ribbonTabIndex(bar, lost));
    EXPECT_EQ(ribbonTabIndex(bar, lost) + 1, ribbonTabIndex(bar, help)) << "回退也不能跑到终止 tab 后面";

    // 锚点既没有 id 也没有 role：请求无效，tab 不加入。
    const int before = bar->numTabs();
    auto*     bad    = new guifw::RibbonTab();
    bad->setId(u8"test.tab.bad");
    guifw::RibbonPlacement invalid;
    invalid.where = guifw::RibbonPlacement::Where::Before;
    EXPECT_EQ(bar->addTab(bad, invalid).status, guifw::RibbonPlaceStatus::InvalidAnchor);
    EXPECT_EQ(bar->numTabs(), before);
}

TEST_F(GuiTest, RibbonBar_AfterHelpIsClamped)
{
    auto* bar = wnd->ribbonBar();

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    bar->addTab(help);

    auto* late = new guifw::RibbonTab();
    late->setId(u8"test.tab.late");
    late->setTitle(u8"Late");
    const auto result = bar->addTab(late, guifw::RibbonPlacement::after(guifw::RibbonTabRole::Help));
    EXPECT_EQ(result.status, guifw::RibbonPlaceStatus::ClampedBeforeTerminal);
    EXPECT_EQ(ribbonTabIndex(bar, late) + 1, ribbonTabIndex(bar, help));
    EXPECT_EQ(bar->tabAt(bar->numTabs() - 1), help);
}

TEST_F(GuiTest, RibbonBar_FindByRoleAndId)
{
    auto* bar = wnd->ribbonBar();

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    bar->addTab(help);

    EXPECT_EQ(bar->findTab(u8"test.tab.help"), help);
    EXPECT_EQ(bar->findTab(guifw::RibbonTabRole::Help), help);
    EXPECT_EQ(bar->findTab(u8"test.tab.missing"), nullptr);
    EXPECT_EQ(bar->findTab(guifw::RibbonTabRole::None), nullptr);

    auto* found = bar->findTab(u8"test.tab.help");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->role(), guifw::RibbonTabRole::Help);
}

TEST_F(GuiTest, RibbonBar_DuplicateIdRejected)
{
    auto* bar = wnd->ribbonBar();

    auto* first = new guifw::RibbonTab();
    first->setId(u8"test.tab.dup");
    bar->addTab(first);

    const int before = bar->numTabs();
    auto*     second = new guifw::RibbonTab();
    second->setId(u8"test.tab.dup");
    EXPECT_EQ(bar->addTab(second, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::DuplicateId);
    EXPECT_EQ(bar->numTabs(), before);
    EXPECT_EQ(bar->findTab(u8"test.tab.dup"), first);
}

TEST_F(GuiTest, RibbonBar_LegacyAddStillAppends)
{
    auto* bar = wnd->ribbonBar();

    auto* legacy = new guifw::RibbonTab();
    legacy->setTitle(u8"Legacy");
    bar->addTab(legacy);
    EXPECT_EQ(bar->tabAt(bar->numTabs() - 1), legacy);

    // 同一个对象再 add 一次：不重复登记（旧行为）。
    const int before = bar->numTabs();
    bar->addTab(legacy);
    EXPECT_EQ(bar->numTabs(), before);
}

TEST_F(GuiTest, RibbonTab_GroupPlacementAndContribution)
{
    auto* bar = wnd->ribbonBar();

    auto* help = new guifw::RibbonTab();
    help->setId(u8"test.tab.help");
    help->setRole(guifw::RibbonTabRole::Help);
    help->setPolicy(guifw::RibbonTabPolicy::Terminal);
    help->setTitle(u8"Help2");
    bar->addTab(help);

    auto* about = new guifw::RibbonGroup();
    about->setId(u8"help.about");
    about->setTitle(u8"About2");
    EXPECT_EQ(help->addGroup(about, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::Placed);

    // 往标准 tab 里插一个组：放在 about 之前。
    auto* extra = new guifw::RibbonGroup();
    extra->setId(u8"help.extra");
    extra->setTitle(u8"Extra2");
    EXPECT_EQ(help->addGroup(extra, guifw::RibbonPlacement::before(u8"help.about")).status, guifw::RibbonPlaceStatus::Placed);

    ASSERT_EQ(help->numGroups(), 2);
    EXPECT_EQ(help->groupAt(0), extra);
    EXPECT_EQ(help->groupAt(1), about);
    EXPECT_EQ(help->findGroup(u8"help.about"), about);
    EXPECT_EQ(help->findGroup(u8"help.missing"), nullptr);

    // 组 id 在 tab 内唯一。
    auto* dup = new guifw::RibbonGroup();
    dup->setId(u8"help.about");
    EXPECT_EQ(help->addGroup(dup, guifw::RibbonPlacement::append()).status, guifw::RibbonPlaceStatus::DuplicateId);
    EXPECT_EQ(help->numGroups(), 2);

    // 组级不支持 role 锚点。
    auto* role_anchor = new guifw::RibbonGroup();
    role_anchor->setId(u8"help.role");
    EXPECT_EQ(help->addGroup(role_anchor, guifw::RibbonPlacement::before(guifw::RibbonTabRole::Help)).status, guifw::RibbonPlaceStatus::InvalidAnchor);
    EXPECT_EQ(help->numGroups(), 2);

    // 旧接口仍然是追加。
    auto* tail = new guifw::RibbonGroup();
    tail->setId(u8"help.tail");
    help->addGroup(tail);
    EXPECT_EQ(help->groupAt(help->numGroups() - 1), tail);
}

// 命令条目是命令的视图：启用态、tooltip 与勾选态都从 CommandManager 派生，而不是条目自己记一份。
TEST_F(GuiTest, RibbonItemsFollowTheCommandState)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const vn::String name(u8"ribbon_state_probe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    auto* button = new guifw::RibbonButton();
    button->setText(u8"State");
    groupClip->addButton(button);
    button->setCommand(name);

    EXPECT_TRUE(button->isEnabled());
    EXPECT_EQ(button->tooltip(), vn::String(u8"dummy command")) << "描述就是命令条目的 tooltip";

    // 持久化偏好翻转 ⇒ 条目跟着灰显/恢复（条目不写偏好，只是显示它）。
    cm->setCommandEnabled(name, false);
    EXPECT_FALSE(button->isEnabled());
    cm->setCommandEnabled(name, true);
    EXPECT_TRUE(button->isEnabled());

    // 调用方自己设的 tooltip 不被描述覆盖。
    auto* action = new guifw::RibbonAction();
    action->setTooltip(u8"mine");
    action->setCommand(name);
    EXPECT_EQ(action->tooltip(), vn::String(u8"mine"));

    // 菜单项/Backstage 条目也是 RibbonAction：同一条规则，启用态同样跟命令走。
    EXPECT_TRUE(action->isEnabled());
    cm->setCommandEnabled(name, false);
    EXPECT_FALSE(action->isEnabled());
    cm->setCommandEnabled(name, true);
    EXPECT_TRUE(action->isEnabled());

    // 勾选态由 provider 回答，并在"命令执行完"这一拍重新求值。
    auto on = std::make_shared<bool>(false);
    button->setCheckedProvider([on] { return *on; });
    EXPECT_TRUE(button->isCheckable());
    EXPECT_FALSE(button->isChecked());

    *on = true;
    ASSERT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(button->isChecked());

    // 命令注销 ⇒ 不能执行 ⇒ 灰显；provider 的回答不会被条目自己改成 false。
    cm->unregisterCommand(name);
    EXPECT_FALSE(button->isEnabled());
    EXPECT_TRUE(button->isChecked()) << "勾选态只由 provider 回答";
}

// NeedsDocument / NeedsView 是对**派发目标**的要求：目标没了，条目就灰显（但那不是持久化禁用）。
TEST_F(GuiTest, RibbonItemsFollowTheResolvedTarget)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm        = app->commandManager();
    auto* documents = app->documentManager();
    auto* views     = app->viewRegistry();
    ASSERT_NE(cm, nullptr);
    ASSERT_NE(documents, nullptr);
    ASSERT_NE(views, nullptr);

    auto* docks = wnd->dockPanelManager();
    ASSERT_NE(docks, nullptr);

    // 共享的应用文档管理器里可能留着别的用例打开的文档。关掉当前文档时宿主要求"接替的 tab"成为当前，
    // 所以本用例先让管理器回到"没有打开文档"，后面的断言才说得清"目标有没有"。
    for (auto* open : documents->documents()) {
        documents->close(open);
    }
    ASSERT_TRUE(documents->documents().empty()) << "别的用例留下的文档拒绝了关闭，本用例无法控制目标";
    ASSERT_EQ(documents->current(), nullptr);

    guifw::CentralDocumentHost host(*documents, *views, *docks);

    // 共享的管理器不会注销文档类型：重复登记返回 false 不是错误，用例只要求"这个 id 可用"。
    vn::appfw::DocumentTypeRegistration with_view;
    with_view.type_id = kRibbonStateTypeWithView;
    with_view.create  = [] { return new RibbonStateDocument(kRibbonStateTypeWithView); };
    documents->registerType(with_view);

    vn::appfw::DocumentTypeRegistration blind;
    blind.type_id = kRibbonStateTypeBlind;
    blind.create  = [] { return new RibbonStateDocument(kRibbonStateTypeBlind); };
    documents->registerType(blind);

    views->registerView(kRibbonStateTypeWithView, [](vn::appfw::Document& document) { return new RibbonProbeView(document); });

    const vn::String needs_document(u8"needs_document");
    const vn::String needs_view(u8"needs_view");
    cm->unregisterCommand(needs_document);
    cm->unregisterCommand(needs_view);
    ASSERT_TRUE(cm->registerCommand<NeedsDocumentCommand>(needs_document));
    ASSERT_TRUE(cm->registerCommand<NeedsViewCommand>(needs_view));

    const auto document_flags = cm->commandFlags(needs_document);
    ASSERT_TRUE(document_flags.has_value());
    EXPECT_TRUE(vn::testFlag(*document_flags, vn::appfw::CommandFlags::NeedsDocument));

    auto* document_item = new guifw::RibbonButton();
    document_item->setText(u8"Doc");
    groupView->addButton(document_item);
    document_item->setCommand(needs_document);

    auto* view_item = new guifw::RibbonButton();
    view_item->setText(u8"View");
    groupView->addButton(view_item);
    view_item->setCommand(needs_view);

    // 有一份带 3D 区的当前文档：两份要求都满足。
    auto* shown = documents->create(kRibbonStateTypeWithView);
    ASSERT_NE(shown, nullptr);
    ASSERT_TRUE(documents->setCurrent(shown));
    EXPECT_TRUE(document_item->isEnabled());
    EXPECT_TRUE(view_item->isEnabled());

    // 当前文档没有视图 ⇒ 派发目标为空（resolveCommandTarget 从视图取文档）⇒ 两份要求都不满足。
    auto* no_view = documents->create(kRibbonStateTypeBlind);
    ASSERT_NE(no_view, nullptr);
    ASSERT_TRUE(documents->setCurrent(no_view));
    EXPECT_TRUE(vn::appfw::gui::resolveCommandTarget(wnd.get()).isEmpty()) << "没有视图的当前文档解析出空目标";
    EXPECT_FALSE(document_item->isEnabled());
    EXPECT_FALSE(view_item->isEnabled());

    // 关掉当前文档：宿主要求剩下的那个 tab 接替 ⇒ 当前文档变回 shown，要求再次满足。
    ASSERT_TRUE(documents->close(no_view));
    EXPECT_EQ(documents->current(), shown) << "中央区宿主把接替的 tab 请求为当前文档";
    EXPECT_TRUE(document_item->isEnabled());
    EXPECT_TRUE(view_item->isEnabled());

    // 最后一份也关掉：没有当前文档，也没有目标。
    ASSERT_TRUE(documents->close(shown));
    EXPECT_EQ(documents->current(), nullptr);
    EXPECT_FALSE(document_item->isEnabled());
    EXPECT_FALSE(view_item->isEnabled());

    cm->unregisterCommand(needs_document);
    cm->unregisterCommand(needs_view);
}

// 命令在工作线程结束时，刷新必须先编组回应用线程：工作线程不许碰控件。
TEST_F(GuiTest, RibbonItemStateFromAWorkerThreadIsMarshalled)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const vn::String name(u8"ribbon_thread_probe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    auto* button = new guifw::RibbonButton();
    button->setText(u8"Thread");
    groupClip->addButton(button);
    button->setCommand(name);

    auto on = std::make_shared<bool>(false);
    button->setCheckedProvider([on] { return *on; });
    EXPECT_FALSE(button->isChecked());

    // provider 的答案在命令跑之前就变了；命令在工作线程上结束 ⇒ executed 也在那个线程发火。
    *on = true;
    std::thread worker([&] { (void)cm->executeCommandAndWait(name); });
    worker.join();

    EXPECT_FALSE(button->isChecked()) << "工作线程不直接改控件，刷新要投递回应用线程";
    QCoreApplication::processEvents();
    EXPECT_TRUE(button->isChecked()) << "投递的刷新在应用线程上执行";

    cm->unregisterCommand(name);
}

// 描述作为 tooltip 要真的能看见：QMenu 默认不显示条目 tooltip，框架创建的两处菜单都必须打开它。
TEST_F(GuiTest, RibbonMenusShowCommandTooltips)
{
    auto* bar = wnd->ribbonBar();

    auto* item = new guifw::RibbonAction();
    item->setText(u8"About");
    bar->appendApplicationMenu(item);

    // 不直接链接 tp::SARibbon（见 CMakeLists：它在 vnAppfwd.dll 里另有一份静态副本），所以这里不调用
    // SARibbonBar::applicationButton() —— 那需要该库的符号。改成按 SARibbon 给默认应用按钮设的对象名，
    // 用纯 Qt 的查找取到按钮：只依赖 QObject/QToolButton 的符号。
    auto* app_btn = bar->impl<SARibbonBar>()->findChild<QToolButton*>(QStringLiteral("SARibbonApplicationButton"));
    ASSERT_NE(app_btn, nullptr);
    ASSERT_NE(app_btn->menu(), nullptr) << "应用按钮的菜单";
    EXPECT_TRUE(app_btn->menu()->toolTipsVisible());

    // 按钮下拉菜单（btnInsert 在夹具里已经有三个条目 ⇒ 菜单已建好）。
    auto* tool_btn = btnInsert->impl<SARibbonToolButton>();
    ASSERT_NE(tool_btn, nullptr);
    ASSERT_NE(tool_btn->menu(), nullptr) << "下拉菜单";
    EXPECT_TRUE(tool_btn->menu()->toolTipsVisible());
}

TEST_F(GuiTest, Backstage_PagesAndNavigation)
{
    guifw::Backstage backstage(wnd.get());

    auto* first = new guifw::BackstagePage();
    first->setId(u8"test.backstage.first");
    first->setTitle(u8"First");
    EXPECT_TRUE(backstage.addPage(first));
    EXPECT_EQ(backstage.numPages(), 1);
    EXPECT_EQ(backstage.currentPage(), first);
    EXPECT_EQ(backstage.findPage(u8"test.backstage.first"), first);

    auto* second = new guifw::BackstagePage();
    second->setId(u8"test.backstage.second");
    second->setTitle(u8"Second");
    EXPECT_TRUE(backstage.addPage(second));
    EXPECT_EQ(backstage.numPages(), 2);
    EXPECT_EQ(backstage.currentPage(), first) << "第一页应保持当前页";

    backstage.setCurrentPage(second);
    EXPECT_EQ(backstage.currentPage(), second);

    // 非空 id 唯一：重复页面被拒绝，且不进入导航。
    auto* duplicate = new guifw::BackstagePage();
    duplicate->setId(u8"test.backstage.first");
    duplicate->setTitle(u8"Dup");
    EXPECT_FALSE(backstage.addPage(duplicate));
    EXPECT_EQ(backstage.numPages(), 2);

    // 空 id 允许存在，但不能被查找。
    auto* anonymous_page = new guifw::BackstagePage();
    anonymous_page->setTitle(u8"Anon");
    EXPECT_TRUE(backstage.addPage(anonymous_page));
    EXPECT_EQ(backstage.pageAt(2), anonymous_page);
    EXPECT_EQ(backstage.findPage(u8""), nullptr);
}

// 没有图标的页面也要和带图标的页面对齐：每行都由导航列表留出同样的图标位。
TEST_F(GuiTest, Backstage_NavRowsReserveTheIconColumn)
{
    guifw::Backstage backstage(wnd.get());

    auto* with_icon = new guifw::BackstagePage();
    with_icon->setId(u8"test.backstage.align.with-icon");
    with_icon->setTitle(u8"WithIcon");
    with_icon->setIcon(guifw::Icon(u8":/icons/about.svg"));

    auto* without_icon = new guifw::BackstagePage();
    without_icon->setId(u8"test.backstage.align.without-icon");
    without_icon->setTitle(u8"NoIcon");

    ASSERT_TRUE(backstage.addPage(with_icon));
    ASSERT_TRUE(backstage.addPage(without_icon));

    auto* nav = backstage.impl<QWidget>()->findChild<QListWidget*>();
    ASSERT_NE(nav, nullptr) << "导航列表";
    ASSERT_EQ(nav->count(), 2);

    // QListWidget 只在行有非空图标时才留图标列；无图标行用透明图标占位，否则标题会左移。
    for (int row = 0; row < nav->count(); ++row) {
        EXPECT_FALSE(nav->item(row)->icon().isNull()) << "导航行缺少图标位: row " << row;
    }
    EXPECT_EQ(nav->item(1)->icon().availableSizes().first(), nav->iconSize());
}

// 版式：页面条目列与标题共用同一条左边线，条目行与导航行同一节奏（40 px）。
TEST_F(GuiTest, Backstage_PageRowsAlignWithTheTitle)
{
    guifw::Backstage backstage(wnd.get());

    auto* page = new guifw::BackstagePage();
    page->setId(u8"test.backstage.layout");
    page->setTitle(u8"Layout");

    auto* item = new guifw::RibbonAction();
    item->setText(u8"An item");
    page->addItem(item);

    ASSERT_TRUE(backstage.addPage(page));
    backstage.open();
    QCoreApplication::processEvents();

    auto* window = backstage.impl<QWidget>();
    ASSERT_NE(window, nullptr);

    auto* title = window->findChild<QLabel*>(QStringLiteral("vnBackstageTitle"));
    auto* row   = page->impl<QWidget>()->findChild<QToolButton*>();

    ASSERT_NE(title, nullptr) << "页面标题";
    ASSERT_NE(row, nullptr) << "条目行";

    // 标题与条目列各在自己的父控件里，所以用窗口坐标比较它们真正画在哪一列。
    EXPECT_EQ(row->mapTo(window, QPoint(0, 0)).x(), title->mapTo(window, QPoint(0, 0)).x())
      << "条目列的左边缘要落在标题下面";
    EXPECT_EQ(row->height(), 40) << "条目行与导航行同一行高";

    backstage.close();
}

// 条目超过一屏时页面列可滚动；body 页不套滚动区（body 自己决定要不要滚）。
TEST_F(GuiTest, Backstage_ManyItemsScroll)
{
    guifw::Backstage backstage(wnd.get());

    auto* page = new guifw::BackstagePage();
    page->setId(u8"test.backstage.scroll");
    page->setTitle(u8"Scroll");

    for (int i = 0; i < 40; ++i) {
        auto* item = new guifw::RibbonAction();
        item->setText(u8"An item");
        page->addItem(item);
    }

    ASSERT_TRUE(backstage.addPage(page));
    backstage.open();
    QCoreApplication::processEvents();

    auto* scroll = page->impl<QWidget>()->findChild<QScrollArea*>();
    ASSERT_NE(scroll, nullptr) << "条目列的滚动区";
    ASSERT_NE(scroll->verticalScrollBar(), nullptr);
    EXPECT_GT(scroll->verticalScrollBar()->maximum(), 0) << "条目超过一屏时应可滚动";
    EXPECT_EQ(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff) << "条目行只在纵向滚动";

    // body 页不套滚动区：body 是一整块控件，滚动与否归它自己。
    auto* body_page = new guifw::BackstagePage();
    body_page->setTitle(u8"Body");

    auto* body = new guifw::Control(new QLabel(QStringLiteral("body")));
    ASSERT_TRUE(body_page->setBody(body));
    EXPECT_EQ(body_page->impl<QWidget>()->findChild<QScrollArea*>(), nullptr) << "body 页不套滚动区";

    backstage.close();
}

TEST_F(GuiTest, Backstage_ApplicationButtonOpensThePage)
{
    auto* bar      = wnd->ribbonBar();
    auto* backstage = new guifw::Backstage(wnd.get());
    auto* page      = new guifw::BackstagePage();
    page->setId(u8"test.backstage.home");
    page->setTitle(u8"Home");
    ASSERT_TRUE(backstage->addPage(page));

    wnd->setBackstage(backstage);
    EXPECT_EQ(wnd->backstage(), backstage);
    EXPECT_EQ(bar->backstage(), backstage);
    EXPECT_FALSE(backstage->isOpen());

    vn::EventArgs args;
    bar->applicationButtonClicked().trigger(*bar, args);
    EXPECT_TRUE(backstage->isOpen());

    backstage->close();
    EXPECT_FALSE(backstage->isOpen());

    wnd->setBackstage(nullptr);
    EXPECT_EQ(wnd->backstage(), nullptr);
    EXPECT_EQ(bar->backstage(), nullptr);
}

TEST_F(GuiTest, Backstage_PageItemsAreCommandBound)
{
    guifw::Backstage backstage(wnd.get());

    auto* page = new guifw::BackstagePage();
    page->setId(u8"test.backstage.items");
    page->setTitle(u8"Items");
    ASSERT_TRUE(backstage.addPage(page));

    auto* item = new guifw::RibbonAction();
    item->setText(u8"About2");
    item->setIcon(guifw::Icon(u8":/icons/about.svg"));
    item->setCommand(u8"about");
    page->addItem(item);

    EXPECT_EQ(page->numItems(), 1);
    EXPECT_EQ(page->itemAt(0), item);
    EXPECT_EQ(page->itemAt(1), nullptr);
    EXPECT_TRUE(item->command() == u8"about");

    // 条目就是一个绑定到动作的按钮：点击会走动作的 triggered → 命令。
    auto* page_widget = page->impl<QWidget>();
    ASSERT_NE(page_widget, nullptr);
    const auto buttons = page_widget->findChildren<QToolButton*>();
    ASSERT_EQ(buttons.size(), 1);
    EXPECT_EQ(buttons.first()->defaultAction(), item->impl<QAction>());
}

// 页面可以被宿主隐藏/恢复：隐藏只是从导航里拿掉，页面对象与条目都还在。
TEST_F(GuiTest, Backstage_PagesCanBeHidden)
{
    guifw::Backstage backstage(wnd.get());

    auto* first = new guifw::BackstagePage();
    first->setId(u8"test.backstage.hidden.first");
    first->setTitle(u8"First");
    auto* second = new guifw::BackstagePage();
    second->setId(u8"test.backstage.hidden.second");
    second->setTitle(u8"Second");
    auto* third = new guifw::BackstagePage();
    third->setId(u8"test.backstage.hidden.third");
    third->setTitle(u8"Third");

    ASSERT_TRUE(backstage.addPage(first));
    ASSERT_TRUE(backstage.addPage(second));
    ASSERT_TRUE(backstage.addPage(third));
    EXPECT_EQ(backstage.currentPage(), first);

    auto* nav = backstage.impl<QWidget>()->findChild<QListWidget*>();
    ASSERT_NE(nav, nullptr) << "导航列表";
    ASSERT_EQ(nav->count(), 3);

    // 隐藏非当前页：导航行消失，但页面仍可查找，页码/顺序不变。
    backstage.setPageVisible(second, false);
    EXPECT_FALSE(backstage.isPageVisible(second));
    EXPECT_TRUE(nav->item(1)->isHidden());
    EXPECT_EQ(backstage.currentPage(), first);
    EXPECT_EQ(backstage.numPages(), 3);
    EXPECT_EQ(backstage.findPage(u8"test.backstage.hidden.second"), second);
    EXPECT_EQ(backstage.pageAt(1), second) << "隐藏不改变逻辑顺序";

    // 隐藏当前页：交给下一个可见页（循环）。
    backstage.setPageVisible(first, false);
    EXPECT_TRUE(nav->item(0)->isHidden());
    EXPECT_EQ(backstage.currentPage(), third);

    // 隐藏的页面不能被选中。
    backstage.setCurrentPage(second);
    EXPECT_EQ(backstage.currentPage(), third);

    // 全部隐藏：没有当前页，打开被拒绝（空 Backstage 不上屏）。
    backstage.setPageVisible(third, false);
    EXPECT_EQ(backstage.currentPage(), nullptr);
    backstage.open();
    EXPECT_FALSE(backstage.isOpen());

    // 恢复一页：它成为当前页，Backstage 又能打开。
    backstage.setPageVisible(second, true);
    EXPECT_FALSE(nav->item(1)->isHidden());
    EXPECT_EQ(backstage.currentPage(), second);
    backstage.open();
    EXPECT_TRUE(backstage.isOpen());
    backstage.close();
}

// 页面与 tab 共用一套放置词汇：id 是身份、role 是语义锚、placement 是意图；顺序不再跟着加载顺序走。
TEST_F(GuiTest, Backstage_PageRolesAndPlacement)
{
    guifw::Backstage backstage(wnd.get());

    auto* home = new guifw::BackstagePage();
    home->setId(u8"test.backstage.home");
    home->setTitle(u8"Home");
    home->setRole(guifw::BackstagePageRole::Home);
    EXPECT_EQ(backstage.addPage(home, guifw::BackstagePlacement::append()).status, guifw::BackstagePlaceStatus::Placed);

    // Close 是终止页：它自己进后缀，别的页排不到它后面。
    auto* close = new guifw::BackstagePage();
    close->setId(u8"test.backstage.close");
    close->setTitle(u8"Close");
    close->setRole(guifw::BackstagePageRole::Close);
    close->setPolicy(guifw::BackstagePagePolicy::Terminal);
    ASSERT_EQ(backstage.addPage(close, guifw::BackstagePlacement::append()).status, guifw::BackstagePlaceStatus::Placed);
    ASSERT_EQ(backstage.numPages(), 2);
    EXPECT_EQ(backstage.pageAt(1), close);

    // 按 id 锚定：模型页插到 Close 前面。
    auto* model = new guifw::BackstagePage();
    model->setId(u8"test.backstage.model");
    model->setTitle(u8"Model");
    const auto by_id = backstage.addPage(model, guifw::BackstagePlacement::before(u8"test.backstage.close"));
    EXPECT_EQ(by_id.status, guifw::BackstagePlaceStatus::Placed);
    EXPECT_EQ(by_id.index, 1);
    EXPECT_EQ(backstage.pageAt(1), model);

    // 角色参与默认顺序：Info 的 role 值比 Home 大，后追加也排在 Home 之后。
    auto* info = new guifw::BackstagePage();
    info->setId(u8"test.backstage.info");
    info->setTitle(u8"Info");
    info->setRole(guifw::BackstagePageRole::Info);
    EXPECT_EQ(backstage.addPage(info, guifw::BackstagePlacement::append()).status, guifw::BackstagePlaceStatus::Placed);
    EXPECT_EQ(backstage.pageAt(0), home);
    EXPECT_EQ(backstage.pageAt(2), info);
    EXPECT_EQ(backstage.pageAt(3), close);

    // 插到当前页前面：当前页跟着对象走，而不是跟着行号。
    backstage.setCurrentPage(model);
    auto* extra = new guifw::BackstagePage();
    extra->setId(u8"test.backstage.extra");
    extra->setTitle(u8"Extra");
    EXPECT_EQ(backstage.addPage(extra, guifw::BackstagePlacement::before(u8"test.backstage.model")).status,
              guifw::BackstagePlaceStatus::Placed);
    EXPECT_EQ(backstage.pageAt(1), extra);
    EXPECT_EQ(backstage.pageAt(2), model);
    EXPECT_EQ(backstage.currentPage(), model) << "重排后当前页不能变";

    // 导航行就是逻辑顺序（不是添加顺序）。
    auto* nav = backstage.impl<QWidget>()->findChild<QListWidget*>();
    ASSERT_NE(nav, nullptr) << "导航列表";
    ASSERT_EQ(nav->count(), 5);
    EXPECT_EQ(nav->item(0)->text(), QStringLiteral("Home"));
    EXPECT_EQ(nav->item(1)->text(), QStringLiteral("Extra"));
    EXPECT_EQ(nav->item(2)->text(), QStringLiteral("Model"));
    EXPECT_EQ(nav->item(3)->text(), QStringLiteral("Info"));
    EXPECT_EQ(nav->item(4)->text(), QStringLiteral("Close"));
}

// 放置的判定与 tab 同一张表：未知锚点降级、越界夹回、无效锚点与重复 id 不加。
TEST_F(GuiTest, Backstage_PagePlacementEdgeCases)
{
    guifw::Backstage backstage(wnd.get());

    auto* close = new guifw::BackstagePage();
    close->setId(u8"test.backstage.close");
    close->setTitle(u8"Close");
    close->setRole(guifw::BackstagePageRole::Close);
    close->setPolicy(guifw::BackstagePagePolicy::Terminal);
    ASSERT_TRUE(backstage.addPage(close));

    // 旧接口 = append。
    auto* legacy = new guifw::BackstagePage();
    legacy->setId(u8"test.backstage.legacy");
    legacy->setTitle(u8"Legacy");
    EXPECT_TRUE(backstage.addPage(legacy));
    EXPECT_EQ(backstage.pageAt(0), legacy);
    EXPECT_EQ(backstage.pageAt(1), close);

    // 未知锚点：降级到非终止区末尾，页面仍然加入。
    auto* lost = new guifw::BackstagePage();
    lost->setId(u8"test.backstage.lost");
    lost->setTitle(u8"Lost");
    const auto fallback = backstage.addPage(lost, guifw::BackstagePlacement::before(u8"test.backstage.missing"));
    EXPECT_EQ(fallback.status, guifw::BackstagePlaceStatus::PlacedByFallback);
    EXPECT_EQ(fallback.index, 1);
    EXPECT_EQ(backstage.pageAt(2), close) << "回退也不能跑到终止页后面";

    // 请求放到终止页之后：夹回终止区之前。
    auto* late = new guifw::BackstagePage();
    late->setId(u8"test.backstage.late");
    late->setTitle(u8"Late");
    const auto clamped = backstage.addPage(late, guifw::BackstagePlacement::after(guifw::BackstagePageRole::Close));
    EXPECT_EQ(clamped.status, guifw::BackstagePlaceStatus::ClampedBeforeTerminal);
    EXPECT_EQ(backstage.pageAt(backstage.numPages() - 1), close);

    // 锚点既没有 id 也没有 role：请求无效，页面不加入。
    const int before = backstage.numPages();
    auto*     bad    = new guifw::BackstagePage();
    bad->setId(u8"test.backstage.bad");
    guifw::BackstagePlacement invalid;
    invalid.where = guifw::BackstagePlacement::Where::Before;
    EXPECT_EQ(backstage.addPage(bad, invalid).status, guifw::BackstagePlaceStatus::InvalidAnchor);
    EXPECT_EQ(backstage.numPages(), before);

    // 重复 id 被拒；同一个对象再 add 一次是 AlreadyAdded。
    auto* dup = new guifw::BackstagePage();
    dup->setId(u8"test.backstage.close");
    EXPECT_FALSE(backstage.addPage(dup));
    EXPECT_EQ(backstage.numPages(), before);

    const auto again = backstage.addPage(close, guifw::BackstagePlacement::append());
    EXPECT_EQ(again.status, guifw::BackstagePlaceStatus::AlreadyAdded);
    EXPECT_EQ(again.index, backstage.numPages() - 1);

    // 按角色查找；None 不参与。
    EXPECT_EQ(backstage.findPage(guifw::BackstagePageRole::Close), close);
    EXPECT_EQ(backstage.findPage(u8"test.backstage.close"), close);
    EXPECT_EQ(backstage.findPage(guifw::BackstagePageRole::Info), nullptr);
    EXPECT_EQ(backstage.findPage(guifw::BackstagePageRole::None), nullptr);
}

// 页面主体唯一：默认是命令列，setBody 之后换成一块控件；两者不能在同一个页上并存。
TEST_F(GuiTest, BackstagePage_BodyReplacesTheCommandColumn)
{
    guifw::Backstage backstage(wnd.get());

    auto* page = new guifw::BackstagePage();
    page->setId(u8"test.backstage.body");
    page->setTitle(u8"Body");
    ASSERT_TRUE(backstage.addPage(page));

    auto* page_widget = page->impl<QWidget>();
    ASSERT_NE(page_widget, nullptr);

    // 已经有命令条目：body 不能插进来（一页只做一件事）。
    auto* item = new guifw::RibbonAction();
    item->setText(u8"Open");
    page->addItem(item);
    ASSERT_EQ(page->numItems(), 1);

    guifw::Control command_body(new QLabel(QStringLiteral("commands")));
    EXPECT_FALSE(page->setBody(&command_body)) << "命令列与 body 互斥";
    EXPECT_EQ(page->body(), nullptr);
    EXPECT_EQ(page->numItems(), 1);

    // 干净的页：body 装得上，装完之后不再接受条目，并且真的进了页面。
    auto* text_page = new guifw::BackstagePage();
    text_page->setId(u8"test.backstage.body.text");
    text_page->setTitle(u8"Text");
    ASSERT_TRUE(backstage.addPage(text_page));

    guifw::Control text_body(new QLabel(QStringLiteral("content")));
    EXPECT_TRUE(text_page->setBody(&text_body));
    EXPECT_EQ(text_page->body(), &text_body);
    EXPECT_EQ(text_body.impl<QWidget>()->parent(), text_page->impl<QWidget>()) << "body 由页面持有";
    EXPECT_TRUE(text_page->setBody(&text_body)) << "同一个 body 重复安装是幂等的";

    auto* rejected = new guifw::RibbonAction();
    rejected->setText(u8"Late");
    text_page->addItem(rejected);
    EXPECT_EQ(text_page->numItems(), 0) << "有 body 的页不再接受命令条目";

    // 换一个 body：前一个交回给它的主人（不是被删掉）。
    guifw::Control second(new QLabel(QStringLiteral("second")));
    EXPECT_TRUE(text_page->setBody(&second));
    EXPECT_EQ(text_page->body(), &second);
    EXPECT_EQ(text_body.impl<QWidget>()->parent(), nullptr) << "旧 body 回到调用方手里";
    EXPECT_EQ(second.impl<QWidget>()->parent(), text_page->impl<QWidget>());

    // null 不算 body。
    EXPECT_FALSE(text_page->setBody(nullptr));
    EXPECT_EQ(text_page->body(), &second);
}

// 标准目录可用：宿主页用 Plugins/Help，插件页用 UserRole + n 并按 role 锚定，终止页不会被越过。
TEST_F(GuiTest, Backstage_StandardRoleCatalog)
{
    guifw::Backstage backstage(wnd.get());

    auto* plugins = new guifw::BackstagePage();
    plugins->setId(u8"test.backstage.catalog.plugins");
    plugins->setTitle(u8"Plugins");
    plugins->setRole(guifw::BackstagePageRole::Plugins);
    ASSERT_TRUE(backstage.addPage(plugins));

    auto* help = new guifw::BackstagePage();
    help->setId(u8"test.backstage.catalog.help");
    help->setTitle(u8"Help");
    help->setRole(guifw::BackstagePageRole::Help);
    help->setPolicy(guifw::BackstagePagePolicy::Terminal);
    ASSERT_TRUE(backstage.addPage(help));
    EXPECT_EQ(backstage.pageAt(1), help);
    EXPECT_EQ(backstage.findPage(guifw::BackstagePageRole::Help), help);

    // 插件不知道宿主页的 id，只按 role 锚定。
    auto* contributed = new guifw::BackstagePage();
    contributed->setId(u8"test.backstage.catalog.contributed");
    contributed->setTitle(u8"Contributed");
    contributed->setRole(static_cast<guifw::BackstagePageRole>(static_cast<std::uint16_t>(guifw::BackstagePageRole::UserRole) + 1));
    const auto placed = backstage.addPage(contributed, guifw::BackstagePlacement::before(guifw::BackstagePageRole::Help));
    EXPECT_EQ(placed.status, guifw::BackstagePlaceStatus::Placed);
    EXPECT_EQ(backstage.pageAt(1), contributed);
    EXPECT_EQ(backstage.pageAt(2), help);

    // 追加也不会跑到终止页后面。
    auto* appended = new guifw::BackstagePage();
    appended->setId(u8"test.backstage.catalog.appended");
    appended->setTitle(u8"Appended");
    EXPECT_EQ(backstage.addPage(appended, guifw::BackstagePlacement::append()).status, guifw::BackstagePlaceStatus::Placed);
    EXPECT_EQ(backstage.pageAt(backstage.numPages() - 1), help);
    EXPECT_EQ(backstage.pageAt(backstage.numPages() - 2), appended);
}

// provider 是贡献的身份：插件卸载/重载时整组撤除自己的页，别人的页不受影响。
TEST_F(GuiTest, Backstage_PagesAreRemovedByProvider)
{
    guifw::Backstage backstage(wnd.get());

    auto* host = new guifw::BackstagePage();
    host->setId(u8"test.backstage.host");
    host->setTitle(u8"Host");
    ASSERT_TRUE(backstage.addPage(host));

    auto* first = new guifw::BackstagePage();
    first->setId(u8"test.backstage.a1");
    first->setTitle(u8"A1");
    first->setProvider(u8"plugin.a");
    ASSERT_TRUE(backstage.addPage(first));

    auto* second = new guifw::BackstagePage();
    second->setId(u8"test.backstage.a2");
    second->setTitle(u8"A2");
    second->setProvider(u8"plugin.a");
    ASSERT_TRUE(backstage.addPage(second));

    auto* other = new guifw::BackstagePage();
    other->setId(u8"test.backstage.b1");
    other->setTitle(u8"B1");
    other->setProvider(u8"plugin.b");
    ASSERT_TRUE(backstage.addPage(other));

    EXPECT_TRUE(first->provider() == u8"plugin.a");
    EXPECT_TRUE(host->provider().empty());

    // 当前页在被撤除的那一组里：交给它后面的那一张。
    backstage.setCurrentPage(second);
    EXPECT_EQ(backstage.currentPage(), second);

    EXPECT_EQ(backstage.removePages(u8"plugin.a"), 2);
    EXPECT_EQ(backstage.numPages(), 2);
    EXPECT_EQ(backstage.pageAt(0), host);
    EXPECT_EQ(backstage.pageAt(1), other);
    EXPECT_EQ(backstage.currentPage(), other) << "被撤除的当前页由后一张接替";
    EXPECT_EQ(backstage.findPage(u8"test.backstage.a1"), nullptr);

    auto* nav = backstage.impl<QWidget>()->findChild<QListWidget*>();
    ASSERT_NE(nav, nullptr) << "导航列表";
    EXPECT_EQ(nav->count(), 2) << "导航行随页面一起撤除";

    // 空 provider 与不存在的 provider 都不动别人的页。
    EXPECT_EQ(backstage.removePages(u8""), 0);
    EXPECT_EQ(backstage.removePages(u8"plugin.missing"), 0);
    EXPECT_EQ(backstage.numPages(), 2);
    EXPECT_EQ(backstage.pageAt(0), host);

    // 撤掉的页可以重新加入：widget 已归还给页面（不是被删掉）。
    EXPECT_TRUE(backstage.addPage(first));
    EXPECT_EQ(backstage.numPages(), 3);
    EXPECT_EQ(backstage.pageAt(2), first);
    EXPECT_EQ(backstage.findPage(u8"test.backstage.a1"), first);
}

// “关于”面板是渲染器：内容来自 AppInfo，身份/构建事实/链接/致谢都由它排出来。
TEST_F(GuiTest, AboutPanel_ShowsTheApplicationInfo)
{
    vn::appfw::AppInfo info;
    info.name        = u8"Vine";
    info.version     = u8"1.0.0";
    info.description = u8"模块化应用框架";
    info.copyright   = u8"© 2026 Vine";
    info.license     = u8"MIT";
    info.build_facts = { { u8"Qt", u8"6.7.2" } };
    info.links       = { { u8"主页", u8"https://example.invalid/home" } };
    info.credits     = u8"spdlog\ngtest";

    guifw::AboutPanel panel;
    panel.setAppInfo(info);

    EXPECT_TRUE(panel.appInfo().name == u8"Vine");

    auto* widget = panel.impl<QWidget>();
    ASSERT_NE(widget, nullptr);

    auto labelTexts = [widget] {
        QStringList texts;
        for (auto* label : widget->findChildren<QLabel*>()) {
            texts << label->text();
        }
        return texts;
    };

    const QStringList shown = labelTexts();
    EXPECT_TRUE(shown.contains(QStringLiteral("Vine")));
    EXPECT_TRUE(shown.contains(QStringLiteral("1.0.0")));
    EXPECT_TRUE(shown.contains(QStringLiteral("6.7.2"))) << "构建事实行";
    EXPECT_TRUE(shown.contains(QStringLiteral("spdlog\ngtest"))) << "致谢块";
    EXPECT_TRUE(shown.contains(QStringLiteral("© 2026 Vine · MIT"))) << "版权行带许可证";
    EXPECT_TRUE(shown.join(QLatin1Char('\n')).contains(QStringLiteral("example.invalid"))) << "链接行";

    // 换一份 info：上一份的细节不能留在屏幕上。
    vn::appfw::AppInfo other;
    other.name = u8"Other";
    panel.setAppInfo(other);

    const QStringList after = labelTexts();
    EXPECT_TRUE(after.contains(QStringLiteral("Other")));
    EXPECT_FALSE(after.join(QLatin1Char('\n')).contains(QStringLiteral("example.invalid")));
    EXPECT_FALSE(after.contains(QStringLiteral("6.7.2")));
}

// 对话框只是面板的薄壳：默认显示进程的应用信息（名字回落到 AppConfig::name）。
TEST_F(GuiTest, AboutDialog_ShowsTheApplicationInfo)
{
    guifw::AboutDialog dialog;
    EXPECT_TRUE(dialog.appInfo().name == u8"test_gui") << "默认读 Application::appInfo()";

    vn::appfw::AppInfo info;
    info.name    = u8"Demo";
    info.version = u8"2.1";
    dialog.setAppInfo(info);
    EXPECT_TRUE(dialog.appInfo().name == u8"Demo");

    auto* widget = dialog.impl<QWidget>();
    ASSERT_NE(widget, nullptr);

    bool name_shown    = false;
    bool version_shown = false;
    for (auto* label : widget->findChildren<QLabel*>()) {
        name_shown    = name_shown || label->text() == QStringLiteral("Demo");
        version_shown = version_shown || label->text() == QStringLiteral("2.1");
    }
    EXPECT_TRUE(name_shown);
    EXPECT_TRUE(version_shown);
}

// 条目可以按动作隐藏/恢复：序号稳定，动作与命令都还在。
TEST_F(GuiTest, BackstagePage_ItemsCanBeHidden)
{
    guifw::BackstagePage page;

    auto* first = new guifw::RibbonAction();
    first->setText(u8"First");
    auto* second = new guifw::RibbonAction();
    second->setText(u8"Second");
    auto* unknown = new guifw::RibbonAction();
    unknown->setText(u8"Unknown");

    page.addItem(first);
    page.addItem(second);
    ASSERT_EQ(page.numItems(), 2);
    page.addItem(first); // 一个动作一个条目：重复添加被忽略
    EXPECT_EQ(page.numItems(), 2) << "动作是条目的身份";
    EXPECT_TRUE(page.isItemVisible(first));

    auto* page_widget = page.impl<QWidget>();
    ASSERT_NE(page_widget, nullptr);
    QToolButton* first_button = nullptr;
    for (QToolButton* button : page_widget->findChildren<QToolButton*>()) {
        if (button->defaultAction() == first->impl<QAction>()) {
            first_button = button;
        }
    }
    ASSERT_NE(first_button, nullptr) << "第一条目的按钮";

    page.setItemVisible(first, false);
    EXPECT_FALSE(page.isItemVisible(first));
    EXPECT_TRUE(page.isItemVisible(second));
    EXPECT_EQ(page.numItems(), 2) << "隐藏不删除条目";
    EXPECT_EQ(page.itemAt(0), first) << "序号保持稳定";
    EXPECT_TRUE(first_button->isHidden()) << "隐藏 = 按钮不参与布局";

    // 页面没有的动作、null 都是空操作。
    page.setItemVisible(unknown, false);
    EXPECT_FALSE(page.isItemVisible(unknown));
    page.setItemVisible(nullptr, false);
    EXPECT_EQ(page.numItems(), 2);

    page.setItemVisible(first, true);
    EXPECT_TRUE(page.isItemVisible(first));
    EXPECT_FALSE(first_button->isHidden());
}

TEST_F(GuiTest, Backstage_FollowsMainWindowSize)
{
    auto* backstage = new guifw::Backstage(wnd.get());
    auto* page      = new guifw::BackstagePage();
    page->setTitle(u8"Size");
    ASSERT_TRUE(backstage->addPage(page));

    wnd->setBackstage(backstage);
    backstage->open();
    ASSERT_TRUE(backstage->isOpen());

    auto* main_window = wnd->impl<QWidget>();
    auto* window      = backstage->impl<QWidget>();
    ASSERT_NE(main_window, nullptr);
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->size(), main_window->size());

    main_window->resize(main_window->width() + 160, main_window->height() + 120);
    QCoreApplication::processEvents();
    EXPECT_EQ(window->size(), main_window->size()) << "主窗口改大小后 Backstage 必须跟随";

    backstage->close();
    EXPECT_FALSE(backstage->isOpen());
}

TEST_F(GuiTest, Backstage_ClosesOnEscapeOrClose)
{
    guifw::Backstage backstage(wnd.get());

    auto* page = new guifw::BackstagePage();
    page->setTitle(u8"Close");
    ASSERT_TRUE(backstage.addPage(page));

    backstage.open();
    ASSERT_TRUE(backstage.isOpen());

    auto* window = backstage.impl<QWidget>();
    ASSERT_NE(window, nullptr);
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(window, &escape);
    QCoreApplication::processEvents();
    EXPECT_FALSE(backstage.isOpen()) << "Esc 必须关闭 Backstage";

    backstage.open();
    ASSERT_TRUE(backstage.isOpen());
    backstage.close();
    EXPECT_FALSE(backstage.isOpen());
}

TEST_F(GuiTest, Backstage_IsOwnedByMainWindow)
{
    auto* first = new guifw::Backstage(wnd.get());
    wnd->setBackstage(first);
    EXPECT_EQ(wnd->backstage(), first);

    // 替换必须清掉 Ribbon 的订阅再删除旧对象（不崩溃、指针更新）。
    auto* second = new guifw::Backstage(wnd.get());
    wnd->setBackstage(second);
    EXPECT_EQ(wnd->backstage(), second);
    EXPECT_EQ(wnd->ribbonBar()->backstage(), second);

    wnd->setBackstage(nullptr);
    EXPECT_EQ(wnd->backstage(), nullptr);
    EXPECT_EQ(wnd->ribbonBar()->backstage(), nullptr);
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
    EXPECT_TRUE(btnPaste->isEnabled());
    EXPECT_FALSE(btnPaste->isCheckable());
    EXPECT_FALSE(btnPaste->isChecked());
    EXPECT_TRUE(btnPaste->tooltip().empty());
}

TEST_F(GuiTest, RibbonButton_StyleAndState)
{
    EXPECT_EQ(btnStyle->buttonSize(), guifw::RibbonItemSize::Small);
    EXPECT_EQ(btnStyle->style(), guifw::RibbonButtonStyle::TextBesideIcon);
    EXPECT_TRUE(btnStyle->iconRightText());
    EXPECT_TRUE(btnStyle->tooltip() == u8"style tooltip");
    EXPECT_TRUE(btnStyle->isCheckable());
    EXPECT_TRUE(btnStyle->isChecked());
    EXPECT_TRUE(btnStyle->isEnabled());
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
    int  clicks       = 0;
    auto subscription = btnStyle->clicked.connect([&clicks](guifw::RibbonButton&, vn::EventArgs&) { ++clicks; });

    auto* tb = btnStyle->impl<SARibbonToolButton>();
    ASSERT_NE(tb, nullptr);
    tb->click();
    EXPECT_EQ(clicks, 1);
    tb->click();
    tb->click();
    EXPECT_EQ(clicks, 3);

    subscription.disconnect();
    tb->click();
    EXPECT_EQ(clicks, 3); // 移除后不再触发
}

TEST_F(GuiTest, RibbonGroup_Methods)
{    // ---- 布局模式（默认值取决于 SARibbon 全局风格，显式设置后再读回）----
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
    EXPECT_FALSE(ctrl->isEnabled());
    ctrl->setEnabled(true);
    EXPECT_TRUE(ctrl->isEnabled());

    ctrl->setVisible(false);
    EXPECT_FALSE(ctrl->isVisible());

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
    auto* cfg = new vn::appfw::ConfigManager();

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
    auto* cfg2 = new vn::appfw::ConfigManager();
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
    auto* cfg3 = new vn::appfw::ConfigManager();
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
    using vn::appfw::ConfigManager;

    auto* cfg = new ConfigManager();
    cfg->setString(u8"name", u8"Vine");
    cfg->setInt(u8"max", 100);

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vn_config_save_test";
    std::error_code             ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / "config.json";
    const vn::String            file_path(file.u8string());

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
    EXPECT_FALSE(cfg->save(vn::String((dir / "missing" / "config.json").u8string())));
    EXPECT_FALSE(cfg->save(vn::String(dir.u8string())));

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
    using vn::appfw::ConfigChangedEventArgs;
    using vn::appfw::ConfigManager;

    auto*      cfg       = new ConfigManager();
    int        events    = 0;
    bool       saw_empty = false;
    vn::String last_key;
    cfg->changed
        .connect([&](ConfigManager&, ConfigChangedEventArgs& e) {
            ++events;
            last_key  = e.key();
            saw_empty = e.key().empty();
        })
        .detach();

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
    auto* other = new ConfigManager();
    other->setInt(u8"a", 1);
    const int before_load = events;
    ASSERT_TRUE(cfg->loadJson(other->toJson()));
    EXPECT_EQ(events, before_load + 1);
    EXPECT_TRUE(saw_empty);
    const int before_same = events;
    ASSERT_TRUE(cfg->loadJson(other->toJson()));
    EXPECT_EQ(events, before_same);

    // clear：有值才通知，且用空 key 表示整体变化
    saw_empty              = false;
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
    using vn::appfw::ConfigManager;

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
    using vn::appfw::ConfigManager;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::StandardCategory;
    using vn::appfw::StandardGroup;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigManager;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::gui::ConfigWindow;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigManager;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::gui::ConfigWindow;

    ConfigRegistry reg;
    auto*          cat = reg.addCategory(u8"C1");
    auto*          g   = cat->addGroup(u8"G");
    ConfigItem     theme(u8"theme", u8"主题", ConfigItemType::Choice);
    theme.choices({
      { 1, u8"深色" },
      { 2, u8"浅色" }
    });
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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigManager;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::gui::ConfigWindow;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;

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
    using vn::String;
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;

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
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigRegistry;

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
    using vn::appfw::ConfigRegistry;

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
    auto*      cfg   = new vn::appfw::ConfigManager();
    int        fired = 0;
    vn::String lastKey;
    auto       subscription = cfg->changed.connect([&](vn::appfw::ConfigManager&, vn::appfw::ConfigChangedEventArgs& args) {
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

    subscription.disconnect();
    cfg->setInt(u8"after", 9);
    EXPECT_EQ(fired, 6); // 已移除 handler

    delete cfg;
}

TEST_F(GuiTest, PluginLoadContext_Configs)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);

    vn::appfw::PluginLoadContext ctx(app);
    EXPECT_EQ(ctx.application(), app);
    ASSERT_NE(ctx.configRegistry(), nullptr);
    EXPECT_EQ(ctx.configRegistry(), app->configRegistry());
    EXPECT_NE(ctx.eventBus(), nullptr);
    EXPECT_EQ(ctx.eventBus(), app->eventBus());

    // Registering through the context targets the same registry as Application
    auto* pluginCat = ctx.configRegistry()->addCategory(u8"插件");
    ASSERT_NE(pluginCat, nullptr);
    pluginCat->addGroup(u8"常规")->addItem(vn::appfw::ConfigItem(u8"plugin.opt", u8"插件选项", vn::appfw::ConfigItemType::Bool));
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

    const auto pluginName = vn::String(u8"testPlugin");
    const auto otherName  = vn::String(u8"myCommand");
    const auto ownedName  = vn::String(u8"pluginCommand");

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
        }
        else if (info.name == otherName) {
            EXPECT_TRUE(info.owner.empty());
        }
    }

    // 清理注册，避免影响其它用例。
    cm->unregisterCommand(otherName);
    cm->unregisterCommand(ownedName);
}

TEST_F(GuiTest, CommandManager_MissingTargetIsRefusedBeforeTheBodyRuns)
{
    // 命令声明它需要哪一层，框架统一拒绝：命令体自己不再写一遍空链判断，结果里说清缺什么。
    // 今天派发还没有附带目标（ADR-2026-10-24 第 2 步），所以两次执行都应该被拒绝 —— 这正是
    // 控制台/无 GUI 宿主看到的形状。
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    cm->unregisterCommand(vn::String(u8"needs_document"));
    cm->unregisterCommand(vn::String(u8"needs_view"));
    ASSERT_TRUE(cm->registerCommand<NeedsDocumentCommand>(vn::String(u8"needs_document")));
    ASSERT_TRUE(cm->registerCommand<NeedsViewCommand>(vn::String(u8"needs_view")));

    const auto document_level = cm->executeCommandAndWait(vn::String(u8"needs_document"));
    EXPECT_EQ(document_level.status(), vn::appfw::CommandStatus::NotApplicable) << "缺文档时是 NotApplicable（解释），不是 Failed（失败）";
    EXPECT_FALSE(document_level.message().empty()) << "要说清缺什么";

    const auto view_level = cm->executeCommandAndWait(vn::String(u8"needs_view"));
    EXPECT_EQ(view_level.status(), vn::appfw::CommandStatus::NotApplicable);
    EXPECT_FALSE(view_level.message().empty());

    // 没有声明前提的命令照旧跑：拒绝只发生在声明过的那一条路上。
    cm->unregisterCommand(vn::String(u8"dummy"));
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(vn::String(u8"dummy")));
    EXPECT_EQ(cm->executeCommandAndWait(vn::String(u8"dummy")).status(), vn::appfw::CommandStatus::Success);

    cm->unregisterCommand(vn::String(u8"needs_document"));
    cm->unregisterCommand(vn::String(u8"needs_view"));
    cm->unregisterCommand(vn::String(u8"dummy"));
}

TEST_F(GuiTest, ConfigRegistry_StandardCategories)
{
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::StandardCategory;
    using vn::appfw::StandardGroup;

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
    EXPECT_TRUE(reg.addItem(StandardCategory::Logging,
                            StandardGroup::Console,
                            ConfigItem(u8"logging.console_enabled", u8"日志输出到控制台", ConfigItemType::Bool),
                            u8"app_shell"));
    EXPECT_NE(reg.item(u8"logging.console_enabled"), nullptr);

    // 重复 key 拒绝
    EXPECT_FALSE(
        reg.addItem(StandardCategory::Logging, StandardGroup::Console, ConfigItem(u8"logging.console_enabled", u8"x", ConfigItemType::Bool), u8"other"));
}

TEST_F(GuiTest, ConfigRegistry_Ownership)
{
    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::ConfigRegistry;
    using vn::appfw::StandardCategory;
    using vn::appfw::StandardGroup;

    ConfigRegistry reg;
    reg.addItem(StandardCategory::Logging,
                StandardGroup::Console,
                ConfigItem(u8"logging.console_enabled", u8"日志输出到控制台", ConfigItemType::Bool),
                u8"app_shell");
    reg.addItem(StandardCategory::Appearance, StandardGroup::Theme, ConfigItem(u8"appearance.theme", u8"主题", ConfigItemType::String), u8"app_shell");
    reg.addItem(StandardCategory::General, StandardGroup::Startup, ConfigItem(u8"general.maximize", u8"最大化启动", ConfigItemType::Bool), u8"other_plugin");

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

    vn::appfw::PluginLoadContext ctx(app, u8"my_plugin");
    EXPECT_TRUE(ctx.pluginName() == u8"my_plugin");

    using vn::appfw::ConfigItem;
    using vn::appfw::ConfigItemType;
    using vn::appfw::StandardCategory;
    using vn::appfw::StandardGroup;

    EXPECT_TRUE(
        ctx.registerConfigItem(StandardCategory::Logging, StandardGroup::Console, ConfigItem(u8"logging.file_path", u8"日志文件", ConfigItemType::String)));

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
    EXPECT_FALSE(vn::testFlag(panelLeft->features(), guifw::DockFeatures::Closable));
    EXPECT_TRUE(vn::testFlag(panelRight->features(), guifw::DockFeatures::Closable));
    EXPECT_TRUE(vn::testFlag(panelTop->features(), guifw::DockFeatures::Closable));
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

TEST_F(GuiTest, DockPanel_FloatRoundTripKeepsArea)
{
    // 四个区域各往返一次：浮动时记住“当前”所在区域，停靠回来必须回到同一侧。
    struct Case {
        guifw::DockPanel* panel;
        guifw::DockAreas  area;
    };

    const Case cases[] = {
        { panelLeft,    guifw::DockAreas::Left   },
        { panelRight,   guifw::DockAreas::Right  },
        { panelTop,     guifw::DockAreas::Top    },
        { panelBottom2, guifw::DockAreas::Bottom }
    };

    for (const auto& c : cases) {
        ASSERT_EQ(c.panel->dockArea(), c.area);

        c.panel->setFloating(true);
        QCoreApplication::processEvents();
        EXPECT_TRUE(c.panel->isFloating());
        EXPECT_EQ(c.panel->dockArea(), guifw::DockAreas::None);

        // 浮窗不能小到抓不住：库侧给浮动面板加了下限尺寸。
        auto* w = c.panel->impl<QWidget>();
        ASSERT_NE(w, nullptr);
        EXPECT_GE(w->minimumWidth(), DockingPaneGeometry::minimumWidth);
        EXPECT_GE(w->minimumHeight(), DockingPaneGeometry::minimumHeight);

        // “标题栏留在屏幕内”只对窗口成立：浮动面板必须是独立窗口，
        // 否则 keepTitleBarReachable() 会把它当成停靠中的子控件跳过。
        EXPECT_TRUE(w->isWindow()) << "浮窗必须是独立窗口";

        c.panel->setFloating(false);
        QCoreApplication::processEvents();
        EXPECT_FALSE(c.panel->isFloating());
        EXPECT_EQ(c.panel->dockArea(), c.area);

        // 停靠回去后释放下限，否则分割条压不小它。
        EXPECT_EQ(w->minimumWidth(), 0);
        EXPECT_EQ(w->minimumHeight(), 0);
    }
}

TEST_F(GuiTest, DockPanel_TabDetachCollapsesGroup)
{
    // panelBottom 与 panelBottom2 共用底部区域，属于同一个 tab 组。
    ASSERT_TRUE(panelBottom->isTabbed());
    ASSERT_TRUE(panelBottom2->isTabbed());

    // “把一个 tab 拖出去变成浮窗”（这里走等价的公共 API）：组里只剩一个面板，
    // 组必须折叠回普通停靠面板，否则会留下一个只剩标题的空 tab 条。
    panelBottom2->setFloating(true);
    QCoreApplication::processEvents();

    EXPECT_TRUE(panelBottom2->isFloating());
    EXPECT_EQ(panelBottom2->dockArea(), guifw::DockAreas::None);

    EXPECT_FALSE(panelBottom->isFloating());
    EXPECT_FALSE(panelBottom->isCollapsed());
    EXPECT_EQ(panelBottom->dockArea(), guifw::DockAreas::Bottom);

    // 拖出去的面板还能停靠回底部。
    panelBottom2->setFloating(false);
    QCoreApplication::processEvents();
    EXPECT_FALSE(panelBottom2->isFloating());
    EXPECT_EQ(panelBottom2->dockArea(), guifw::DockAreas::Bottom);
}

TEST_F(GuiTest, DockPanelManager_RemoveDockPanel)
{
    auto* mgr = wnd->dockPanelManager();
    ASSERT_EQ(mgr->count(), 5);
    // 关掉一个可关闭面板：包装对象与容器一起销毁，管理器里不留残余。
    mgr->removeDockPanel(panelBottom);
    QCoreApplication::processEvents();
    EXPECT_EQ(mgr->count(), 4);
    EXPECT_EQ(mgr->findById(u8"dock_output"), nullptr);
    EXPECT_EQ(mgr->findByTitle(u8"Output"), nullptr);

    // 同组的另一个面板仍然可用（关闭回调 / userData 没有悬空）。
    EXPECT_FALSE(panelBottom2->isFloating());
    EXPECT_EQ(panelBottom2->dockArea(), guifw::DockAreas::Bottom);

    auto* survivor = panelBottom2->impl<QWidget>();
    ASSERT_NE(survivor, nullptr);
    EXPECT_FALSE(panelBottom2->isCollapsed());
    EXPECT_TRUE(survivor->isVisible()) << "关掉同组的一个 tab 后，剩下的面板不能变成不可见";

    // 其余面板依旧可以正常浮动/停靠。
    panelRight->setFloating(true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(panelRight->isFloating());

    panelRight->setFloating(false);
    QCoreApplication::processEvents();
    EXPECT_EQ(panelRight->dockArea(), guifw::DockAreas::Right);
}

TEST_F(GuiTest, DockPanel_TitleBarClickDoesNotChangeLayout)
{
    // 单击标题栏只是“激活/聚焦”，不是一次拖放：库在按下时就开始拖动态，松手时只要
    // 命中某个停靠指示器就会真的重新停靠；标题栏恰好压在指示器上时，一次普通点击
    // 就会把面板从原位置挪走（分割条尺寸错乱 → 面板看不见，但空间还在）。
    struct Case {
        guifw::DockPanel* panel;
        guifw::DockAreas  area;
        const char*       name;
    };

    // 让窗口比屏幕还大（等价于窗口落在副屏/负坐标屏幕上）：停靠面板的父级局部坐标
    // 会落在“可用屏幕矩形”之外。单击标题栏只是激活面板，绝不能用浮动窗的
    // “标题栏留在屏幕内”规则去移动一个停靠中的子控件 —— 那会把它挪出分割格，
    // 表现为面板看不见、但空间还在，直到下一次布局（改窗口大小）才回来。
    auto*    native_window = wnd->impl<QWidget>();
    QScreen* screen        = QGuiApplication::primaryScreen();
    ASSERT_NE(native_window, nullptr);
    ASSERT_NE(screen, nullptr);
    const QRect available = screen->availableGeometry();
    native_window->resize(qMax(available.width() + 600, 1400), qMax(available.height() + 400, 1000));
    QCoreApplication::processEvents();

    const Case cases[] = {
        { panelLeft,  guifw::DockAreas::Left,  "left"  },
        { panelRight, guifw::DockAreas::Right, "right" },
        { panelTop,   guifw::DockAreas::Top,   "top"   }
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);

        auto* pane = c.panel->impl<QWidget>();
        ASSERT_NE(pane, nullptr);

        QWidget* title = nullptr;
        for (QWidget* w : pane->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockingPaneTitleWidget")) {
                title = w;
                break;
            }
        }
        ASSERT_NE(title, nullptr);

        const QWidget* parent_before   = pane->parentWidget();
        const QRect    geometry_before = pane->geometry();

        EXPECT_FALSE(pane->isWindow()) << "停靠状态的面板必须是子控件，不是独立窗口";

        const QPoint local  = title->rect().center();
        const QPoint global = title->mapToGlobal(local);

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(title, &press);

        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(title, &release);

        QCoreApplication::processEvents();

        EXPECT_FALSE(c.panel->isFloating());
        EXPECT_EQ(c.panel->dockArea(), c.area);
        EXPECT_EQ(pane->parentWidget(), parent_before) << "单击标题栏不能把面板挪出原来的父级";
        EXPECT_EQ(pane->geometry(), geometry_before) << "单击标题栏不能改变面板几何";
        EXPECT_TRUE(pane->isVisible()) << "单击标题栏后面板必须仍然可见";
    }
}

TEST_F(GuiTest, DockPanel_PinnedFlyoutIsAnchoredToDockArea)
{
    struct Case {
        guifw::DockPanel* panel;
        const char*       side;
    };

    const Case cases[] = {
        { panelLeft,  "left"  },
        { panelRight, "right" },
        { panelTop,   "top"   }
    };

    // 底部两个面板同属一个 tab 组，直接 pin 组内面板不生效：先把同组的面板移出。
    panelBottom2->setFloating(true);
    QCoreApplication::processEvents();
    ASSERT_FALSE(panelBottom->isTabbed());

    for (const auto& c : cases) {
        SCOPED_TRACE(c.side);

        // 给面板一个最小尺寸很大的内容：内容比飞窗本身还“大”时（例如 flyoutSize
        // 过期或为空，飞窗只能拿到最小 50px），贴边的几何仍然必须成立。
        auto* content_widget = new QPlainTextEdit();
        content_widget->setMinimumSize(420, 260);
        c.panel->setContent(new guifw::Control(content_widget));
        QCoreApplication::processEvents();

        c.panel->pin();
        QCoreApplication::processEvents();
        ASSERT_TRUE(c.panel->isPinned());

        // 自动隐藏按钮出现在对应的边上。
        QAbstractButton* button = nullptr;
        for (QWidget* w : wnd->impl<QWidget>()->findChildren<QWidget*>()) {
            if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockAutoHideButton") && w->isVisible()) {
                button = qobject_cast<QAbstractButton*>(w);
                break;
            }
        }
        ASSERT_NE(button, nullptr) << "折叠后应出现自动隐藏按钮";

        // click() 是 QAbstractButton 的公共 API，等价于用户点一下按钮。
        // 这里不再泵事件：飞窗是同步创建的，而空闲事件会让失焦回调立刻把它回收
        // （offscreen 平台下没有真实焦点）。
        button->click();

        QWidget* flyout = nullptr;
        for (QWidget* w : QApplication::topLevelWidgets()) {
            if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockingPaneFlyoutWidget") && w->isVisible()) {
                flyout = w;
                break;
            }
        }
        ASSERT_NE(flyout, nullptr) << "点击按钮应弹出飞窗";

        // 飞窗挂在停靠区上：Left/Top 贴近边、Right/Bottom 贴远边，四周内缩 5px，
        // 远边再留 9px 给自动隐藏条（库的原有几何规则）。
        QWidget* host = flyout->parentWidget();
        ASSERT_NE(host, nullptr);

        const QRect   host_rect(host->mapToGlobal(QPoint(0, 0)), host->size());
        const QRect   flyout_rect(flyout->pos(), flyout->size());
        const QString side = QString::fromLatin1(c.side);

        if (side == QLatin1String("left")) {
            EXPECT_EQ(flyout_rect.left(), host_rect.left() + 5);
            EXPECT_EQ(flyout_rect.top(), host_rect.top() + 5);
            EXPECT_EQ(flyout_rect.bottom(), host_rect.bottom() - 5);
        }
        else if (side == QLatin1String("right")) {
            EXPECT_EQ(flyout_rect.right(), host_rect.right() - 5);
            EXPECT_EQ(flyout_rect.top(), host_rect.top() + 5);
            EXPECT_EQ(flyout_rect.bottom(), host_rect.bottom() - 5);
        }
        else if (side == QLatin1String("top")) {
            EXPECT_EQ(flyout_rect.top(), host_rect.top() + 5);
            EXPECT_EQ(flyout_rect.left(), host_rect.left() + 5);
            EXPECT_EQ(flyout_rect.right(), host_rect.right() - 5);
        }
        else {
            EXPECT_EQ(flyout_rect.bottom(), host_rect.bottom() - 5);
            EXPECT_EQ(flyout_rect.left(), host_rect.left() + 5);
            EXPECT_EQ(flyout_rect.right(), host_rect.right() - 5);
        }

        c.panel->unpin();
        QCoreApplication::processEvents();
        EXPECT_FALSE(c.panel->isPinned());
    }

    // 底部：单独 pin 底边面板，飞窗应贴住停靠区的下沿。
    auto* bottom_content = new QPlainTextEdit();
    bottom_content->setMinimumSize(420, 260);
    panelBottom->setContent(new guifw::Control(bottom_content));
    QCoreApplication::processEvents();

    panelBottom->pin();
    QCoreApplication::processEvents();
    ASSERT_TRUE(panelBottom->isPinned());

    QAbstractButton* bottom_button = nullptr;
    for (QWidget* w : wnd->impl<QWidget>()->findChildren<QWidget*>()) {
        if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockAutoHideButton") && w->isVisible()) {
            bottom_button = qobject_cast<QAbstractButton*>(w);
            break;
        }
    }
    ASSERT_NE(bottom_button, nullptr);

    bottom_button->click();

    QWidget* bottom_flyout = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockingPaneFlyoutWidget") && w->isVisible()) {
            bottom_flyout = w;
            break;
        }
    }
    ASSERT_NE(bottom_flyout, nullptr);

    QWidget* bottom_host = bottom_flyout->parentWidget();
    ASSERT_NE(bottom_host, nullptr);

    const QRect bottom_host_rect(bottom_host->mapToGlobal(QPoint(0, 0)), bottom_host->size());
    const QRect bottom_flyout_rect(bottom_flyout->pos(), bottom_flyout->size());

    EXPECT_EQ(bottom_flyout_rect.bottom(), bottom_host_rect.bottom() - 5);
    EXPECT_EQ(bottom_flyout_rect.left(), bottom_host_rect.left() + 5);
    EXPECT_EQ(bottom_flyout_rect.right(), bottom_host_rect.right() - 5);

    panelBottom->unpin();
    QCoreApplication::processEvents();
    EXPECT_FALSE(panelBottom->isPinned());
}

// ============================ 标题栏配色 ============================

namespace
{

/// 把“焦点落到了谁身上”直接告诉库：Tab 组与标题栏都订阅 `QApplication::focusChanged`，
/// 而用例不能依赖测试平台真的把窗口激活（offscreen / 远程桌面下这不可靠）。
void sendFocusChange(QWidget* from, QWidget* to)
{
    QMetaObject::invokeMethod(qApp, "focusChanged", Qt::DirectConnection, Q_ARG(QWidget*, from), Q_ARG(QWidget*, to));
}

/// 按类名找库内部的 Tab 组容器（它的头文件不进测试目标，只认 QWidget）。
QWidget* findTabbedGroup(QWidget* root)
{
    for (QWidget* w : root->findChildren<QWidget*>()) {
        if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String("DockingPaneTabbedContainer")) {
            return w;
        }
    }

    return nullptr;
}

} // namespace

// Tab 组的标题栏也要随激活变色：组的 paintEvent 自己画边框与底部 Tab 条，
// 基类画标题栏背景那一步不会替它执行 —— 漏掉这一步时，组内标题栏永远是“未激活”色。
TEST_F(GuiTest, DockPanel_TabGroupHeaderShowsTheActiveState)
{
    if (DockingPaneTheme::activeHeaderColor() == DockingPaneTheme::inactiveHeaderColor()) {
        GTEST_SKIP() << "当前调色板的 Base 与 Window 同色，无法区分激活态";
    }

    ASSERT_TRUE(panelBottom->isTabbed());
    ASSERT_TRUE(panelBottom2->isTabbed());

    QWidget* group = findTabbedGroup(wnd->impl<QWidget>());
    ASSERT_NE(group, nullptr) << "Tab 组容器";

    QWidget* header = group->findChild<QWidget*>(QStringLiteral("headerWidget"));
    ASSERT_NE(header, nullptr) << "标题栏";

    // 标题文字从 x=5 开始，边框在 x=0..1：x=3 一定是纯背景。
    const QPoint probe(3, header->geometry().center().y());

    const auto headerColour = [group, probe] {
        QImage image(group->size(), QImage::Format_ARGB32);

        image.fill(Qt::transparent);
        group->render(&image);

        return image.pixelColor(probe);
    };

    // 组内任意子控件拿到焦点 = 这一组是活动组（库自己的判定就是 isAncestorOf()）。
    sendFocusChange(nullptr, header);
    QCoreApplication::processEvents();
    EXPECT_EQ(headerColour(), DockingPaneTheme::activeHeaderColor());

    // 焦点交给组外（主窗口）= 未激活。
    sendFocusChange(header, wnd->impl<QWidget>());
    QCoreApplication::processEvents();
    EXPECT_EQ(headerColour(), DockingPaneTheme::inactiveHeaderColor());

    // 把焦点状态还原成真实值，后面的用例不受影响。
    sendFocusChange(wnd->impl<QWidget>(), QApplication::focusWidget());
}

// ============================ 浮窗几何 ============================
TEST(DockGeometryTest, FloatingMinimum)
{
    QWidget pane;
    EXPECT_EQ(pane.minimumWidth(), 0);

    DockingPaneGeometry::applyFloatingMinimum(&pane);
    EXPECT_EQ(pane.minimumWidth(), DockingPaneGeometry::minimumWidth);
    EXPECT_EQ(pane.minimumHeight(), DockingPaneGeometry::minimumHeight);

    // 调用方已经要求了更大的下限，不能被改小。
    pane.setMinimumSize(DockingPaneGeometry::minimumWidth * 3, DockingPaneGeometry::minimumHeight * 3);
    DockingPaneGeometry::applyFloatingMinimum(&pane);
    EXPECT_EQ(pane.minimumWidth(), DockingPaneGeometry::minimumWidth * 3);
    EXPECT_EQ(pane.minimumHeight(), DockingPaneGeometry::minimumHeight * 3);

    DockingPaneGeometry::clearFloatingMinimum(&pane);
    EXPECT_EQ(pane.minimumWidth(), 0);
    EXPECT_EQ(pane.minimumHeight(), 0);
}

TEST(DockGeometryTest, TitleBarStaysReachable)
{
    QScreen* screen = QGuiApplication::primaryScreen();
    ASSERT_NE(screen, nullptr);

    QWidget pane;
    pane.resize(200, 150);
    pane.show();
    QCoreApplication::processEvents();

    const QRect available   = screen->availableGeometry();
    const int   titleHeight = 24;

    // 标题栏矩形（位置会变，每次判前重算）。
    const auto titleBarRect = [&pane, titleHeight]() { return QRect(pane.pos(), QSize(pane.width(), titleHeight)); };

    // 完全拖到屏幕外的右下角：拉回来，标题栏必须留在可用区域内。
    pane.move(available.right() + 400, available.bottom() + 400);
    if (available.contains(pane.pos())) {
        GTEST_SKIP() << "窗口管理器不允许把窗口移出屏幕";
    }
    EXPECT_TRUE(DockingPaneGeometry::keepTitleBarReachable(&pane, titleHeight));
    EXPECT_TRUE(available.intersects(titleBarRect()));

    // 已经在屏幕内：不动。
    pane.move(available.left() + 10, available.top() + 10);
    EXPECT_FALSE(DockingPaneGeometry::keepTitleBarReachable(&pane, titleHeight));

    // 只露出一点点：也拉回来。
    pane.move(available.right() - 1, available.top() + 10);
    EXPECT_TRUE(DockingPaneGeometry::keepTitleBarReachable(&pane, titleHeight));
    EXPECT_TRUE(available.intersects(titleBarRect()));
}

TEST(DockGeometryTest, KeepTitleBarReachableDoesNotMoveChildWidgets)
{
    // 停靠中的面板是子控件：它的坐标是父级局部坐标，而屏幕矩形是全局坐标。
    // 若把浮动窗的“标题栏留在屏幕内”规则用在它身上，就会把面板移出分割格
    // （看不见、但空间还在，直到下一次布局）。
    QWidget host;
    host.resize(400, 300);

    QWidget child(&host);
    child.setGeometry(2000, 2000, 150, 100);
    EXPECT_FALSE(child.isWindow());

    EXPECT_FALSE(DockingPaneGeometry::keepTitleBarReachable(&child, 24));
    EXPECT_EQ(child.pos(), QPoint(2000, 2000));
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
        vn::appfw::ProgressHost host;
        host.setForeground(true); // 只有前台宿主才让应用处于忙态
        EXPECT_TRUE(vn::appfw::ProgressHost::isActive());
        EXPECT_TRUE(app->isBusy());
        EXPECT_TRUE(vn::appfw::ProgressHost::current() == &host);
    }
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());
}

TEST_F(GuiTest, CommandManager_LongRunningCreatesAmbientHost)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"longCmd");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<LongCommand>(name));

    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());

    // Task 是惰性的：放到工作线程上跑，主线程轮询观察宿主生命周期。
    auto                     task = cm->executeCommandAsync(name);
    std::atomic<bool>        done{ false };
    vn::appfw::CommandResult result;
    std::thread              runner([&] {
        result = std::move(task).result();
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!vn::appfw::ProgressHost::isActive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 长命令运行期间，环境宿主已挂载、应用处于忙态。
    EXPECT_TRUE(vn::appfw::ProgressHost::isActive());
    EXPECT_TRUE(app->isBusy());

    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    runner.join();
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    // 链结束后宿主释放、应用恢复空闲。
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());

    cm->unregisterCommand(name);
}

TEST_F(GuiTest, CommandManager_BusyGateRejectsNewCommand)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"busyDummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    // 空闲时可执行。
    auto ok = cm->executeCommandAndWait(name);
    EXPECT_EQ(ok.status(), vn::appfw::CommandStatus::Success);

    {
        // 外部前台长任务持有宿主（模拟用户触发的耗时操作）。
        vn::appfw::ProgressHost host;
        host.setForeground(true);
        EXPECT_TRUE(app->isBusy());

        // 忙时新命令被拒并提示。
        auto busy = cm->executeCommandAndWait(name);
        EXPECT_EQ(busy.status(), vn::appfw::CommandStatus::Failed);
        EXPECT_EQ(busy.message(), vn::String(u8"另一个操作正在进行中，请稍候。"));
    }

    // 宿主释放后可再次执行。
    auto after = cm->executeCommandAndWait(name);
    EXPECT_EQ(after.status(), vn::appfw::CommandStatus::Success);

    cm->unregisterCommand(name);
}

TEST(ProgressPresenterTest, ShowsBarForActiveHostAndHidesAfter)
{
    guifw::ProgressPresenter presenter;

    // 无宿主：不忙、隐藏。
    EXPECT_FALSE(presenter.isBusy());
    EXPECT_FALSE(presenter.isVisible());

    {
        vn::appfw::ProgressHost host;
        host.setForeground(true); // 前台操作驱动主进度条
        std::thread worker([&] {
            vn::progress::ProgressScope scope(host.range(), "Exporting", 30);
            for (int i = 0; i < 30; ++i) {
                scope.next(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });

        // 超过显示阈值后应出现进度条。
        pumpEventsFor(700);
        EXPECT_TRUE(presenter.isBusy());
        EXPECT_TRUE(presenter.isVisible());

        worker.join();
    }

    // 宿主结束后，延迟隐藏。
    pumpEventsFor(500);
    EXPECT_FALSE(presenter.isBusy());
    EXPECT_FALSE(presenter.isVisible());
}

// 无 GUI 时的进度消费者：ConsoleUserIO 用它把 LongRunning 命令的进度打到控制台。
// 这里手动调 poll()，所以既不依赖计时器也不依赖事件循环，节流规则可确定性断言。
TEST(ConsoleProgressReporterTest, WritesThrottledLinesWhileAForegroundOperationRuns)
{
    std::vector<vn::String>                     lines;
    vn::appfw::ConsoleProgressReporter::Options options;
    options.interval     = std::chrono::milliseconds(0); // 手动步进不该被最小行距节流
    options.show_delay   = std::chrono::milliseconds(0);
    options.step_percent = 10;

    vn::appfw::ConsoleProgressReporter reporter([&lines](const vn::String& line) { lines.push_back(line); }, options);

    // 没有前台宿主 ⇒ 什么都不打印。
    reporter.poll();
    EXPECT_TRUE(lines.empty());

    {
        vn::appfw::ProgressHost host;
        host.setForeground(true);
        host.setLabel(u8"导出");

        // show_delay 为 0 ⇒ 首行立刻出来，且带标签。
        reporter.poll();
        ASSERT_EQ(lines.size(), 1u);
        EXPECT_NE(lines[0].find(u8"导出"), vn::String::npos);

        auto scope = host.scope(u8"导出", 100);
        scope.next(50);
        reporter.poll();
        ASSERT_EQ(lines.size(), 2u);
        EXPECT_NE(lines[1].find(u8"50"), vn::String::npos);

        // 进度没动 ⇒ 不重复打印（这就是节流）。
        reporter.poll();
        EXPECT_EQ(lines.size(), 2u);

        // 涨到 +10% ⇒ 再出一行。
        scope.next(10);
        reporter.poll();
        EXPECT_EQ(lines.size(), 3u);
    }

    // 操作结束 ⇒ 收尾一行，而不是静默收场。
    reporter.poll();
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_NE(lines[3].find(u8"已结束"), vn::String::npos);
}

// 通知驱动版本：start() 订阅变更，首行在呈现延迟到点后由定时器线程写出（无事件循环也能用），
// stop() 之后必须彻底安静。
TEST(ConsoleProgressReporterTest, ChangeNotificationWritesLinesAndStops)
{
    std::vector<vn::String>                     lines;
    std::mutex                                  lines_mutex;
    vn::appfw::ConsoleProgressReporter::Options options;
    options.interval     = std::chrono::milliseconds(10);
    options.show_delay   = std::chrono::milliseconds(0);
    options.step_percent = 10;

    vn::appfw::ConsoleProgressReporter reporter(
        [&lines, &lines_mutex](const vn::String& line) {
            std::lock_guard lock(lines_mutex);
            lines.push_back(line);
        },
        options);

    vn::appfw::ProgressHost host;
    host.setForeground(true);
    host.setLabel(u8"后台导出");

    reporter.start();

    // 等计时器至少写出一行（有界等待，避免用例挂死）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(lines_mutex);
            if (!lines.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard lock(lines_mutex);
        EXPECT_FALSE(lines.empty());
    }

    // stop() 是彻底的：之后不再有任何行。
    reporter.stop();
    std::size_t after_stop = 0;
    {
        std::lock_guard lock(lines_mutex);
        after_stop = lines.size();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    {
        std::lock_guard lock(lines_mutex);
        EXPECT_EQ(lines.size(), after_stop);
    }
}

// 唤醒帧可能由**上报线程**创建（进度是从命令所在线程上报的），所以它必须活过那个线程：
// 这里让一个短命线程触发被节流的一次上报并退出，到点后仍要写出那一行。
TEST(ConsoleProgressReporterTest, WakeupOutlivesTheThreadThatReported)
{
    std::vector<vn::String>                     lines;
    std::mutex                                  lines_mutex;
    vn::appfw::ConsoleProgressReporter::Options options;
    options.interval     = std::chrono::milliseconds(50);
    options.show_delay   = std::chrono::milliseconds(0);
    options.step_percent = 5;

    vn::appfw::ProgressHost host;
    host.setForeground(true);
    host.setLabel(u8"导出");

    vn::appfw::ConsoleProgressReporter reporter(
        [&lines, &lines_mutex](const vn::String& line) {
            std::lock_guard lock(lines_mutex);
            lines.push_back(line);
        },
        options);

    reporter.start();
    ASSERT_EQ(lines.size(), 1u); // show_delay 为 0 ⇒ start() 就把首行写出来了

    // 在短命线程里上报 10%：事件在该线程上触发，但此刻距上一行不到 interval，
    // 于是消费者把"到点再写"的一次性唤醒臂在那个线程上——那个线程马上就退出了。
    std::thread worker([&host] {
        vn::progress::ProgressScope scope = host.scope(u8"worker", 100);
        scope.next(10);
    });
    worker.join();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(lines_mutex);
            if (lines.size() >= 2) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::lock_guard lock(lines_mutex);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[1].find(u8"10"), vn::String::npos);
}

// 端到端：ConsoleUserIO 构造时就挂上了进度消费者，所以无头模式跑 LongRunning 命令也能看到
// 进度——在这一条之前，无头模式里进度是“什么都不显示”。
// 只有 ConsoleUserIO 析构之后才去读被捕获的 stdout：析构会 stop() 消费者，之后没有别的
// 写者，字符串流因此不会与定时器线程竞争。
TEST(ConsoleProgressReporterTest, ConsoleUserIOPrintsTheProgressOfAForegroundOperation)
{
    std::ostringstream captured;
    auto* const        previous_buffer = std::cout.rdbuf(captured.rdbuf());

    {
        vn::appfw::ConsoleUserIO io;

        {
            vn::appfw::ProgressHost host;
            host.setForeground(true);
            host.setLabel(u8"无头导出");

            // 默认节流：500ms 首字 + 200ms 轮询，留足余量到 1s。
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        // 宿主结束后，消费者会再补一行收尾。
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }

    std::cout.rdbuf(previous_buffer);

    const auto text = captured.str();
    EXPECT_NE(text.find("无头导出"), std::string::npos);
    EXPECT_NE(text.find("已结束"), std::string::npos);
}

// ── ConsoleUserIO 读路径（无头 stdin）：此前只有进度侧有覆盖 ───────────────
//
// 读路径是 一个分离的后台读线程 + 行队列 + 可唤醒事件（std::getline 阻塞且不可
// 中断），取消是关机的关键依赖（Application::shutdown() 先 cancelPendingInput()
// 再排空命令链）。这里用脚本化的 std::cin 缓冲把行、EOF、单飞拒绝与跨线程取消
// 全部钉成确定性事实。
//
// 缓冲是静态的、进程级且绝不还原：后台读线程可能比用例对象活得久，让线程去碰
// 一个已经析构的测试对象就是测试自己的 UB；碰静态对象则永远安全。
class ScriptedStdinBuf : public std::streambuf {
  public:
    /// 追加可读数据并唤醒阻塞中的读线程。
    void feed(const std::string& text)
    {
        {
            std::lock_guard lock(mutex_);
            data_ += text;
        }
        cv_.notify_all();
    }

    /// 数据读完（且不再追加）后给出 EOF：std::getline 以失败收尾，读线程退出。
    void set_eof()
    {
        {
            std::lock_guard lock(mutex_);
            eof_ = true;
        }
        cv_.notify_all();
    }

    /// 回到"无数据、无 EOF"（每个用例开头）。
    void reset()
    {
        std::lock_guard lock(mutex_);
        data_.clear();
        eof_ = false;
    }

    /// 等到至少 count 个线程卡在取字符上（卡住 ⇒ 读线程确实跑起来了）。
    bool waitBlockedAtLeast(int count, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(mutex_);
                if (blocked_ >= count) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::lock_guard lock(mutex_);
        return blocked_ >= count;
    }

    /// EOF 已交给某个读线程的次数；比之前多 1 ⇒ 该线程已从 getline 出来，
    /// 此后不会再碰这个缓冲（可以安全拆测试对象）。
    int eofReturns()
    {
        std::lock_guard lock(mutex_);
        return eof_returns_;
    }

    /// 等到 EOF 至少再多交付一次（有界，避免用例挂死）。
    bool waitEofDelivered(int previous_count, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (eofReturns() > previous_count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return eofReturns() > previous_count;
    }

  protected:
    int_type underflow() override
    {
        std::unique_lock lock(mutex_);
        ++blocked_;
        cv_.wait(lock, [this] { return !data_.empty() || eof_; });
        --blocked_;

        if (data_.empty()) {
            ++eof_returns_;
            return traits_type::eof();
        }

        ch_ = data_.front();
        data_.erase(0, 1);
        setg(&ch_, &ch_, &ch_ + 1);
        return traits_type::to_int_type(ch_);
    }

  private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::string             data_;
    bool                    eof_{ false };
    int                     blocked_{ 0 };
    int                     eof_returns_{ 0 };
    char                    ch_{ 0 };
};

/// 进程级脚本 stdin：首次调用把 std::cin 接到脚本缓冲上，并解开 cin→cout 的 tie
/// （后台线程每次 getline 都会 flush 它，而用例同时把 cout 抓到字符串流里——
/// 解开 tie 就没有两线程同写一个流的竞争）。
ScriptedStdinBuf& scriptedStdin()
{
    static ScriptedStdinBuf buf;
    static const bool       armed = [] {
        std::cin.tie(nullptr);
        std::cin.rdbuf(&buf);
        return true;
    }();
    static_cast<void>(armed);
    return buf;
}

/// 在临时目录里造一个用例专属目录（名字带序号，重复跑与并行跑都不冲突）。
std::filesystem::path makeTempCaseDir(const char* tag)
{
    static std::atomic<int> counter{ 0 };
    const auto              dir = std::filesystem::temp_directory_path() / ("vn_test_userio_" + std::string(tag) + "_" + std::to_string(++counter));

    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

/// 造一个非空文件：路径读只关心"在不在、叫什么名"，内容无关。
void touchFile(const std::filesystem::path& path)
{
    std::ofstream out(path, std::ios::binary);
    out << "vine";
}

/// 路径的 UTF-8 字节：脚本 stdin 与 vn::String 都按 UTF-8 走。
std::string narrow(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

class ConsoleUserIOReadTest : public ::testing::Test {
  protected:
    void SetUp() override
    {
        scriptedStdin(); // 首次调用完成 cin 的替换，之后是幂等的
        previous_eof_returns_ = scriptedStdin().eofReturns();
        scriptedStdin().reset();
        std::cin.clear(); // 上一轮 EOF 留下的 eofbit/failbit 必须清掉

        previous_cout_ = std::cout.rdbuf(captured_.rdbuf());
        io             = std::make_unique<vn::appfw::ConsoleUserIO>();
    }

    void TearDown() override
    {
        // 让后台读线程走到 EOF 并退出 getline：它是分离线程，只有 EOF 能放它走，
        // 而它可能正阻塞在脚本缓冲里——确认它最后一次用完缓冲之后，测试才可以拆。
        scriptedStdin().set_eof();
        EXPECT_TRUE(scriptedStdin().waitEofDelivered(previous_eof_returns_, std::chrono::seconds(5))) << "后台读线程没有在 EOF 后退出";

        io.reset();
        std::cout.rdbuf(previous_cout_);
    }

    std::unique_ptr<vn::appfw::ConsoleUserIO> io;
    std::ostringstream                        captured_;
    std::streambuf*                           previous_cout_{ nullptr };
    int                                       previous_eof_returns_{ 0 };
};

// 行、EOF 与提示上屏：第二行来自行队列（读线程在第一个读跑完前就把它读进来了），
// 第三次读必须以 nullopt 立即收尾——关机排空等待的正是这个收尾。
TEST_F(ConsoleUserIOReadTest, ReadsLinesAndReportsEndOfInput)
{
    scriptedStdin().feed("hello\nworld\n");
    scriptedStdin().set_eof();

    const auto first = io->getStringAsync(u8"输入名称> ").result();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, vn::String(u8"hello"));

    const auto second = io->getStringAsync().result();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, vn::String(u8"world"));

    const auto third = io->getStringAsync().result();
    EXPECT_FALSE(third.has_value()) << "EOF 之后的读必须立刻以 nullopt 收尾，而不是挂住";

    EXPECT_NE(captured_.str().find("输入名称> "), std::string::npos) << "提示必须写进 stdout";
}

// Console 的解析语义与 GUI 不同：坏行 ⇒ nullopt（GUI 是原地重新提示）。
// 这里同时钉住 U2 的范围规则：超出 int 的输入必须失败，绝不回绕。
TEST_F(ConsoleUserIOReadTest, ParsesIntsAndFailsOnBadInputWithoutReprompt)
{
    scriptedStdin().feed("42\n不是数字\n99999999999\n");
    scriptedStdin().set_eof();

    const auto ok = io->getIntAsync(u8"n> ").result();
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 42);

    const auto bad = io->getIntAsync().result();
    EXPECT_FALSE(bad.has_value()) << "非数字必须失败，而不是回绕或原地重提示";

    const auto overflow = io->getIntAsync().result();
    EXPECT_FALSE(overflow.has_value()) << "超出 int 的输入必须失败（U2 的范围规则）";
}

// 不变量 2（无头侧）：同一时刻最多一个读在等，第二个立刻以 nullopt 被拒。
TEST_F(ConsoleUserIOReadTest, SecondReadIsRefusedWhileOneIsPending)
{
    std::optional<vn::String> first;
    std::thread               runner([&] { first = io->getStringAsync(u8"first> ").result(); });

    // 后台读线程一跑起来，槽位就已经归第一个读：此后它只可能被输入或取消解开。
    ASSERT_TRUE(scriptedStdin().waitBlockedAtLeast(1, std::chrono::seconds(5)));

    const auto second = io->getStringAsync(u8"second> ").result();
    EXPECT_FALSE(second.has_value()) << "第二个读必须被单飞槽位拒绝";

    io->cancelPendingInput();
    runner.join();
    EXPECT_FALSE(first.has_value()) << "被取消的读以 nullopt 收尾";
}

// 不变量 3（无头侧）：cancelPendingInput() 可以从任意线程调用并解开等待中的读；
// 取消既不能吃掉之后到达的输入，也不能把取消状态泄漏给下一个读。
TEST_F(ConsoleUserIOReadTest, CancelFromAnotherThreadUnblocksAndKeepsArrivedLine)
{
    std::optional<vn::String> first;
    std::thread               runner([&] { first = io->getStringAsync(u8"parked> ").result(); });
    ASSERT_TRUE(scriptedStdin().waitBlockedAtLeast(1, std::chrono::seconds(5)));

    // Application::shutdown() 正是从别的线程这样调的。
    std::thread canceller([this] { io->cancelPendingInput(); });
    canceller.join();
    runner.join();
    EXPECT_FALSE(first.has_value()) << "取消必须解开等待中的读";

    scriptedStdin().feed("late\n");
    const auto second = io->getStringAsync().result();
    ASSERT_TRUE(second.has_value()) << "取消后的下一个读必须照常工作（取消状态不得泄漏）";
    EXPECT_EQ(*second, vn::String(u8"late"));
}

// 路径读的契约（无头侧）：提示里带上"要什么"，合法路径原样给出，不合法的只报**一次**原因然后结束。
// 重提示是 GUI 的交互模型，脚本里没有人可以重打。
TEST_F(ConsoleUserIOReadTest, FileReadTakesAnExistingFileAndReportsWhyItRefuses)
{
    const auto dir = makeTempCaseDir("file");
    const auto stl = dir / "mesh.stl";
    const auto txt = dir / "notes.txt";
    touchFile(stl);
    touchFile(txt);

    const vn::appfw::UserIO::FileReadOptions options{ { u8".stl" } };

    scriptedStdin().feed(narrow(stl) + "\n");
    const auto ok = io->getFileAsync(u8"网格> ", options).result();
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, stl);
    EXPECT_NE(captured_.str().find("网格（.stl）> "), std::string::npos) << "提示里要带上接受的后缀";

    // 后缀不符：报原因，读结束（不是重提示）。
    scriptedStdin().feed(narrow(txt) + "\n");
    const auto wrong_extension = io->getFileAsync(u8"网格> ", options).result();
    EXPECT_FALSE(wrong_extension.has_value());
    EXPECT_NE(captured_.str().find("后缀不支持（仅 .stl）"), std::string::npos);

    // 不存在：同样一行原因。
    scriptedStdin().feed(narrow(dir / "missing.stl") + "\n");
    const auto missing = io->getFileAsync(u8"网格> ", options).result();
    EXPECT_FALSE(missing.has_value());
    EXPECT_NE(captured_.str().find("文件不存在: "), std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 多个路径写在一行里：';' 分隔、顺序保持、空段忽略；任何一段不合法则整条读失败（绝不部分成功）。
TEST_F(ConsoleUserIOReadTest, MultiplePathsComeFromOneSemicolonSeparatedLine)
{
    const auto dir = makeTempCaseDir("files");
    const auto one = dir / "one.stl";
    const auto two = dir / "two.stl";
    touchFile(one);
    touchFile(two);

    const vn::appfw::UserIO::FileReadOptions options{ { u8"stl" } };

    // "one.stl;;; two.stl;" —— 空段与首尾空白都无害。
    scriptedStdin().feed(narrow(one) + ";;; " + narrow(two) + ";\n");
    const auto ok = io->getFilesAsync(u8"输入> ", options).result();
    ASSERT_TRUE(ok.has_value());
    ASSERT_EQ(ok->size(), 2u);
    EXPECT_EQ((*ok)[0], one);
    EXPECT_EQ((*ok)[1], two);
    EXPECT_NE(captured_.str().find("多个用 ; 分隔"), std::string::npos) << "提示里要说明多个怎么给";

    // 第二段不存在：整条读失败，原因指向那一段。
    scriptedStdin().feed(narrow(one) + ";" + narrow(dir / "missing.stl") + "\n");
    const auto bad = io->getFilesAsync(u8"输入> ", options).result();
    EXPECT_FALSE(bad.has_value()) << "一段坏则整条读失败，不给部分成功";

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 文件与目录是两种提问：目录读拒绝文件，文件读拒绝目录。
TEST_F(ConsoleUserIOReadTest, DirReadRejectsAFileAndAcceptsADirectory)
{
    const auto dir  = makeTempCaseDir("dir");
    const auto file = dir / "mesh.stl";
    touchFile(file);

    scriptedStdin().feed(narrow(dir) + "\n");
    const auto ok = io->getDirAsync(u8"输出目录> ").result();
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, dir);

    scriptedStdin().feed(narrow(file) + "\n");
    const auto not_a_dir = io->getDirAsync(u8"输出目录> ").result();
    EXPECT_FALSE(not_a_dir.has_value());
    EXPECT_NE(captured_.str().find("不是目录"), std::string::npos);

    scriptedStdin().feed(narrow(dir) + "\n");
    const auto not_a_file = io->getFileAsync(u8"网格> ", vn::appfw::UserIO::FileReadOptions{}).result();
    EXPECT_FALSE(not_a_file.has_value());
    EXPECT_NE(captured_.str().find("不是文件"), std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

TEST(ProgressPresenterTest, CancelButtonRequestsStop)
{
    guifw::ProgressPresenter presenter;
    vn::appfw::ProgressHost  host;
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

    const auto name = vn::String(u8"nestedProgressCmd");
    cm->unregisterCommand(name);
    cm->unregisterCommand(u8"nested_child");
    ASSERT_TRUE(cm->registerCommand<NestedChildCommand>(u8"nested_child"));
    ASSERT_TRUE(cm->registerCommand<NestedProgressCommand>(name));

    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());

    // 父命令为 LongRunning → 前台宿主；内部经 executeChild 运行子命令（绕过串联门）。
    auto                     task = cm->executeCommandAsync(name);
    std::atomic<bool>        done{ false };
    vn::appfw::CommandResult result;
    std::thread              runner([&] {
        result = std::move(task).result();
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!vn::appfw::ProgressHost::isActive() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // 父命令运行期间：前台宿主挂载、应用忙。
    EXPECT_TRUE(vn::appfw::ProgressHost::isActive());
    EXPECT_TRUE(app->isBusy());

    // 方案B：子命令压栈顶替父命令 → 前台栈出现深度 2。
    bool saw_depth2 = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (vn::appfw::ProgressHost::foregroundStack().size() >= 2) {
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
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);

    // 链结束后宿主释放。
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
    EXPECT_FALSE(app->isBusy());

    cm->unregisterCommand(name);
}

// ── CommandManager：执行链、取消与历史 ──────────────────────────────────────

// 可取消命令：睡眠期间响应取消请求。用 None 标志 ⇒ 不挂进度宿主、不触发串联门，
// 因此多个实例可以作为互相独立的链并发运行。
class CancellableSleepCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    CancellableSleepCommand(vn::String name, std::chrono::milliseconds duration)
      : name_(std::move(name))
      , duration_(duration)
    {}

    vn::String name() const override
    {
        return name_;
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"cancellable sleep";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        co_await vn::async::sleepFor(duration_, context->stopToken());
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

  private:
    vn::String                name_;
    std::chrono::milliseconds duration_;
};

VN_OBJECT_META_IMPL(CancellableSleepCommand, vn::appfw::Command)

// 抛异常的命令（挂起后抛，走协程的真实异常路径），用于验证异常不会穿出到
// DetachedTask/事件循环（旧行为是 std::terminate）。
class ThrowingCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"throwing";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"throws after a suspension";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_await vn::async::sleepFor(std::chrono::milliseconds(1));
        throw std::runtime_error(s_empty_message.load() ? "" : "boom");

        // Unreachable, and written on purpose: MSVC reports C4716 ("must return a value") for a non-void
        // coroutine body whose closing brace it considers reachable, and it does not count the throw above as
        // ending the flow. The throw is the behaviour under test, so this value is never produced.
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed);
    }

    /// 为 true 时抛出的异常 what() 为空，用于验证结果消息的兜底。
    inline static std::atomic<bool> s_empty_message{ false };
};

VN_OBJECT_META_IMPL(ThrowingCommand, vn::appfw::Command)

// "不适用"的命令：明白请求，但当前上下文里没有它能作用的东西（没选中的模型、没打开的
// 文档……）。用于验证这个结果不会被报成错误（旧行为：任何非 Success 都记 VN_LOGE，并且
// 给用户弹一句"命令执行失败"）。
class NotApplicableCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"notApplicable";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"does not apply to the current context";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        ++s_runs;
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::NotApplicable, s_message);
    }

    /// 结果里带的解释；为空表示"没什么可说的"。
    inline static vn::String s_message{ u8"当前上下文没有可作用的东西" };
    /// 执行次数，用于确认命令真的跑了。
    inline static std::atomic<int> s_runs{ 0 };
};

VN_OBJECT_META_IMPL(NotApplicableCommand, vn::appfw::Command)

// Undoable 命令：记录是否真的执行过，用于验证快照回调的先后与拦截。
class UndoableProbeCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"undoableProbe";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"records that it ran";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::Undoable;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        s_ran = true;
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_ran{ false };
    inline static std::atomic<int>  s_snapshots{ 0 };
};

VN_OBJECT_META_IMPL(UndoableProbeCommand, vn::appfw::Command)

// 父命令：内部直接用 CommandManager 的顶层入口（而不是 context->executeChild）
// 运行子命令，用于验证“顶层入口不可当嵌套用”。
class TopLevelCallingParentCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"topLevelCaller";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"calls a top-level entry from inside";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (!cm) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"no manager"));
        }

        // 父命令持有串联门（LongRunning），顶层入口会被拒。
        const auto child = cm->executeCommandAndWait(s_child_name);
        s_child_status   = child.status();
        s_child_message  = child.message();
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static vn::String               s_child_name{ u8"syncChildInsideParent" };
    inline static vn::appfw::CommandStatus s_child_status{ vn::appfw::CommandStatus::Success };
    inline static vn::String               s_child_message;
};

VN_OBJECT_META_IMPL(TopLevelCallingParentCommand, vn::appfw::Command)

// 组合标志：长时间且改数据的命令——两条特性必须同时生效（快照 + 串联门）。
class CombinedFlagsCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"combinedFlags";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"Undoable | LongRunning";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::Undoable | vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        // 跑得够久，让调用方能在它运行期间去抢串联门。
        co_await vn::async::sleepFor(std::chrono::milliseconds(50));
        s_ran = true;
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static bool s_ran = false;
};

VN_OBJECT_META_IMPL(CombinedFlagsCommand, vn::appfw::Command)

// 探针子命令：记录执行时“前台链里是谁”与深度，用于观察链的归属。
class ForegroundProbeChildCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"foregroundProbe";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"records the foreground chain";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (cm) {
            const auto* current = cm->currentCommand();
            s_foreground_name   = current ? current->name() : vn::String(u8"<none>");
            s_foreground_depth  = cm->runningCount();
        }
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static vn::String s_foreground_name;
    inline static int        s_foreground_depth{ 0 };
};

VN_OBJECT_META_IMPL(ForegroundProbeChildCommand, vn::appfw::Command)

// 父命令（非 LongRunning）：内部 co_await 异步顶层入口去跑“子命令”。
// 异步版是惰性的 ⇒ 链在 await 时才建立，且建立后会成为前台链。
class AsyncCallingParentCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"asyncCaller";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"awaits a top-level entry from inside";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        if (!cm) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed, vn::String(u8"no manager"));
        }

        const auto* before     = cm->currentCommand();
        s_parent_is_foreground = before != nullptr && before->name() == name();

        const auto child = co_await cm->executeCommandAsync(s_child_name);
        s_child_status   = child.status();

        // 顶层入口把前台链换成了子链，子链结束后不会自动恢复成父链。
        const auto* after = cm->currentCommand();
        s_current_after   = after ? after->name() : vn::String(u8"<none>");
        s_depth_after     = cm->runningCount();
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static vn::String               s_child_name{ u8"foregroundProbe" };
    inline static bool                     s_parent_is_foreground{ false };
    inline static vn::appfw::CommandStatus s_child_status{ vn::appfw::CommandStatus::Success };
    inline static vn::String               s_current_after;
    inline static int                      s_depth_after{ 0 };
};

VN_OBJECT_META_IMPL(AsyncCallingParentCommand, vn::appfw::Command)

// 持有串联门的 LongRunning 命令：一直等待直到测试释放，用来确定性地观察
// “门的检查与占用是原子的”。
class GateHoldCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"gateHold";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"holds the serialization gate";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::LongRunning;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        co_await s_release;
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    /// 测试通过 set() 释放；使用前先 reset() 恢复未触发态。
    inline static vn::async::AsyncEvent s_release{};
};

VN_OBJECT_META_IMPL(GateHoldCommand, vn::appfw::Command)

// 自递归命令：每次通过 context->executeChild() 再进一层，用于验证嵌套深度上限。
class RecursiveCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"selfNesting";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nests into itself";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        s_entries.fetch_add(1);
        co_return co_await context->executeChild(u8"selfNesting");
    }

    inline static std::atomic<int> s_entries{ 0 };
};

VN_OBJECT_META_IMPL(RecursiveCommand, vn::appfw::Command)

// 排他命令：接管前台，旧链被取消。记录自身是否真的执行过。
class ExclusiveTakeOverCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"takeover";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"exclusive take over";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::Exclusive;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        s_ran = true;
        if (context && context->isCancelled()) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Cancelled);
        }
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_ran{ false };
};

VN_OBJECT_META_IMPL(ExclusiveTakeOverCommand, vn::appfw::Command)

// 不配合取消的命令：睡眠时不传 token，因此取消请求对它无效，只能自然结束。
// 用来验证排他命令的等待是有界的（超时后继续，而不是无限等待）。
class UncooperativeSleepCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"uncooperative";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"ignores cancellation";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        s_running.store(true);
        co_await vn::async::sleepFor(std::chrono::milliseconds(2600));
        s_running.store(false);
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_running{ false };
};

VN_OBJECT_META_IMPL(UncooperativeSleepCommand, vn::appfw::Command)

// 探针排他命令：执行时记录被接管的链是否仍在运行。
class ExclusiveProbeCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"probeTakeover";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"probe exclusive take over";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::Exclusive;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        s_saw_victim_running.store(UncooperativeSleepCommand::s_running.load());
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_saw_victim_running{ false };
};

VN_OBJECT_META_IMPL(ExclusiveProbeCommand, vn::appfw::Command)

// 父命令：通过 context->executeChild() 嵌套一个可取消子命令，自身不捕获子命令的
// 取消异常（子命令的取消默认向上抛）。用来验证“子命令被取消时也要上报自己结束”。
class NestingCancellableParentCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"nestingParent";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nests a cancellable child";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        co_return co_await context->executeChild(s_child_name);
    }

    inline static vn::String s_child_name{ u8"nestingChild" };
};

VN_OBJECT_META_IMPL(NestingCancellableParentCommand, vn::appfw::Command)

// 参数由父命令给、就地构造的子命令：这种子命令没法预先注册（注册会在运行期搅动全局注册表、
// 触发 commandsChanged、并进入"禁用偏好"持久化），所以走 executeChild(instance)。
class ParameterisedChildCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    explicit ParameterisedChildCommand(int magnitude)
      : magnitude_(magnitude)
    {
        ++s_live;
    }

    ~ParameterisedChildCommand() override
    {
        --s_live;
    }

    vn::String name() const override
    {
        return u8"parameterisedChild";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"child built by its parent";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        s_magnitude     = magnitude_;
        s_live_seen     = s_live;
        s_running_count = context->application()->commandManager()->runningCount();
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static int s_live          = 0;
    inline static int s_live_seen     = 0;
    inline static int s_magnitude     = 0;
    inline static int s_running_count = 0;

  private:
    int magnitude_;
};

VN_OBJECT_META_IMPL(ParameterisedChildCommand, vn::appfw::Command)

// 父命令的三种形态：0 = 嵌套参数化子命令，1 = 传 nullptr，2 = 传"名称已被禁用"的实例。
class InstanceChildParentCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"instanceChildParent";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nests a caller-supplied child";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        if (s_mode == 1) {
            co_return co_await context->executeChild(std::unique_ptr<vn::appfw::Command>{});
        }
        if (s_mode == 2) {
            co_return co_await context->executeChild(std::make_unique<DummyCommand>());
        }
        co_return co_await context->executeChild(std::make_unique<ParameterisedChildCommand>(42));
    }

    inline static int s_mode = 0;
};

VN_OBJECT_META_IMPL(InstanceChildParentCommand, vn::appfw::Command)

// 通过实例自我递归：每层构造一个新的自己作为子命令，用于验证实例形态同样受深度上限约束。
class InstanceRecursiveCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"instanceRecursive";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"nests itself through instances";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        ++s_entries;
        co_return co_await context->executeChild(std::make_unique<InstanceRecursiveCommand>());
    }

    inline static int s_entries = 0;
};

VN_OBJECT_META_IMPL(InstanceRecursiveCommand, vn::appfw::Command)

// executeChild(instance)：子命令进父链（不是新顶层链）、执行期由父帧持有、结束后即销毁。
TEST_F(GuiTest, CommandManager_ExecuteChildRunsACallerSuppliedInstance)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"instanceChildParent");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<InstanceChildParentCommand>(name));

    InstanceChildParentCommand::s_mode         = 0;
    ParameterisedChildCommand::s_live          = 0;
    ParameterisedChildCommand::s_live_seen     = 0;
    ParameterisedChildCommand::s_magnitude     = 0;
    ParameterisedChildCommand::s_running_count = 0;

    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);

    EXPECT_EQ(ParameterisedChildCommand::s_magnitude, 42);
    // 深度为 2 ⇒ 子命令在父链上（顶层入口会开新链，深度则是 1）。
    EXPECT_EQ(ParameterisedChildCommand::s_running_count, 2);
    // 执行期间实例活着，回到父命令时已被父帧销毁。
    EXPECT_EQ(ParameterisedChildCommand::s_live_seen, 1);
    EXPECT_EQ(ParameterisedChildCommand::s_live, 0);

    // 未注册的子命令不出现在列举里（它的名字只用于事件与历史）。
    const auto infos = cm->commandInfos();
    EXPECT_TRUE(std::ranges::none_of(infos, [](const vn::appfw::CommandInfo& info) { return info.name == u8"parameterisedChild"; }));

    cm->unregisterCommand(name);
}

// 拒绝路径：null 实例与"名称已注册且被禁用"的实例，都与顶层按实例入口同规则。
TEST_F(GuiTest, CommandManager_ExecuteChildRefusesNullAndDisabledInstances)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent = vn::String(u8"instanceChildParent");
    const auto dummy  = vn::String(u8"dummy");
    cm->unregisterCommand(parent);
    ASSERT_TRUE(cm->registerCommand<InstanceChildParentCommand>(parent));
    cm->unregisterCommand(dummy);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(dummy));

    InstanceChildParentCommand::s_mode = 1;
    const auto null_child              = cm->executeCommandAndWait(parent);
    EXPECT_EQ(null_child.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(null_child.message(), vn::String(u8"Command is null"));

    // 用户禁用了 "dummy" ⇒ 父命令拿着同名实例也不能绕过禁用（与顶层按实例入口一致）。
    cm->setCommandEnabled(dummy, false);
    InstanceChildParentCommand::s_mode = 2;
    const auto disabled_child          = cm->executeCommandAndWait(parent);
    EXPECT_EQ(disabled_child.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_NE(disabled_child.message().find(u8"已被禁用"), vn::String::npos);

    cm->setCommandEnabled(dummy, true);
    cm->unregisterCommand(dummy);
    cm->unregisterCommand(parent);
}

// 实例形态同样受 maxChainDepth() 约束：自我递归在 64 层被拒，而不是把协程帧耗尽。
TEST_F(GuiTest, CommandManager_ExecuteChildBoundsInstanceNesting)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"instanceRecursive");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<InstanceRecursiveCommand>(name));
    InstanceRecursiveCommand::s_entries = 0;

    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(result.message(), vn::String(u8"Command nesting is too deep"));
    EXPECT_EQ(InstanceRecursiveCommand::s_entries, vn::appfw::CommandManager::maxChainDepth());
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(name);
}

// 历史记录的是执行结果快照，不是命令对象：命令实例在协程返回时即被销毁，
// 旧实现保存原始指针后 historyAt() 返回悬垂指针（ASan 下即 UAF）。
TEST_F(GuiTest, CommandManager_HistoryRecordsValueSnapshots)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"historyDummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    cm->clearHistory();
    ASSERT_EQ(cm->historyCount(), 0);

    const auto result = cm->executeCommandAndWait(name);
    ASSERT_EQ(result.status(), vn::appfw::CommandStatus::Success);

    ASSERT_EQ(cm->historyCount(), 1);
    const auto entry = cm->historyAt(0);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, vn::String(u8"dummy"));
    EXPECT_EQ(entry->command_class, DummyCommand::desc());
    EXPECT_EQ(entry->result.status(), vn::appfw::CommandStatus::Success);

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

// 历史是“跑了什么、结果如何”的记录，刻意不留结果载荷（std::any）：
// 否则最近 maxHistoryEntries() 次运行的载荷（可能是大缓冲区）会被一直吊住。
TEST_F(GuiTest, CommandManager_HistoryDoesNotRetainTheResultPayload)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"historyPayload");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<PayloadCommand>(name));
    cm->clearHistory();

    const auto result = cm->executeCommandAndWait(name);
    ASSERT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    // 调用方拿到的那份带载荷……
    ASSERT_TRUE(result.data().has_value());
    EXPECT_EQ(std::any_cast<int>(result.data()), 7);

    // ……历史里的那份不带。
    ASSERT_EQ(cm->historyCount(), 1);
    const auto entry = cm->historyAt(0);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_FALSE(entry->result.data().has_value());

    cm->unregisterCommand(name);
    cm->clearHistory();
}

// 排他命令接管正在运行的链：旧链被取消并在有界时间内收尾，新命令在全新链上成功。
TEST_F(GuiTest, CommandManager_ExclusiveTakesOverRunningChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto victim_name = vn::String(u8"exclusiveVictim");
    const auto taker_name  = vn::String(u8"takeoverCmd");
    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(taker_name);
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), victim_name, [victim_name] {
        return new CancellableSleepCommand(victim_name, std::chrono::milliseconds(300));
    }));
    ASSERT_TRUE(cm->registerCommand<ExclusiveTakeOverCommand>(taker_name));
    ExclusiveTakeOverCommand::s_ran = false;

    // 后台链：可取消的长睡眠，放到工作线程上驱动，主线程观察。
    auto                     task = cm->executeCommandAsync(victim_name);
    vn::appfw::CommandResult victim_result;
    std::atomic<bool>        victim_done{ false };
    std::thread              runner([&] {
        victim_result = std::move(task).result();
        victim_done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 1);
    ASSERT_NE(cm->currentCommand(), nullptr);
    EXPECT_EQ(cm->currentCommand()->name(), vn::String(u8"exclusiveVictim"));

    // 接管：有界等待旧链收尾后，排他命令在自己的链上执行。
    const auto takeover = cm->executeCommandAndWait(taker_name);
    EXPECT_EQ(takeover.status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(ExclusiveTakeOverCommand::s_ran);

    while (!victim_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.join();

    // 旧链是被取消的（而不是无人取消地继续跑完），且接管后前台没有残留。
    EXPECT_TRUE(victim_done.load());
    EXPECT_EQ(victim_result.status(), vn::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(cm->runningCount(), 0);
    EXPECT_EQ(cm->currentCommand(), nullptr);
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
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

    const auto victim_name = vn::String(u8"uncooperative");
    const auto probe_name  = vn::String(u8"probeTakeover");
    cm->unregisterCommand(victim_name);
    cm->unregisterCommand(probe_name);
    ASSERT_TRUE(cm->registerCommand<UncooperativeSleepCommand>(victim_name));
    ASSERT_TRUE(cm->registerCommand<ExclusiveProbeCommand>(probe_name));
    ExclusiveProbeCommand::s_saw_victim_running = false;

    auto                     task = cm->executeCommandAsync(victim_name);
    vn::appfw::CommandResult victim_result;
    std::thread              runner([&] { victim_result = std::move(task).result(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!UncooperativeSleepCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(UncooperativeSleepCommand::s_running.load());

    // 受害者无视取消 ⇒ 排他命令等到上界后被拒绝，而不是与之并发执行。
    const auto takeover = cm->executeCommandAndWait(probe_name);
    EXPECT_EQ(takeover.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_FALSE(ExclusiveProbeCommand::s_saw_victim_running.load());

    runner.join();
    // 受害者并未被取消（它不配合），最终自然成功；它从未与新链重叠。
    EXPECT_EQ(victim_result.status(), vn::appfw::CommandStatus::Success);

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

    const auto name_a = vn::String(u8"chainA");
    const auto name_b = vn::String(u8"chainB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
    ASSERT_TRUE(
        cm->registerCommand(CancellableSleepCommand::desc(), name_a, [name_a] { return new CancellableSleepCommand(name_a, std::chrono::milliseconds(300)); }));
    ASSERT_TRUE(
        cm->registerCommand(CancellableSleepCommand::desc(), name_b, [name_b] { return new CancellableSleepCommand(name_b, std::chrono::milliseconds(300)); }));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 链 A 先运行。
    auto                     task_a = cm->executeCommandAsync(name_a);
    vn::appfw::CommandResult result_a;
    std::thread              runner_a([&] { result_a = std::move(task_a).result(); });

    while ((cm->runningCount() == 0 || cm->currentCommand() == nullptr || cm->currentCommand()->name() != name_a) &&
           std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), name_a);

    // 链 B 随后启动，成为前台链。
    auto                     task_b = cm->executeCommandAsync(name_b);
    vn::appfw::CommandResult result_b;
    std::thread              runner_b([&] { result_b = std::move(task_b).result(); });

    while ((cm->currentCommand() == nullptr || cm->currentCommand()->name() != name_b) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), name_b);

    // 只取消前台链 B。
    cm->cancelCurrent();

    runner_a.join();
    runner_b.join();

    EXPECT_EQ(result_b.status(), vn::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(result_a.status(), vn::appfw::CommandStatus::Success);
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

    const auto name = vn::String(u8"validatedCmd");
    cm->unregisterCommand(name);

    // 空名与空工厂被拒绝，不会留下不可用的注册项。
    EXPECT_FALSE(cm->registerCommand(DummyCommand::desc(), vn::String{}, [] { return new DummyCommand; }));
    EXPECT_FALSE(cm->registerCommand(DummyCommand::desc(), name, std::function<vn::appfw::Command*()>{}));
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
        const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
        ASSERT_NE(it, infos.end());
        EXPECT_EQ(it->group, vn::String(u8"Test"));
        EXPECT_EQ(it->description, vn::String(u8"dummy command"));
        EXPECT_EQ(it->owner, vn::String{});
    }
    // 旧实现每次 commandInfos() 都会实例化一次 ⇒ 这里会变成 4。
    EXPECT_EQ(instantiations.load(), 1);

    // 单条读取（UI 的"这个条目能不能点/写什么 tooltip"）走同一份缓存，不重新实例化。
    const auto flags = cm->commandFlags(name);
    ASSERT_TRUE(flags.has_value());
    EXPECT_EQ(*flags, vn::appfw::CommandFlags::None);
    EXPECT_EQ(cm->commandDescription(name).value_or(vn::String{}), vn::String(u8"dummy command"));
    EXPECT_EQ(instantiations.load(), 1);

    EXPECT_FALSE(cm->commandFlags(vn::String(u8"notRegisteredCmd")).has_value());
    EXPECT_FALSE(cm->commandDescription(vn::String(u8"notRegisteredCmd")).has_value());

    cm->unregisterCommand(name);
    EXPECT_FALSE(cm->isRegistered(name));
}

// 界面用命令名找状态：别名与执行入口同一条解析规则，读到的是命令自己的 flags/description。
TEST_F(GuiTest, CommandManager_FlagsAndDescriptionResolveThroughAliases)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const vn::String name(u8"flags_probe");
    const vn::String alias(u8"flags_probe_alias");
    cm->unregisterCommand(name);
    cm->unregisterAlias(alias);

    ASSERT_TRUE(cm->registerCommand<NeedsDocumentCommand>(name));
    ASSERT_TRUE(cm->registerAlias(alias, name));

    const auto flags = cm->commandFlags(alias);
    ASSERT_TRUE(flags.has_value()) << "别名解析到它指向的命令";
    EXPECT_TRUE(vn::testFlag(*flags, vn::appfw::CommandFlags::NeedsDocument));
    EXPECT_FALSE(vn::testFlag(*flags, vn::appfw::CommandFlags::NeedsView));
    EXPECT_EQ(cm->commandDescription(alias).value_or(vn::String{}), vn::String(u8"needs a document"));

    cm->unregisterAlias(alias);
    cm->unregisterCommand(name);
}

// 禁用是注册表里的一个标记，不是移除：命令仍在列表里、别名仍指向它，只是不能执行；
// 重新启用后立刻恢复。偏好同时写进配置，插件下次启动重新注册时会带着标记回来。
TEST_F(GuiTest, CommandManager_DisableIsAFlagAndIsPersisted)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"disableProbe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->setCommandEnabled(name, true)) << "先清掉可能残留的偏好";
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return new DummyCommand; }));
    ASSERT_TRUE(cm->registerAlias(vn::String(u8"disableProbeAlias"), name));

    // 禁用：仍是注册项、仍在列表里（enabled=false），但不可执行。
    ASSERT_TRUE(cm->setCommandEnabled(name, false));
    EXPECT_TRUE(cm->isRegistered(name)) << "禁用是标记而不是移除";
    EXPECT_FALSE(cm->isCommandEnabled(name)) << "禁用的命令不能执行";
    EXPECT_FALSE(cm->isRegistered(vn::String(u8"disableProbeAlias"))) << "别名不是注册的命令名";
    EXPECT_FALSE(cm->isCommandEnabled(vn::String(u8"disableProbeAlias"))) << "别名解析到禁用的命令，同样不可执行";

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end()) << "禁用不把命令从列表里移除";
    EXPECT_FALSE(it->enabled);

    const auto blocked = cm->executeCommandAndWait(name);
    EXPECT_EQ(blocked.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(blocked.message(), vn::String(u8"命令“disableProbe”已被禁用；可在「命令管理器」中启用。"))
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
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
    const auto after = cm->disabledCommands();
    EXPECT_EQ(std::find(after.begin(), after.end(), name), after.end());

    cm->unregisterAlias(vn::String(u8"disableProbeAlias"));
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
    const auto name = vn::String(u8"dummy");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->setCommandEnabled(name, true));
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return new DummyCommand; }));

    EXPECT_EQ(cm->executeCommandAndWait(new DummyCommand).status(), vn::appfw::CommandStatus::Success);

    ASSERT_TRUE(cm->setCommandEnabled(name, false));
    const auto blocked = cm->executeCommandAndWait(new DummyCommand);
    EXPECT_EQ(blocked.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(blocked.message(), vn::String(u8"命令“dummy”已被禁用；可在「命令管理器」中启用。"));

    // 未注册的名字不受影响：临时造的命令照旧执行。
    ASSERT_TRUE(cm->setCommandEnabled(name, true));
    cm->unregisterCommand(name);
    EXPECT_EQ(cm->executeCommandAndWait(new DummyCommand).status(), vn::appfw::CommandStatus::Success);
}

// 工厂返回空指针不应让列举崩溃（旧实现在 commandInfos() 里直接解引用）。
TEST_F(GuiTest, CommandManager_NullFactoryIsTolerated)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"brokenFactory");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand(DummyCommand::desc(), name, [] { return static_cast<vn::appfw::Command*>(nullptr); }));
    EXPECT_TRUE(cm->isRegistered(name));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_TRUE(it->group.empty());
    EXPECT_TRUE(it->description.empty());

    // 执行也返回 Failed，而不是解引用空指针。
    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);

    cm->unregisterCommand(name);
}

// 别名挂接：按名字索引挂到目标条目，目标不存在时忽略（registerAlias 允许先建别名）。
TEST_F(GuiTest, CommandManager_CommandInfosAttachesAliases)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"aliasTarget");
    cm->unregisterCommand(name);
    cm->unregisterAlias(vn::String(u8"aliasTargetAlias"));
    cm->unregisterAlias(vn::String(u8"aliasWithoutTarget"));

    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    ASSERT_TRUE(cm->registerAlias(vn::String(u8"aliasTargetAlias"), name));
    ASSERT_TRUE(cm->registerAlias(vn::String(u8"aliasWithoutTarget"), vn::String(u8"notRegisteredAtAll")));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    ASSERT_EQ(it->aliases.size(), 1u);
    EXPECT_EQ(it->aliases.front(), vn::String(u8"aliasTargetAlias"));

    // 结果按名字排序。
    EXPECT_TRUE(std::is_sorted(infos.begin(), infos.end(), [](const vn::appfw::CommandInfo& a, const vn::appfw::CommandInfo& b) { return a.name < b.name; }));

    // 别名可以启动目标命令。
    EXPECT_EQ(cm->executeCommandAndWait(vn::String(u8"aliasTargetAlias")).status(), vn::appfw::CommandStatus::Success);

    cm->unregisterAlias(vn::String(u8"aliasTargetAlias"));
    cm->unregisterAlias(vn::String(u8"aliasWithoutTarget"));
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

    const auto name = vn::String(u8"undoableProbe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<UndoableProbeCommand>(name));

    // 1) 正常路径：快照先于命令体。
    UndoableProbeCommand::s_ran       = false;
    UndoableProbeCommand::s_snapshots = 0;
    cm->setSnapshotHandler([] { UndoableProbeCommand::s_snapshots.fetch_add(1); });
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(UndoableProbeCommand::s_snapshots.load(), 1);
    EXPECT_TRUE(UndoableProbeCommand::s_ran.load());

    // 2) 快照失败：不得执行命令（否则之后的 undo 会回到不存在的状态）。
    cm->setSnapshotHandler([] { throw std::runtime_error("snapshot boom"); });
    UndoableProbeCommand::s_ran = false;
    const auto failed           = cm->executeCommandAndWait(name);
    EXPECT_EQ(failed.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(failed.message(), vn::String(u8"snapshot boom"));
    EXPECT_FALSE(UndoableProbeCommand::s_ran.load());

    // 3) 清空回调：不再调用，命令正常执行。
    cm->setSnapshotHandler({});
    UndoableProbeCommand::s_ran = false;
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(UndoableProbeCommand::s_ran.load());
    EXPECT_EQ(UndoableProbeCommand::s_snapshots.load(), 1);

    cm->setSnapshotHandler({});
    cm->unregisterCommand(name);
}

// 顶层入口用错位置会被串联门拦住：LongRunning 父命令内部调 executeCommandAndWait() 属于新链，
// 得到 Failed（在命令内部跑子命令必须走 context->executeChild()）。
TEST_F(GuiTest, CommandManager_TopLevelCallInsideLongRunningParentIsRefused)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent_name = vn::String(u8"topLevelCaller");
    const auto child_name  = TopLevelCallingParentCommand::s_child_name;
    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(child_name));
    ASSERT_TRUE(cm->registerCommand<TopLevelCallingParentCommand>(parent_name));
    TopLevelCallingParentCommand::s_child_status  = vn::appfw::CommandStatus::Success;
    TopLevelCallingParentCommand::s_child_message = {};

    const auto result = cm->executeCommandAndWait(parent_name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(TopLevelCallingParentCommand::s_child_status, vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(TopLevelCallingParentCommand::s_child_message, vn::String(u8"另一个操作正在进行中，请稍候。"));

    // 门随父命令结束释放。
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
}

// 组合标志：`Undoable | LongRunning` 的命令必须**两条**特性同时生效
// （快照先于执行 + 运行期间占住串联门），而不是只有一个标志被看见。
TEST_F(GuiTest, CommandManager_CombinedFlagsAreHonoured)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name  = vn::String(u8"combinedFlags");
    const auto other = vn::String(u8"combinedFlagsOther");
    cm->unregisterCommand(name);
    cm->unregisterCommand(other);
    ASSERT_TRUE(cm->registerCommand<CombinedFlagsCommand>(name));
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(other));

    int snapshots = 0;
    cm->setSnapshotHandler([&snapshots] { ++snapshots; });

    CombinedFlagsCommand::s_ran = false;
    cm->clearHistory();
    cm->executeDetached(name);

    // LongRunning 部分：命令正在 50ms 的 sleep 里，顶层命令必须被门拒绝。
    const auto refused = cm->executeCommandAndWait(other);
    EXPECT_EQ(refused.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(refused.message(), vn::String(u8"另一个操作正在进行中，请稍候。"));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        app->mainThreadDispatcher()->deliverPostedCalls();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_GE(cm->historyCount(), 1);
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(CombinedFlagsCommand::s_ran);
    EXPECT_EQ(snapshots, 1) << "Undoable 部分：快照在执行前发生一次";

    cm->setSnapshotHandler({});
    cm->unregisterCommand(name);
    cm->unregisterCommand(other);
    cm->clearHistory();
}

// 命令内部 co_await 异步顶层入口 = 真正新开一条链（Task 惰性 ⇒ 调用时不建链，await 时才建）：
// 子命令在新链上运行并自己成为前台链，父链被顶下去且结束后不会自动恢复。
TEST_F(GuiTest, CommandManager_AsyncTopLevelCallInsideCommandOpensNewChain)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto parent_name = vn::String(u8"asyncCaller");
    const auto child_name  = AsyncCallingParentCommand::s_child_name;
    cm->unregisterCommand(parent_name);
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<ForegroundProbeChildCommand>(child_name));
    ASSERT_TRUE(cm->registerCommand<AsyncCallingParentCommand>(parent_name));
    ForegroundProbeChildCommand::s_foreground_name.clear();
    ForegroundProbeChildCommand::s_foreground_depth   = 0;
    AsyncCallingParentCommand::s_parent_is_foreground = false;
    AsyncCallingParentCommand::s_child_status         = vn::appfw::CommandStatus::Failed;
    AsyncCallingParentCommand::s_current_after.clear();
    AsyncCallingParentCommand::s_depth_after = -1;

    const auto result = cm->executeCommandAndWait(parent_name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);

    // 父命令确实在前台链上（否则后面的对照没意义）。
    EXPECT_TRUE(AsyncCallingParentCommand::s_parent_is_foreground);
    // 子命令新链上跑成功，并且在它自己运行时它就是前台。
    EXPECT_EQ(AsyncCallingParentCommand::s_child_status, vn::appfw::CommandStatus::Success);
    EXPECT_EQ(ForegroundProbeChildCommand::s_foreground_name, vn::String(u8"foregroundProbe"));
    EXPECT_EQ(ForegroundProbeChildCommand::s_foreground_depth, 1);
    // 前台链不被恢复：子链已结束 ⇒ 父命令还在跑但前台查不到任何人。
    EXPECT_EQ(AsyncCallingParentCommand::s_current_after, vn::String(u8"<none>"));
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

    const auto name   = vn::String(u8"chainTarget");
    const auto first  = vn::String(u8"chainAlias1");
    const auto second = vn::String(u8"chainAlias2");
    const auto loop_a = vn::String(u8"loopA");
    const auto loop_b = vn::String(u8"loopB");
    cm->unregisterCommand(name);
    cm->unregisterAlias(first);
    cm->unregisterAlias(second);
    cm->unregisterAlias(loop_a);
    cm->unregisterAlias(loop_b);

    // 先建别名、后注册命令：链式解析仍须生效（旧实现只解析一层 ⇒ 失败）。
    ASSERT_TRUE(cm->registerAlias(first, second));
    ASSERT_TRUE(cm->registerAlias(second, name));
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));

    EXPECT_TRUE(cm->isCommandEnabled(first)) << "别名链解析到已注册且启用的命令";
    EXPECT_EQ(cm->executeCommandAndWait(first).status(), vn::appfw::CommandStatus::Success);

    // 两个别名都挂在最终解析到的命令下。
    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_EQ(it->aliases.size(), 2u);

    // 环：不挂死，只是解析不到命令。
    ASSERT_TRUE(cm->registerAlias(loop_a, loop_b));
    ASSERT_TRUE(cm->registerAlias(loop_b, loop_a));
    EXPECT_FALSE(cm->isCommandEnabled(loop_a)) << "环解析不到命令";
    EXPECT_EQ(cm->executeCommandAndWait(loop_a).status(), vn::appfw::CommandStatus::Failed);

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

    const auto name = vn::String(u8"selfNesting");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<RecursiveCommand>(name));
    RecursiveCommand::s_entries = 0;

    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(result.message(), vn::String(u8"Command nesting is too deep"));
    EXPECT_EQ(RecursiveCommand::s_entries.load(), vn::appfw::CommandManager::maxChainDepth());
    EXPECT_EQ(cm->runningCount(), 0);
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());

    cm->unregisterCommand(name);
}

// 历史有容量上限：超出后丢弃最旧的一条，长时间运行不会无限增长。
TEST_F(GuiTest, CommandManager_HistoryIsBounded)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"historyBound");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    cm->clearHistory();

    const auto cap   = static_cast<int>(vn::appfw::CommandManager::maxHistoryEntries());
    const auto total = cap + 3;
    for (int i = 0; i < total; ++i) {
        ASSERT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
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

    const auto name = vn::String(u8"throwingCmd");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<ThrowingCommand>(name));
    cm->clearHistory();

    // 同步路径：不向调用方抛异常，而是返回 Failed 并带上原因。
    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(result.message(), vn::String(u8"boom"));

    // detached 路径：旧实现会在 DetachedTask 里 terminate 掉整个进程。
    cm->executeDetached(name);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->historyCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 2);
    const auto entry = cm->historyAt(1);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, vn::String(u8"throwing"));
    EXPECT_EQ(entry->result.status(), vn::appfw::CommandStatus::Failed);

    // 异常本身没有描述时，结果消息要有兜底文本（否则 UI 只能显示一条无信息的失败）。
    ThrowingCommand::s_empty_message = true;
    const auto silent                = cm->executeCommandAndWait(name);
    EXPECT_EQ(silent.status(), vn::appfw::CommandStatus::Failed);
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

    const auto name = vn::String(u8"throwingFactory");
    cm->unregisterCommand(name);

    const auto factory = []() -> vn::appfw::Command* { throw std::runtime_error("factory boom"); };
    EXPECT_NO_THROW(EXPECT_TRUE(cm->registerCommand(DummyCommand::desc(), name, factory)));
    EXPECT_TRUE(cm->isRegistered(name));

    const auto infos = cm->commandInfos();
    const auto it    = std::find_if(infos.begin(), infos.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    ASSERT_NE(it, infos.end());
    EXPECT_TRUE(it->group.empty());
    EXPECT_TRUE(it->description.empty());

    EXPECT_NO_THROW({
        const auto result = cm->executeCommandAndWait(name);
        EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Failed);
        EXPECT_EQ(result.message(), vn::String(u8"factory boom"));
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

    const auto name_a = vn::String(u8"backgroundA");
    const auto name_b = vn::String(u8"foregroundB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);
    // A 睡很久，保证它在 B 结束后仍然活着；B 短，取消后很快落定。
    ASSERT_TRUE(cm->registerCommand(CancellableSleepCommand::desc(), name_a, [name_a] {
        return new CancellableSleepCommand(name_a, std::chrono::milliseconds(3000));
    }));
    ASSERT_TRUE(
        cm->registerCommand(CancellableSleepCommand::desc(), name_b, [name_b] { return new CancellableSleepCommand(name_b, std::chrono::milliseconds(200)); }));
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
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Cancelled);

    // cancelAll() 触到后台链 A。
    cm->cancelAll();
    while (cm->historyCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 2);
    ASSERT_TRUE(cm->historyAt(1).has_value());
    EXPECT_EQ(cm->historyAt(1)->name, name_a);
    EXPECT_EQ(cm->historyAt(1)->result.status(), vn::appfw::CommandStatus::Cancelled);
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

    const auto parent = vn::String(u8"nestingParent");
    const auto child  = vn::String(u8"nestingChild");
    cm->unregisterCommand(parent);
    cm->unregisterCommand(child);
    ASSERT_TRUE(cm->registerCommand<NestingCancellableParentCommand>(parent));
    ASSERT_TRUE(
        cm->registerCommand(CancellableSleepCommand::desc(), child, [child] { return new CancellableSleepCommand(child, std::chrono::milliseconds(5000)); }));
    cm->clearHistory();

    std::atomic<int> child_executing{ 0 };
    std::atomic<int> child_executed{ 0 };

    auto executing = cm->executing.connect([&](vn::appfw::CommandManager&, vn::appfw::CommandExecutingEventArgs& args) {
        const auto* c = args.command();
        if (c != nullptr && c->name() == child) {
            ++child_executing;
        }
    });
    auto executed  = cm->executed.connect([&](vn::appfw::CommandManager&, vn::appfw::CommandExecutedEventArgs& args) {
        const auto* c = args.command();
        if (c != nullptr && c->name() == child) {
            ++child_executed;
        }
    });

    auto                     task = cm->executeCommandAsync(parent);
    vn::appfw::CommandResult result;
    std::thread              runner([&] { result = std::move(task).result(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->runningCount() < 2 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 2) << "父命令与嵌套子命令应在同一条链上";

    cm->cancelCurrent();
    runner.join();

    executing.disconnect();
    executed.disconnect();

    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Cancelled);
    EXPECT_EQ(child_executing.load(), 1);
    EXPECT_EQ(child_executed.load(), 1) << "被取消的嵌套子命令必须发出 executed 事件";

    ASSERT_EQ(cm->historyCount(), 2) << "父子命令都应进历史";
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->name, child);
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Cancelled);
    ASSERT_TRUE(cm->historyAt(1).has_value());
    EXPECT_EQ(cm->historyAt(1)->name, parent);
    EXPECT_EQ(cm->historyAt(1)->result.status(), vn::appfw::CommandStatus::Cancelled);

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

    const auto victim = vn::String(u8"backgroundVictim");
    const auto quick  = vn::String(u8"quickForeground");
    const auto taker  = vn::String(u8"takeover");
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
    auto                     task = cm->executeCommandAsync(victim);
    vn::appfw::CommandResult victim_result;
    std::atomic<bool>        victim_done{ false };
    std::thread              runner([&] {
        victim_result = std::move(task).result();
        victim_done.store(true);
    });

    while ((cm->currentCommand() == nullptr || cm->currentCommand()->name() != victim) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_NE(cm->currentCommand(), nullptr);
    ASSERT_EQ(cm->currentCommand()->name(), victim);

    // 一条普通顶层命令把受害者顶出前台（她不是 LongRunning，门是开的）。
    ASSERT_EQ(cm->executeCommandAndWait(quick).status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(cm->currentCommand(), nullptr) << "前台已换成刚结束的 quick 链";

    // 排他命令：接管时必须连后台链一起收掉，并在放行前等到她真的结束。
    const auto takeover = cm->executeCommandAndWait(taker);
    EXPECT_EQ(takeover.status(), vn::appfw::CommandStatus::Success);
    EXPECT_TRUE(ExclusiveTakeOverCommand::s_ran);
    EXPECT_TRUE(victim_done.load()) << "排他命令运行时后台链必须已经收尾，不能并发";

    while (!victim_done.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    runner.join();
    EXPECT_EQ(victim_result.status(), vn::appfw::CommandStatus::Cancelled);

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

    const auto name  = vn::String(u8"aliasDisableTarget");
    const auto alias = vn::String(u8"aliasDisableBridge");
    cm->unregisterCommand(name);
    cm->unregisterAlias(alias);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(name));
    ASSERT_TRUE(cm->registerAlias(alias, name));

    EXPECT_TRUE(cm->setCommandEnabled(alias, false));
    EXPECT_FALSE(cm->isCommandEnabled(alias));
    EXPECT_FALSE(cm->isCommandEnabled(name));
    EXPECT_EQ(cm->executeCommandAndWait(alias).status(), vn::appfw::CommandStatus::Failed);
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Failed);

    // 启用同样按别名落到命令上。
    EXPECT_TRUE(cm->setCommandEnabled(alias, true));
    EXPECT_TRUE(cm->isCommandEnabled(name));
    EXPECT_EQ(cm->executeCommandAndWait(alias).status(), vn::appfw::CommandStatus::Success);

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

    const auto name = vn::String(u8"drainTarget");
    cm->unregisterCommand(name);
    ASSERT_TRUE(
        cm->registerCommand(CancellableSleepCommand::desc(), name, [name] { return new CancellableSleepCommand(name, std::chrono::milliseconds(3000)); }));
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
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Cancelled);

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

    const auto name = vn::String(u8"detachedThrow");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<ThrowingCommand>(name));
    cm->clearHistory();

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
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
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Failed);

    // 命令已经记录完失败，但消息还没写进面板：它被投递到应用线程，等事件循环取。
    EXPECT_FALSE(output->toPlainText().contains(u8"boom")) << "失败消息不得从工作线程直接写进控制台面板";
    ASSERT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
    EXPECT_TRUE(output->toPlainText().contains(u8"boom")) << "消息应已投递到应用线程";

    bindConsole(app, nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// "不适用"不是失败：命令说清了当前上下文缺什么，就把那句话照实转达（黄色提示），而不是
// 编一句"命令执行失败"；它自己不说，就什么都不说。历史里记的也必须是真实结果。
TEST_F(GuiTest, CommandManager_DetachedNotApplicableIsExplainedAndNotReportedAsAFailure)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"notApplicable");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<NotApplicableCommand>(name));

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto run_once = [&] {
        cm->clearHistory();
        NotApplicableCommand::s_runs = 0;
        cm->executeDetached(name);
        while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
        EXPECT_EQ(NotApplicableCommand::s_runs.load(), 1) << "命令真的跑了（这不是被拒绝）";
        EXPECT_EQ(cm->historyCount(), 1);
    };

    // ① 带着解释：照实转达，而且绝不出现"命令执行失败"。
    NotApplicableCommand::s_message = vn::String(u8"当前文档不是模型文档");
    attachLogCapture();
    const auto log_mark = logLines().mark();
    run_once();
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::NotApplicable) << "历史里记的是真实结果，不是 Failed";
    EXPECT_TRUE(output->toPlainText().contains(u8"当前文档不是模型文档"));
    EXPECT_FALSE(output->toPlainText().contains(u8"命令执行失败")) << "不适用不是错误";

    // 日志那一份也不能是错误级：这个状态码存在的全部意义就是"别记成故障"。
    bool logged_as_info = false;
    for (const auto& [level, text] : logLines().since(log_mark)) {
        if (text.find("notApplicable") == std::string::npos) {
            continue;
        }
        EXPECT_NE(level, vn::logging::LogLevel::Error) << text;
        if (level == vn::logging::LogLevel::Info && text.find("not applicable") != std::string::npos) {
            logged_as_info = true;
        }
    }
    EXPECT_TRUE(logged_as_info) << "要有这一行 info 级的\"not applicable\"记录";

    // ② 没有解释：一句话都不说 —— 唯一比沉默更糟的选择是编一句不存在的故障。
    panel.clear();
    NotApplicableCommand::s_message = vn::String();
    const auto silent_mark          = logLines().mark();
    run_once();
    EXPECT_TRUE(output->toPlainText().isEmpty()) << "没有解释就不该有任何输出";
    for (const auto& [level, text] : logLines().since(silent_mark)) {
        EXPECT_NE(level, vn::logging::LogLevel::Error) << text;
    }

    NotApplicableCommand::s_message = vn::String(u8"当前上下文没有可作用的东西");
    bindConsole(app, nullptr);
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

    const auto name = vn::String(u8"gateHold");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<GateHoldCommand>(name));
    ASSERT_FALSE(vn::appfw::ProgressHost::isActive());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 阶段 1：持有者挂着不放 ⇒ 第二个顶层命令被门拒绝。
    GateHoldCommand::s_release.reset();
    vn::appfw::CommandResult holder_result;
    std::thread              holder([&] { holder_result = cm->executeCommandAndWait(name); });

    while (cm->runningCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->runningCount(), 1);
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Failed);

    GateHoldCommand::s_release.set();
    holder.join();
    EXPECT_EQ(holder_result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());

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
        const auto result = cm->executeCommandAndWait(name);
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

    // 赢家被接受后挂在 s_release 上（executeCommandAndWait 会阻塞），输家被门拒绝后立即返回。
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

    EXPECT_FALSE(vn::appfw::ProgressHost::isActive());
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

    const auto name_a = vn::String(u8"concurrentA");
    const auto name_b = vn::String(u8"concurrentB");
    cm->unregisterCommand(name_a);
    cm->unregisterCommand(name_b);

    std::atomic<bool> go{ false };
    std::atomic<int>  failures{ 0 };
    const auto        worker = [&](const vn::String& name) {
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
            (void)cm->executeCommandAndWait(name);
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

// 命令内部同步调用 executeCommandAndWait() 不会死锁（`result()` 在调用线程上驱动任务），
// 但会开一条新链——嵌套应当走 context->executeChild()。
TEST_F(GuiTest, CommandManager_NestedSyncExecuteCommandDoesNotDeadlock)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto child_name = vn::String(u8"syncChild");
    cm->unregisterCommand(child_name);
    ASSERT_TRUE(cm->registerCommand<DummyCommand>(child_name));

    const auto parent = [cm, child_name]() -> vn::async::Task<vn::appfw::CommandResult> { co_return cm->executeCommandAndWait(child_name); };

    const auto result = parent().result();
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(cm->runningCount(), 0);

    cm->unregisterCommand(child_name);
}

// ============================ 插件生命周期 ============================
//
// PluginManager 从默认插件目录（<build>/plugins/vine）发现插件。测试环境里
// 一定存在 app_shell 与依赖它的 test_plugin（见 test_plugin/TestPlugin.cpp 的
// VN_DECLARE_PLUGIN 依赖声明）。这些用例依赖该目录，缺失时直接跳过。
//
// 用例之间共享进程内的插件状态，必须按声明顺序执行：先验证"依赖被禁用则
// 依赖者不加载"，再验证"被禁用的插件仍可见但不加载"，最后验证卸载。

namespace
{

/// 在插件状态列表里按名字查找，未找到返回 nullptr。
const vn::appfw::PluginEntry* findPluginEntry(const std::vector<vn::appfw::PluginEntry>& entries, const vn::String& name)
{
    for (const auto& entry : entries) {
        if (entry.info.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

/// 把 vn::String 转成 std::string，用于在 JSON 文本里查找子串。
std::string toUtf8(const vn::String& text)
{
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// 在注册列表里按 id 查找，未找到返回 nullptr。
const vn::appfw::PluginRegistration* findRegistration(const std::vector<vn::appfw::PluginRegistration>& registrations, const vn::String& id)
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
    const auto      dir = vn::appfw::PluginManager::builtInPluginDirectory();
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
      : saved_(vn::appfw::PluginManager::builtInPluginDirectory())
    {
        vn::appfw::PluginManager::setBuiltInPluginDirectory(dir);
    }

    ~BuiltInPluginDirectoryScope()
    {
        vn::appfw::PluginManager::setBuiltInPluginDirectory(saved_);
    }

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
    UserPluginSandbox(vn::appfw::PluginManager* manager, const std::vector<const char*>& stems)
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
            std::filesystem::copy_file(library, directory() / library.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        }

        if (manager_ != nullptr) {
            registration_ = manager_->installPlugin(vn::String(directory().u8string()), vn::appfw::PluginScope::User);
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
                static_cast<void>(manager_->uninstallPlugin(registration_, vn::appfw::PluginScope::User));
            }
        }
        std::filesystem::remove_all(directory(), ec);
        std::filesystem::remove_all(emptyDirectory(), ec);
    }

    UserPluginSandbox(const UserPluginSandbox&)            = delete;
    UserPluginSandbox& operator=(const UserPluginSandbox&) = delete;

    const std::filesystem::path& directory() const
    {
        return directory_;
    }

    const vn::String& registration() const
    {
        return registration_;
    }

  private:
    static std::filesystem::path emptyDirectory()
    {
        return std::filesystem::temp_directory_path() / "vn_sandbox_empty_plugin_dir";
    }

    vn::appfw::PluginManager*                    manager_;
    std::unique_ptr<BuiltInPluginDirectoryScope> directory_scope_;
    std::filesystem::path                        directory_{ std::filesystem::temp_directory_path() / "vn_test_plugins" };
    vn::String                                   registration_;
};

/// 进程内的宿主跳过列表作用域：构造时替换，析构时恢复，避免用例之间互相影响。
/// 注意 skipList() 返回的是内部列表的视图，必须先拷贝再 setSkipList()。
class SkipListScope {
  public:
    explicit SkipListScope(std::vector<vn::String> names)
      : saved_(vn::appfw::PluginManager::skipList())
    {
        vn::appfw::PluginManager::setSkipList(names);
    }

    ~SkipListScope()
    {
        vn::appfw::PluginManager::setSkipList(saved_);
    }

    SkipListScope(const SkipListScope&)            = delete;
    SkipListScope& operator=(const SkipListScope&) = delete;

  private:
    std::vector<vn::String> saved_;
};

constexpr char8_t s_shell_plugin[]     = u8"app_shell";   // 被依赖的插件
constexpr char8_t s_dependent_plugin[] = u8"test_plugin"; // 依赖 app_shell 的插件

/// 插件测试需要真实插件库（Debug 构建的文件名带 d 后缀，如 app_shelld.so）；
/// 没有构建插件时跳过，而不是失败或空跑。
bool builtInPluginDirectoryReady()
{
    std::error_code ec;
    const auto      dir = vn::appfw::PluginManager::builtInPluginDirectory();
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
    EXPECT_EQ(shell->scope, vn::appfw::PluginScope::User);
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
    EXPECT_FALSE(dependent->info.uuid.isNull()) << "插件身份由 VN_DECLARE_PLUGIN 硬编码";
    EXPECT_FALSE(dependent->path.empty());
    EXPECT_FALSE(pm->libraryPath(u8"test_plugin").empty());

    // 未加载 = 生命周期没跑 = 没有注册任何命令。
    EXPECT_FALSE(cm->isRegistered(u8"test_hello"));

    // 也不能被算到别的插件头上：命令队列属于模块，命令的 owner 必须是注册它的
    // 那个插件。修复前这里会把 test_plugin 的命令全部算给 app_shell。
    const auto shell_commands = pm->commandInfosForPlugin(u8"app_shell");
    EXPECT_TRUE(std::none_of(shell_commands.begin(), shell_commands.end(), [](const vn::appfw::CommandInfo& info) {
        return info.name.rfind(u8"test_", 0) == 0;
    })) << "app_shell 名下不得出现 test_plugin 的命令";

    // 外壳自带的文档命令登记在本插件名下（VN_DECLARE_COMMAND 的模块队列在插件注册时冲刷）。
    const auto shell_has_command = [&shell_commands](const vn::String& name) {
        return std::any_of(shell_commands.begin(), shell_commands.end(), [&name](const vn::appfw::CommandInfo& info) { return info.name == name; });
    };
    EXPECT_TRUE(shell_has_command(u8"switch_document"));
    EXPECT_TRUE(shell_has_command(u8"close_document"));

    EXPECT_TRUE(pm->commandInfosForPlugin(u8"test_plugin").empty()) << "未加载的插件名下不得有任何命令";

    // 启用后重新加载等价于 "重启后" 的状态：命令才出现在注册表里。
    EXPECT_TRUE(pm->setPluginEnabled(u8"test_plugin", true));
    ASSERT_TRUE(pm->loadAll());
    EXPECT_TRUE(pm->isLoaded(u8"test_plugin"));
    EXPECT_TRUE(cm->isRegistered(u8"test_hello"));

    const auto own_commands = pm->commandInfosForPlugin(u8"test_plugin");
    EXPECT_TRUE(std::any_of(own_commands.begin(), own_commands.end(), [](const vn::appfw::CommandInfo& info) { return info.name == u8"test_hello"; }))
        << "加载后命令应归属于声明它的插件";
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
    vn::appfw::PluginManager::removeFromSkipList(u8"test_plugin");
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

    const vn::String name(u8"no_such_plugin");
    const auto&      key = vn::appfw::PluginManager::disabledConfigKey();
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
    const auto root = std::filesystem::path(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdU16String());
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

    const auto path = std::filesystem::temp_directory_path() / "vn_test_config_never_created.json";
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
    EXPECT_EQ(shell->scope, vn::appfw::PluginScope::User);
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
    const auto order   = vn::appfw::PluginManager::unloadOrder(entries);

    std::size_t loaded = 0;
    for (const auto& entry : entries) {
        if (entry.loaded) {
            ++loaded;
        }
    }
    ASSERT_EQ(order.size(), loaded);

    const auto index_of = [&order](const vn::String& name) {
        const auto it = std::find(order.begin(), order.end(), name);
        return it == order.end() ? order.size() : static_cast<std::size_t>(it - order.begin());
    };
    for (const auto& entry : entries) {
        if (!entry.loaded) {
            continue;
        }
        for (const auto& dep : entry.info.dependencies) {
            if (std::find(order.begin(), order.end(), dep) != order.end()) {
                EXPECT_LT(index_of(entry.info.name), index_of(dep)) << "依赖方必须先卸载：" << toUtf8(entry.info.name);
            }
        }
    }

    // 合成集合：结论只由依赖声明决定，与传入顺序无关。
    const auto make_entry = [](const char8_t* name, std::vector<vn::String> deps, bool loaded_flag = true) {
        vn::appfw::PluginEntry entry;
        entry.info.name         = name;
        entry.info.dependencies = std::move(deps);
        entry.enabled           = true;
        entry.loaded            = loaded_flag;
        return entry;
    };

    // child 依赖 parent，但 child 排在前面（显式 load() 的典型结果）。
    const std::vector<vn::appfw::PluginEntry> dependent_first = {
        make_entry(u8"child", { u8"parent" }),
        make_entry(u8"parent", {}),
    };
    EXPECT_EQ(vn::appfw::PluginManager::unloadOrder(dependent_first), (std::vector<vn::String>{ u8"child", u8"parent" }));

    // 正常加载顺序也必须得到同样的结果（"列表倒序"在这里会给出错误答案）。
    const std::vector<vn::appfw::PluginEntry> dependency_first = {
        make_entry(u8"parent", {}),
        make_entry(u8"child", { u8"parent" }),
    };
    EXPECT_EQ(vn::appfw::PluginManager::unloadOrder(dependency_first), (std::vector<vn::String>{ u8"child", u8"parent" }));

    // 三层链 + 未加载项参与排序（未加载不算数）+ 已不在集合里的依赖不阻塞。
    const std::vector<vn::appfw::PluginEntry> chain = {
        make_entry(u8"grandchild", { u8"child" }),
        make_entry(u8"child", { u8"parent", u8"missing" }),
        make_entry(u8"parent", {}),
        make_entry(u8"not_loaded", { u8"grandchild" }, false),
    };
    EXPECT_EQ(vn::appfw::PluginManager::unloadOrder(chain), (std::vector<vn::String>{ u8"grandchild", u8"child", u8"parent" }));

    // 声明成环（只有显式 load() 能造出来）：不能死循环，回退为传入顺序。
    const std::vector<vn::appfw::PluginEntry> cycle = {
        make_entry(u8"a", { u8"b" }),
        make_entry(u8"b", { u8"a" }),
    };
    const auto cycle_order = vn::appfw::PluginManager::unloadOrder(cycle);
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

    vn::appfw::PluginLoadContext ctx(app, u8"my_plugin");
    const auto                   dir = ctx.ensureDataDirectory();
    EXPECT_EQ(dir, root / "my_plugin");
    EXPECT_TRUE(std::filesystem::is_directory(dir)) << "首次调用即创建";

    // 另一个插件拿到另一个目录，互不干扰。
    vn::appfw::PluginLoadContext other(app, u8"other_plugin");
    EXPECT_EQ(other.ensureDataDirectory(), root / "other_plugin");

    // 没有 Application / 没有插件名的上下文返回空路径，不崩。
    vn::appfw::PluginLoadContext headless(nullptr, u8"my_plugin");
    EXPECT_TRUE(headless.ensureDataDirectory().empty());
    vn::appfw::PluginLoadContext unnamed(app, {});
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
    const auto      user_dir = std::filesystem::temp_directory_path() / "vn_user_plugins_test";
    std::filesystem::remove_all(user_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(user_dir, ec));

    // 注册目录属于应用的数据目录，路径与配置/日志同级。
    const auto registry = app->pluginRegistrationDirectory();
    EXPECT_EQ(registry.parent_path(), app->dataDirectory());
    EXPECT_EQ(registry.filename().string(), "installed.d");

    const auto id = pm->installPlugin(vn::String(user_dir.u8string()), vn::appfw::PluginScope::User);
    ASSERT_FALSE(id.empty()) << "注册成功应返回 id";
    EXPECT_TRUE(std::filesystem::is_directory(registry)) << "注册目录按需创建";

    auto registrations = pm->pluginRegistrations();
    ASSERT_EQ(registrations.size(), 1u);
    EXPECT_TRUE(registrations[0].id == id);
    EXPECT_TRUE(registrations[0].path == vn::String(user_dir.u8string()));
    EXPECT_EQ(registrations[0].scope, vn::appfw::PluginScope::User);
    EXPECT_TRUE(registrations[0].enabled);
    const std::string file_text(reinterpret_cast<const char*>(registrations[0].file.data()), registrations[0].file.size());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(std::u8string(file_text.begin(), file_text.end()))));
    EXPECT_NE(std::string(reinterpret_cast<const char*>(registrations[0].file.data()), registrations[0].file.size()).find("installed.d"), std::string::npos);

    // 重复安装同一位置：写同一个文件，仍然只有一条注册。
    EXPECT_TRUE(pm->installPlugin(vn::String(user_dir.u8string()), vn::appfw::PluginScope::User) == id);
    EXPECT_EQ(pm->pluginRegistrations().size(), 1u);

    // 相对路径必须落盘成绝对路径：注册文件是下次启动读的，那时的工作目录未必定是同一个。
    const auto relative_dir = std::filesystem::temp_directory_path() / "vn_relative_plugins_test";
    std::filesystem::remove_all(relative_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(relative_dir, ec));
    const auto previous_directory = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path(), ec);
    const auto relative_id = pm->installPlugin(u8"vn_relative_plugins_test", vn::appfw::PluginScope::User);
    std::filesystem::current_path(previous_directory, ec);
    ASSERT_FALSE(relative_id.empty()) << "相对路径也应能注册";
    const auto  relative_entries = pm->pluginRegistrations();
    const auto* relative_entry   = findRegistration(relative_entries, relative_id);
    ASSERT_NE(relative_entry, nullptr);
    const std::filesystem::path stored(std::u8string_view(relative_entry->path.data(), relative_entry->path.size()));
    EXPECT_TRUE(stored.is_absolute()) << "注册文件里必须是绝对路径";
    EXPECT_EQ(stored, std::filesystem::weakly_canonical(relative_dir, ec));
    EXPECT_TRUE(pm->uninstallPlugin(relative_id, vn::appfw::PluginScope::User));
    std::filesystem::remove_all(relative_dir, ec);

    // 拒绝的输入：空路径、不存在的路径、BuiltIn 作用域。
    EXPECT_TRUE(pm->installPlugin({}, vn::appfw::PluginScope::User).empty());
    EXPECT_TRUE(pm->installPlugin(u8"/no/such/plugin/location.so", vn::appfw::PluginScope::User).empty());
    EXPECT_TRUE(pm->installPlugin(vn::String(user_dir.u8string()), vn::appfw::PluginScope::BuiltIn).empty());
    EXPECT_EQ(pm->pluginRegistrations().size(), 1u);

    EXPECT_TRUE(pm->uninstallPlugin(id, vn::appfw::PluginScope::User));
    EXPECT_TRUE(pm->pluginRegistrations().empty());
    EXPECT_FALSE(pm->uninstallPlugin(id, vn::appfw::PluginScope::User)) << "已删除的注册再删应返回 false";

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
    // 注册必须点名那个库 FILE（沙箱里那份拷贝），而不是只把插件名抄回来；期望的文件名从拷贝自身
    // 推出，两棵树都成立——这里原来写死 Debug 后缀 "test_plugind"，于是本条在 Release 树一直是红的
    // （Debug 的库叫 test_plugind.so，Release 叫 test_plugin.so）。
    const std::string registered_path(reinterpret_cast<const char*>(manual->path.data()), manual->path.size());
    EXPECT_NE(registered_path.find(source.filename().string()), std::string::npos);

    // 策略必须在没有跑过 loadAll() 的进程里也生效：一次显式 load()（对话框的试用加载）
    // 不得把管理员对所有用户禁用的插件重新拉起来。
    EXPECT_EQ(pm->load(vn::String(copy.u8string())), nullptr) << "策略禁用的插件不得被显式加载";
    EXPECT_FALSE(pm->isLoaded(u8"test_plugin"));

    ASSERT_TRUE(pm->loadAll());

    const auto  entries = pm->pluginEntries();
    const auto* entry   = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(entry, nullptr) << "被策略禁用的插件仍要能被发现（列表里可见）";
    EXPECT_EQ(entry->scope, vn::appfw::PluginScope::User);
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
    const auto      second_dir = std::filesystem::temp_directory_path() / "vn_test_plugins_second";
    std::filesystem::remove_all(second_dir, ec);
    ASSERT_TRUE(std::filesystem::create_directories(second_dir, ec));
    ASSERT_TRUE(std::filesystem::copy_file(source, second_dir / source.filename(), std::filesystem::copy_options::overwrite_existing, ec));
    const auto second_id = pm->installPlugin(vn::String(second_dir.u8string()), vn::appfw::PluginScope::User);
    ASSERT_FALSE(second_id.empty());

    // 只观察发现与去重，不加载：一个插件库在一个进程里只能创建一次实例（vine
    // 类型注册是进程级且不可撤销），前面的用例已经把这份拷贝创建过了，所以这里的
    // loadAll() 成功与否不是本用例的断言对象。
    static_cast<void>(pm->loadAll());

    const auto entries = pm->pluginEntries();
    const auto count   = std::count_if(entries.begin(), entries.end(), [](const vn::appfw::PluginEntry& e) { return e.info.name == u8"test_plugin"; });
    EXPECT_EQ(count, 1) << "同一插件被多处提供时只能出现一次";

    const auto* entry = findPluginEntry(entries, u8"test_plugin");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->scope, vn::appfw::PluginScope::User);
    // 注册按文件名的顺序读取，所以先发现哪一个位置由注册文件名决定；关键是只有一份
    // 被发现，另一份不会产生第二条记录。
    EXPECT_TRUE(entry->path == first_copy || entry->path == second_dir / source.filename());

    EXPECT_TRUE(pm->uninstallPlugin(second_id, vn::appfw::PluginScope::User));
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

    const vn::String key(u8"plugins.test_plugin.fail_load");
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
    const auto entries = pm->pluginEntries();
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
    const std::filesystem::path trailing = std::filesystem::temp_directory_path() / "vn_plugins_trailing";
    std::filesystem::create_directories(trailing, ec);
    const vn::String id = pm->installPlugin(vn::String((trailing / "").u8string()), vn::appfw::PluginScope::User);
    EXPECT_TRUE(id == u8"vn_plugins_trailing") << "应当取目录名，而不是写出畸形文件名";
    if (!id.empty()) {
        EXPECT_TRUE(pm->uninstallPlugin(id, vn::appfw::PluginScope::User));
    }
    std::filesystem::remove_all(trailing, ec);
    EXPECT_EQ(pm->pluginRegistrations().size(), baseline) << "临时注册应当被清掉";

    // 名字含 ':'（Windows 上是盘符/数据流分隔符）：拒绝，且不留文件。
    const std::filesystem::path bad = std::filesystem::temp_directory_path() / "vn_bad:name";
    std::filesystem::create_directories(bad, ec);
    EXPECT_TRUE(pm->installPlugin(vn::String(bad.u8string()), vn::appfw::PluginScope::User).empty()) << "含 ':' 的名字必须被拒绝";
    EXPECT_FALSE(pm->uninstallPlugin(vn::String(u8"bad/name"), vn::appfw::PluginScope::User)) << "带分隔符的 id 也必须被拒绝";
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

    const std::filesystem::path legacy(VN_LEGACY_ABI_PLUGIN);
    const std::filesystem::path future(VN_FUTURE_ABI_PLUGIN);
    ASSERT_TRUE(std::filesystem::exists(legacy)) << legacy;
    ASSERT_TRUE(std::filesystem::exists(future)) << future;

    EXPECT_EQ(pm->load(vn::String(legacy.u8string())), nullptr) << "没有 ABI 握手的库必须被拒绝";
    EXPECT_EQ(pm->load(vn::String(future.u8string())), nullptr) << "ABI 号对不上的库必须被拒绝";
    EXPECT_FALSE(pm->isLoaded(u8"legacy_plugin"));
    EXPECT_FALSE(pm->isLoaded(u8"future_plugin"));

    // 被拒绝的库连"发现"都不做：宿主读不到可以信任的元数据，也就不该把它列进列表
    // （列表里的每一条都保证是能安全读取的）。日志里给出两边的 ABI 与框架版本，够定位。
    const auto entries = pm->pluginEntries();
    EXPECT_EQ(findPluginEntry(entries, u8"legacy_plugin"), nullptr);
    EXPECT_EQ(findPluginEntry(entries, u8"future_plugin"), nullptr);

    // 安装同样被拒：注册一个用不了的库，只会在以后每次启动多一条警告。
    EXPECT_TRUE(pm->installPlugin(vn::String(legacy.u8string())).empty());
}

// 握手带回来的"构建时框架版本"要一路走到 PluginEntry，插件管理器对话框的"信息"页才有
// 东西可显示。这里与编译期宏比较，能证明整条链路（VN_DECLARE_PLUGIN → PluginAbi →
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
    // VN_APPFW_VERSION 是构建注入的窄字符串字面量（CMake 的 PROJECT_VERSION），
    // String 存 UTF-8 字节，所以按字节比较。
    const vn::String expected(std::u8string_view(reinterpret_cast<const char8_t*>(VN_APPFW_VERSION)));
    EXPECT_TRUE(shell->framework_version == expected) << "插件报告的应当是它编译时的框架版本（" VN_APPFW_VERSION "）";
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
    const std::filesystem::path chain_lib(VN_CHAIN_PLUGIN);
    std::filesystem::copy_file(chain_lib, sandbox.directory() / chain_lib.filename(), std::filesystem::copy_options::overwrite_existing, ec);
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
    EXPECT_NE(pm->load(vn::String(test_lib.u8string())), nullptr) << "显式 load() 不解析依赖";
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
    const std::filesystem::path chain_lib(VN_CHAIN_PLUGIN);
    std::filesystem::copy_file(chain_lib, sandbox.directory() / chain_lib.filename(), std::filesystem::copy_options::overwrite_existing, ec);
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
    for (const char* fixture : { VN_LOOP_A_PLUGIN, VN_LOOP_B_PLUGIN }) {
        const std::filesystem::path library(fixture);
        std::filesystem::copy_file(library, sandbox.directory() / library.filename(), std::filesystem::copy_options::overwrite_existing, ec);
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
    using vn::appfw::PluginEntry;
    using vn::appfw::PluginManager;

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
    EXPECT_TRUE(std::find(cyclic_order.begin(), cyclic_order.end(), vn::String(u8"a")) != cyclic_order.end());
    EXPECT_TRUE(std::find(cyclic_order.begin(), cyclic_order.end(), vn::String(u8"b")) != cyclic_order.end());
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
class InteractiveWaitCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"interactiveWait";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"waits for user input";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* io  = app ? app->userIO() : nullptr;
        if (io == nullptr) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed);
        }

        s_parked.store(true);
        const auto input = co_await io->getStringAsync(u8"输入点什么> ");
        s_parked.store(false);
        co_return vn::appfw::CommandResult(input.has_value() ? vn::appfw::CommandStatus::Success : vn::appfw::CommandStatus::Cancelled);
    }

    inline static std::atomic<bool> s_parked{ false };
};

VN_OBJECT_META_IMPL(InteractiveWaitCommand, vn::appfw::Command)

// 读一个整数并记录结果：用于验证 getIntAsync 的值、范围检查与控制台重新提示。
class IntReadCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"intRead";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"reads one integer";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* io  = app ? app->userIO() : nullptr;
        if (io == nullptr) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed);
        }

        const auto value = co_await io->getIntAsync(u8"n> ");
        s_value          = value;
        co_return vn::appfw::CommandResult(value.has_value() ? vn::appfw::CommandStatus::Success : vn::appfw::CommandStatus::Cancelled);
    }

    inline static std::optional<int> s_value;
};

VN_OBJECT_META_IMPL(IntReadCommand, vn::appfw::Command)

// 挂起一次之后再读入：sleepFor 在定时器线程上恢复，所以这次读（连同它的提示状态）
// 是在**非应用线程**上发起的。
class WorkerThreadReadCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"workerRead";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"reads an integer after suspending";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* io  = app ? app->userIO() : nullptr;
        if (io == nullptr) {
            co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Failed);
        }

        co_await vn::async::sleepFor(std::chrono::milliseconds(1));
        s_read_thread = std::this_thread::get_id();

        const auto value = co_await io->getIntAsync(u8"worker> ");
        s_value          = value;
        co_return vn::appfw::CommandResult(value.has_value() ? vn::appfw::CommandStatus::Success : vn::appfw::CommandStatus::Cancelled);
    }

    inline static std::optional<int> s_value;
    /// 读发起时所在的线程，用于断言这条路径确实不在应用线程上。
    inline static std::thread::id s_read_thread;
};

VN_OBJECT_META_IMPL(WorkerThreadReadCommand, vn::appfw::Command)

// 在命令内部调用"取消并排空"。
class DrainFromInsideCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"drainFromInside";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"drains from inside itself";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext* context) override
    {
        auto* app = context ? context->application() : nullptr;
        auto* cm  = app ? app->commandManager() : nullptr;
        s_drained = cm != nullptr && cm->cancelAllAndWait(std::chrono::milliseconds(50));
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static bool s_drained{ true };
};

VN_OBJECT_META_IMPL(DrainFromInsideCommand, vn::appfw::Command)

// 不配合取消、时长可调的睡眠命令：用来把"排他命令正在等待旧链"这个窗口撑开。
class UncooperativeSleepForCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    explicit UncooperativeSleepForCommand(vn::String name, std::chrono::milliseconds duration)
      : name_(std::move(name))
      , duration_(duration)
    {}

    vn::String name() const override
    {
        return name_;
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"ignores cancellation";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        s_running.store(true);
        co_await vn::async::sleepFor(duration_);
        s_running.store(false);
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<bool> s_running{ false };

  private:
    vn::String                name_;
    std::chrono::milliseconds duration_;
};

VN_OBJECT_META_IMPL(UncooperativeSleepForCommand, vn::appfw::Command)

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

    const auto name = vn::String(u8"interactiveWait");
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
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Cancelled);

    // 没有挂起交互时再取消一次是安全的（幂等）。
    io->cancelPendingInput();

    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 路径校验是基类共享的那一份：两个后端问的是同一个问题，所以它在桌面上钉住，
// 而不是只靠两个后端各自的用例间接覆盖。
namespace
{

/// 只为把基类受保护的校验摊在桌面上（测试不开生产口：子类里转一手就够了）。
///
/// 用转发函数而不是 `using`：`using` 会把三个重载一起拉进来，而其中一个（带 directory 参数的那个）是私有的，
/// 于是"并非所有重载都可访问"。转发只挑得出的那一份。
struct PathCheckProbe : vn::appfw::UserIO {
    static PathCheck check(const vn::String& text, const vn::appfw::UserIO::FileReadOptions& options)
    {
        return checkPath(text, options);
    }

    static PathCheck check(const vn::String& text, const vn::appfw::UserIO::DirReadOptions& options)
    {
        return checkPath(text, options);
    }

    static PathLineCheck checkLine(const vn::String& text, const vn::appfw::UserIO::FileReadOptions& options)
    {
        return checkPathLine(text, options);
    }

    static std::vector<vn::String> patterns(const vn::appfw::UserIO::FileReadOptions& options)
    {
        return filterPatterns(options);
    }

    static vn::String summary(const std::vector<vn::String>& extensions, bool multiple)
    {
        return requirementSummary(extensions, multiple);
    }

    static vn::String prompt(const vn::String& text, const vn::String& summary)
    {
        return promptWithRequirements(text, summary);
    }
};

/// 路径的 UTF-8 文本形式（用户就是这样打进来的）。
vn::String pathText(const std::filesystem::path& path)
{
    return vn::String(path.u8string());
}

/// Qt 文本 → vn::String（面板写进输入行与信号里的是同一串 UTF-8 字节）。
vn::String fromQString(const QString& text)
{
    const QByteArray bytes = text.toUtf8();
    return vn::String::fromUtf8(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

} // namespace

TEST(UserIOTest, PathCheckTable)
{
    const auto dir   = makeTempCaseDir("check");
    const auto stl   = dir / "a.stl";
    const auto upper = dir / "b.STL";
    const auto multi = dir / "pack.tar.gz";
    const auto notes = dir / "notes.txt";
    for (const auto& path : { stl, upper, multi, notes }) {
        touchFile(path);
    }

    const auto stl_only = vn::appfw::UserIO::FileReadOptions{ { u8".stl" } };

    // 后缀：大小写不敏感，"stl" / ".stl" / "*.stl" 三种写法等价，多段后缀也认。
    EXPECT_TRUE(PathCheckProbe::check(pathText(stl), stl_only).ok);
    EXPECT_TRUE(PathCheckProbe::check(pathText(upper), stl_only).ok);
    EXPECT_TRUE(PathCheckProbe::check(pathText(stl), vn::appfw::UserIO::FileReadOptions{ { u8"stl" } }).ok);
    EXPECT_TRUE(PathCheckProbe::check(pathText(stl), vn::appfw::UserIO::FileReadOptions{ { u8"*.stl" } }).ok);
    EXPECT_TRUE(PathCheckProbe::check(pathText(multi), vn::appfw::UserIO::FileReadOptions{ { u8"tar.gz" } }).ok);
    EXPECT_FALSE(PathCheckProbe::check(pathText(notes), stl_only).ok);
    EXPECT_NE(PathCheckProbe::check(pathText(notes), stl_only).message.find(u8"后缀不支持"), vn::String::npos);

    // 空后缀列表 = 不限；存在性由 require_existing 决定（false 是"另存为"）。
    EXPECT_TRUE(PathCheckProbe::check(pathText(notes), vn::appfw::UserIO::FileReadOptions{}).ok);
    EXPECT_FALSE(PathCheckProbe::check(pathText(dir / "missing.stl"), vn::appfw::UserIO::FileReadOptions{}).ok);

    vn::appfw::UserIO::FileReadOptions save_as;
    save_as.require_existing = false;
    save_as.extensions       = { u8".stl" };
    EXPECT_TRUE(PathCheckProbe::check(pathText(dir / "brand_new.stl"), save_as).ok);
    EXPECT_FALSE(PathCheckProbe::check(pathText(dir / "brand_new.txt"), save_as).ok) << "名字仍要带对后缀";
    EXPECT_FALSE(PathCheckProbe::check(pathText(dir), save_as).ok) << "已存在的目录不能当目标文件";

    // 一层成对的引号（从文件管理器复制来的形态）剥掉；空文本有自己的原因，不当成"不存在"。
    EXPECT_TRUE(PathCheckProbe::check(vn::String(u8"  \"") + pathText(stl) + u8"\"  ", stl_only).ok);
    EXPECT_EQ(PathCheckProbe::check(vn::String(u8"   "), stl_only).message, vn::String(u8"请输入路径"));

    // 目录读是另一种提问：不吃后缀，但要求是目录。
    EXPECT_TRUE(PathCheckProbe::check(pathText(dir), vn::appfw::UserIO::DirReadOptions{}).ok);
    EXPECT_NE(PathCheckProbe::check(pathText(stl), vn::appfw::UserIO::DirReadOptions{}).message.find(u8"不是目录"), vn::String::npos);
    EXPECT_NE(PathCheckProbe::check(pathText(dir / "nope"), vn::appfw::UserIO::DirReadOptions{}).message.find(u8"目录不存在"), vn::String::npos);

    // 多个：';' 切分、空段忽略、保序；一段坏则整条失败并指向那一段。
    const auto line = PathCheckProbe::checkLine(pathText(stl) + u8";;" + pathText(upper) + u8";", stl_only);
    ASSERT_TRUE(line.ok);
    ASSERT_EQ(line.paths.size(), 2u);
    EXPECT_EQ(line.paths[0], stl);
    EXPECT_EQ(line.paths[1], upper);
    EXPECT_NE(PathCheckProbe::checkLine(pathText(stl) + u8";" + pathText(dir / "missing.stl"), stl_only).message.find(u8"不存在"), vn::String::npos);
    EXPECT_EQ(PathCheckProbe::checkLine(vn::String(u8"; ;"), stl_only).message, vn::String(u8"请输入路径"));

    // 提示与过滤器的拼法（两个后端共用，所以在这里钉住）。
    EXPECT_EQ(PathCheckProbe::summary({ u8"stl", u8"*.obj" }, false), vn::String(u8"（.stl .obj）"));
    EXPECT_EQ(PathCheckProbe::summary({}, true), vn::String(u8"（多个用 ; 分隔）"));
    EXPECT_EQ(PathCheckProbe::prompt(u8"网格文件: ", u8"（.stl）"), vn::String(u8"网格文件（.stl）: "));
    EXPECT_EQ(PathCheckProbe::prompt(u8"输入名称> ", u8"（.stl）"), vn::String(u8"输入名称（.stl）> "));
    const auto patterns = PathCheckProbe::patterns(stl_only);
    ASSERT_EQ(patterns.size(), 1u);
    EXPECT_EQ(patterns[0], vn::String(u8"*.stl"));

    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 文件读在控制台上：提示带上"要什么"，不合法的输入重新提示**并保留原文**（一行里几条路径时，
// 清空输入行等于让人重打），合法的输入原样返回。
TEST(UserIOTest, FileReadValidatesAndKeepsTheRejectedTextInTheLine)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* io = vn::obj_cast<guifw::VisualUserIO>(app->userIO());
    ASSERT_NE(io, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* input = panel.impl<QWidget>()->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);

    const auto dir   = makeTempCaseDir("guifile");
    const auto stl   = dir / "mesh.stl";
    const auto notes = dir / "notes.txt";
    touchFile(stl);
    touchFile(notes);

    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto pump_until = [&](const std::function<bool()>& done) {
        while (!done()) {
            EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
            EXPECT_LT(std::chrono::steady_clock::now(), deadline) << "等待控制台超时";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    std::optional<std::filesystem::path> picked;
    std::thread                          runner([&] { picked = io->getFileAsync(u8"网格> ", vn::appfw::UserIO::FileReadOptions{ { u8".stl" } }).result(); });

    // 提示里带上后缀：键盘用户不该靠悬浮文案才知道要什么。
    pump_until([&] { return panel.impl<QWidget>()->findChild<QPlainTextEdit*>()->toPlainText().contains(u8"网格（.stl）> "); });

    // 后缀不符：一行原因 + 原文留在输入行里，读仍在等。
    panel.lineEntered.trigger(pathText(notes));
    pump_until([&] { return input->text().contains(QStringLiteral("notes.txt")); });
    EXPECT_FALSE(picked.has_value()) << "拒绝的输入不能让读结束";

    // 合法输入：原样给出，命令读到的是同一个路径。
    panel.lineEntered.trigger(pathText(stl));
    runner.join();
    ASSERT_TRUE(picked.has_value());
    EXPECT_EQ(*picked, stl);

    bindConsole(app, nullptr);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 多个文件写在一行里：';' 分隔、顺序保持；多值读用同一个交互槽位（一整条读只有一次输入）。
TEST(UserIOTest, MultipleFileReadTakesOneSemicolonSeparatedLine)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* io = vn::obj_cast<guifw::VisualUserIO>(app->userIO());
    ASSERT_NE(io, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    const auto dir = makeTempCaseDir("guifiles");
    const auto one = dir / "one.stl";
    const auto two = dir / "two.stl";
    touchFile(one);
    touchFile(two);

    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto pump_until = [&](const std::function<bool()>& done) {
        while (!done()) {
            EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
            EXPECT_LT(std::chrono::steady_clock::now(), deadline) << "等待控制台超时";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    std::optional<vn::appfw::UserIO::PathList> picked;
    std::thread runner([&] { picked = io->getFilesAsync(u8"输入> ", vn::appfw::UserIO::FileReadOptions{ { u8"stl" } }).result(); });

    pump_until([&] { return output->toPlainText().contains(u8"多个用 ; 分隔"); });

    panel.lineEntered.trigger(pathText(one) + u8";" + pathText(two));
    runner.join();
    ASSERT_TRUE(picked.has_value());
    ASSERT_EQ(picked->size(), 2u);
    EXPECT_EQ((*picked)[0], one);
    EXPECT_EQ((*picked)[1], two);

    bindConsole(app, nullptr);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 输入行的选择器：读开始时装上、读结束时撤掉，且只在这段时间里存在（按钮不能活过它的提示）。
TEST(UserIOTest, InputActionIsOfferedOnlyWhileTheReadWaits)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* io = vn::obj_cast<guifw::VisualUserIO>(app->userIO());
    ASSERT_NE(io, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* input = panel.impl<QWidget>()->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->findChild<QToolButton*>(), nullptr) << "命令行的普通状态没有按钮";

    guifw::ConsoleInputAction action;
    action.kind    = guifw::ConsoleInputKind::File;
    action.filter  = vn::String(u8"网格文件 (*.stl);;所有文件 (*)");
    action.tooltip = vn::String(u8"选择文件…（.stl）");
    panel.setInputAction(action);

    auto* button = input->findChild<QToolButton*>();
    ASSERT_NE(button, nullptr) << "请求文件时输入行里要有一个选文件的按钮";
    EXPECT_EQ(button->toolTip(), QString::fromUtf8("选择文件…（.stl）"));
    EXPECT_FALSE(button->icon().isNull());

    // 目录读换的是图标与文案（同一个按钮位置）。
    panel.setInputAction(guifw::ConsoleInputAction{ guifw::ConsoleInputKind::Directories, {}, vn::String(u8"选择目录…") });
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_NE(input->findChild<QToolButton*>(), nullptr);
    EXPECT_EQ(input->findChild<QToolButton*>()->toolTip(), QString::fromUtf8("选择目录…"));

    panel.setInputAction(guifw::ConsoleInputAction{});
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(input->findChild<QToolButton*>(), nullptr) << "读结束后按钮必须消失";

    bindConsole(app, nullptr);
}

// 点按钮只是"更快地填那一行"：选中的路径落进输入行、**不自动提交**（回车才算数），而且多值读两次点选是
// 追加（与手打 "a;b" 同构）。对话框在 offscreen 上是 Qt 自己的，用例用一次性定时器把它关掉。
TEST(UserIOTest, PickingFilesFillsTheLineWithoutSubmitting)
{
    // 无头平台驱动不了真实文件对话框：这条用例的判法是"替用户点一下对话框"（拿 activeModalWidget 当 QFileDialog 去脚本它），
    // offscreen/minimal 下那个模态窗口根本不会出现，等它的人只会空转到死 —— CTest 一律用 offscreen
    // （见 tests/test_gui/CMakeLists.txt），所以这里如实跳过，而不是让仓库门禁自己超时。
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
        GTEST_SKIP() << "无头平台没有可脚本化的文件对话框（平台：" << platform.toStdString() << "）";
    }

    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* io = vn::obj_cast<guifw::VisualUserIO>(app->userIO());
    ASSERT_NE(io, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* input = panel.impl<QWidget>()->findChild<QLineEdit*>();
    ASSERT_NE(input, nullptr);

    const auto dir = makeTempCaseDir("guipick");
    const auto one = dir / "one.stl";
    const auto two = dir / "two.stl";
    touchFile(one);
    touchFile(two);

    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    const auto pump_until = [&](const std::function<bool()>& done) {
        while (!done()) {
            EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
            EXPECT_LT(std::chrono::steady_clock::now(), deadline) << "等待控制台超时";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    bool       dialog_seen   = false;
    const auto script_dialog = [&dialog_seen](bool accept, const std::filesystem::path& chosen) {
        QTimer::singleShot(0, [accept, chosen, &dialog_seen] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr) {
                return;
            }
            dialog_seen = true;
            if (accept) {
                dialog->selectFile(QString::fromStdString(narrow(chosen)));
                // accept() 在 QFileDialog 里是 protected，所以走元对象调用 —— 校验仍是对话框自己那一条。
                QMetaObject::invokeMethod(dialog, "accept");
            }
            else {
                dialog->reject();
            }
        });
        // 兜底：万一对话框没按预期出现，也别让用例卡在模态循环里。
        QTimer::singleShot(5000, [] {
            if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                dialog->reject();
            }
        });
    };

    std::optional<vn::appfw::UserIO::PathList> picked;
    std::thread runner([&] { picked = io->getFilesAsync(u8"输入> ", vn::appfw::UserIO::FileReadOptions{ { u8"stl" } }).result(); });

    pump_until([&] { return input->findChild<QToolButton*>() != nullptr; });
    auto* button = input->findChild<QToolButton*>();
    ASSERT_NE(button, nullptr);

    // 一次点选：路径落进输入行，但读还在等 —— 浏览只是填行。
    script_dialog(/*accept=*/true, one);
    button->click();
    EXPECT_TRUE(dialog_seen) << "点按钮应当打开文件对话框";
    EXPECT_EQ(input->text(), QString::fromStdString(narrow(one)));
    EXPECT_FALSE(picked.has_value()) << "浏览不提交：按回车才算数";

    // 再一次点选：多值读追加（"点两次 = 两个路径"）。
    script_dialog(/*accept=*/true, two);
    button->click();
    EXPECT_EQ(input->text(), QString::fromStdString(narrow(one) + ";" + narrow(two)));

    // 取消对话框：输入行与读都不动。
    const QString before_cancel = input->text();
    script_dialog(/*accept=*/false, {});
    button->click();
    EXPECT_EQ(input->text(), before_cancel);
    EXPECT_FALSE(picked.has_value());

    // 回车提交：走的是与手打完全同一条校验路径。
    panel.lineEntered.trigger(fromQString(input->text()));
    runner.join();
    ASSERT_TRUE(picked.has_value());
    ASSERT_EQ(picked->size(), 2u);
    EXPECT_EQ((*picked)[0], one);
    EXPECT_EQ((*picked)[1], two);

    // 读结束 ⇒ 按钮撤下（撤下用 deleteLater，所以延迟销毁要自己处理）。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(input->findChild<QToolButton*>(), nullptr) << "按钮不能活过它的读";

    bindConsole(app, nullptr);
    std::error_code error;
    std::filesystem::remove_all(dir, error);
}

// 命令的输出来自它恢复时所在的线程（定时器线程、IO 线程）：从工作线程调用
// putString() 也必须落到应用线程上（面板是 QWidget）。旧实现直接写面板。
TEST(UserIOTest, OutputFromAWorkerThreadIsMarshalledToTheApplicationThread)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* io = app->userIO();
    ASSERT_NE(io, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    std::thread writer([io] { io->putString(vn::String(u8"from worker")); });
    writer.join();

    EXPECT_FALSE(output->toPlainText().contains(u8"from worker")) << "工作线程不得直接写控制台面板";
    ASSERT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
    EXPECT_TRUE(output->toPlainText().contains(u8"from worker")) << "输出应已投递到应用线程";

    bindConsole(app, nullptr);
}

// 同一时刻只允许一个交互等待：第二个读立即以 nullopt 收尾，而不是和第一个共享
// 同一个完成事件与结果字段（旧实现里两次读会互相覆盖）。
TEST(UserIOTest, SecondReadIsRefusedWhileOneIsPending)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    auto* io = app->userIO();
    ASSERT_NE(cm, nullptr);
    ASSERT_NE(io, nullptr);

    const auto name = vn::String(u8"intRead");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<IntReadCommand>(name));
    cm->clearHistory();
    IntReadCommand::s_value.reset();

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    cm->executeDetached(name);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!output->toPlainText().contains(u8"n> ")) {
        ASSERT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "第一个读没有显示提示";
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // 第二个读必须立刻收尾，并且不能把第一个读叫醒（否则它会拿到别的结果）。
    const auto refused = io->getDoubleAsync(u8"second> ").result();
    EXPECT_FALSE(refused.has_value()) << "一个交互挂起时，第二个读必须被拒绝";
    EXPECT_EQ(cm->historyCount(), 0) << "第一个读仍应在等待";

    // 收尾：取消挂起的交互，命令以 Cancelled 结束。
    io->cancelPendingInput();
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    EXPECT_FALSE(IntReadCommand::s_value.has_value());

    bindConsole(app, nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 整数读：能装进 int 的值必须原样返回（旧的 int8_t 会把 1000 变成 -24），装不下的
// 要求重新输入而不是静默回绕。
TEST(UserIOTest, IntReadKeepsItsValueAndRepromptsOnOverflow)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"intRead");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<IntReadCommand>(name));

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto wait_for = [&](const char8_t* needle) {
        while (!output->toPlainText().contains(needle)) {
            EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
            EXPECT_LT(std::chrono::steady_clock::now(), deadline) << "等待控制台输出超时";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    // 1000 超出 int8_t，但对 int 完全合法：必须原样返回。
    cm->clearHistory();
    IntReadCommand::s_value.reset();
    cm->executeDetached(name);
    wait_for(u8"n> ");
    panel.lineEntered.trigger(vn::String(u8"1000"));
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(IntReadCommand::s_value.has_value());
    EXPECT_EQ(*IntReadCommand::s_value, 1000);

    // 超出 int 范围：重新提示，读继续等待，随后的合法输入生效。
    cm->clearHistory();
    IntReadCommand::s_value.reset();
    cm->executeDetached(name);
    wait_for(u8"n> ");
    panel.lineEntered.trigger(vn::String(u8"99999999999"));
    EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
    EXPECT_TRUE(output->toPlainText().contains(u8"请输入整数")) << "超出 int 的文本要重新提示";
    EXPECT_EQ(cm->historyCount(), 0) << "重新提示后读仍在等待";

    panel.lineEntered.trigger(vn::String(u8"42"));
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    ASSERT_TRUE(IntReadCommand::s_value.has_value());
    EXPECT_EQ(*IntReadCommand::s_value, 42);

    bindConsole(app, nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 重复绑定同一个面板不能重复挂 handler：否则一行输入会被执行两次。
TEST(UserIOTest, RebindingTheConsoleDoesNotRunALineTwice)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    bindConsole(app, &panel); // 二次绑定：旧实现会在这里再挂一份 handler

    const auto name = vn::String(u8"rebindProbe");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<ThrowingCommand>(name));
    cm->clearHistory();

    panel.lineEntered.trigger(name);

    // 每条执行的命令都会记一条历史，因此历史条数就是执行次数。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_EQ(cm->historyCount(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(cm->historyCount(), 1) << "同一行只能执行一次";

    bindConsole(app, nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 控制台里输入的命令返回"不适用"时也一样：黄色提示而不是红色错误（旧行为两种都写成
// ConsoleMessageType::Error，外加一句"命令执行失败"）。
TEST(UserIOTest, NotApplicableTypedInTheConsoleIsAHintNotAnError)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"notApplicable");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<NotApplicableCommand>(name));
    NotApplicableCommand::s_message = vn::String(u8"当前文档不是模型文档");

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto run_once = [&] {
        cm->clearHistory();
        NotApplicableCommand::s_runs = 0;
        panel.lineEntered.trigger(name);
        while (cm->historyCount() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
        EXPECT_EQ(NotApplicableCommand::s_runs.load(), 1);
        EXPECT_EQ(cm->historyCount(), 1);
    };

    run_once();
    ASSERT_TRUE(cm->historyAt(0).has_value());
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::NotApplicable);
    EXPECT_TRUE(output->toPlainText().contains(u8"当前文档不是模型文档")) << "解释要看得见";
    EXPECT_FALSE(output->toPlainText().contains(u8"命令执行失败"));

    // 颜色：提示色而不是错误色。基准色由面板自己写，不写死色值。
    panel.append(guifw::ConsoleMessageType::Warning, vn::String(u8"基准-警告"));
    panel.append(guifw::ConsoleMessageType::Error, vn::String(u8"基准-错误"));
    const auto hint  = consoleLineColor(output, QStringLiteral("当前文档不是模型文档"));
    const auto warn  = consoleLineColor(output, QStringLiteral("基准-警告"));
    const auto error = consoleLineColor(output, QStringLiteral("基准-错误"));
    EXPECT_EQ(hint, warn) << "不适用是警告级提示";
    EXPECT_NE(hint, error) << "不适用不是错误";

    // 没有解释：除了"用户敲进来的那一行"的回显，控制台不写任何东西（连"命令执行失败"都不写）。
    panel.clear();
    NotApplicableCommand::s_message = vn::String();
    run_once();
    EXPECT_EQ(output->toPlainText().trimmed(), QStringLiteral("notApplicable")) << "除了输入回显，不该有任何输出";

    NotApplicableCommand::s_message = vn::String(u8"当前上下文没有可作用的东西");
    bindConsole(app, nullptr);
    cm->clearHistory();
    cm->unregisterCommand(name);
}

// 命令集合变化要能通知出去：缓存的列表（控制台补全）靠它刷新。旧的
// register/unregister 不发任何事件，绑定控制台之后注册的命令永远进不了补全。
TEST(UserIOTest, CommandsChangedReportsRegistryAndAliasEdits)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    int  changes = 0;
    auto handler = cm->commandsChanged.connect([&changes](vn::appfw::CommandManager&, vn::EventArgs&) { ++changes; });

    const auto name = vn::String(u8"changedProbe");
    EXPECT_FALSE(cm->unregisterCommand(name)) << "未注册的名字取消失败";
    EXPECT_EQ(changes, 0) << "集合没变就不通知";

    ASSERT_TRUE(cm->registerCommand<IntReadCommand>(name));
    EXPECT_EQ(changes, 1);

    cm->setCommandEnabled(name, false);
    EXPECT_EQ(changes, 2) << "禁用会从补全列表里拿掉一项";
    cm->setCommandEnabled(name, false);
    EXPECT_EQ(changes, 2) << "没有实际变化就不重复通知";

    const auto alias = vn::String(u8"changedProbeAlias");
    EXPECT_FALSE(cm->unregisterAlias(alias));
    EXPECT_EQ(changes, 2);
    ASSERT_TRUE(cm->registerAlias(alias, name));
    EXPECT_EQ(changes, 3);
    ASSERT_TRUE(cm->unregisterAlias(alias));
    EXPECT_EQ(changes, 4);

    EXPECT_TRUE(cm->unregisterCommand(name));
    EXPECT_EQ(changes, 5);

    EXPECT_TRUE(cm->setCommandEnabled(name, true)) << "未注册的命令只是记住偏好";
    EXPECT_EQ(changes, 5) << "注册表没变就不通知";

    handler.disconnect();
    cm->clearHistory();
}

// 读可以从任意线程发起：命令在定时器/IO 线程上恢复后调用 getXxxAsync，所以提示的
// 记帐（currentPrompt_）不能由发起线程直接写。这里让命令先 sleepFor 再读，断言
// 那条路径确实不在应用线程上，并且整条链路（提示、重新提示、取值）照常工作。
TEST(UserIOTest, ReadStartedOnAWorkerThreadIsMarshalledAndReprompts)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name = vn::String(u8"workerRead");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<WorkerThreadReadCommand>(name));

    guifw::ConsolePanel panel;
    bindConsole(app, &panel);
    auto* output = panel.impl<QWidget>()->findChild<QPlainTextEdit*>();
    ASSERT_NE(output, nullptr);

    const auto countPrompt = [output] {
        const auto text = output->toPlainText().toStdString();
        int        n    = 0;
        for (auto at = text.find("worker> "); at != std::string::npos; at = text.find("worker> ", at + 1)) {
            ++n;
        }
        return n;
    };

    const auto app_thread = std::this_thread::get_id();
    const auto deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto wait_for   = [&](const std::function<bool()>& done) {
        while (!done()) {
            EXPECT_TRUE(app->mainThreadDispatcher()->deliverPostedCalls());
            EXPECT_LT(std::chrono::steady_clock::now(), deadline) << "等待控制台输出超时";
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    cm->clearHistory();
    WorkerThreadReadCommand::s_value.reset();
    cm->executeDetached(name);

    wait_for([&] { return countPrompt() >= 1; });
    EXPECT_NE(WorkerThreadReadCommand::s_read_thread, app_thread) << "用例前提：这次读必须在非应用线程上发起（否则钉不住这条路径）";

    // 非法输入：重新提示必须用同一个提示文本（它来自编组写入的提示记帐）。
    panel.lineEntered.trigger(vn::String(u8"abc"));
    wait_for([&] { return countPrompt() >= 2; });

    // 合法输入：值原样返回，命令成功结束。
    panel.lineEntered.trigger(vn::String(u8"7"));
    wait_for([&] { return cm->historyCount() >= 1; });

    ASSERT_TRUE(WorkerThreadReadCommand::s_value.has_value());
    EXPECT_EQ(*WorkerThreadReadCommand::s_value, 7);
    EXPECT_EQ(cm->historyAt(0)->result.status(), vn::appfw::CommandStatus::Success);

    bindConsole(app, nullptr);
    cm->unregisterCommand(name);
    cm->clearHistory();
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

    const auto name = vn::String(u8"drainFromInside");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<DrainFromInsideCommand>(name));
    cm->clearHistory();
    DrainFromInsideCommand::s_drained = true;

    const auto result = cm->executeCommandAndWait(name);
    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);
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

    const auto victim = vn::String(u8"uncooperativeTimed");
    const auto taker  = vn::String(u8"takeover");
    cm->unregisterCommand(victim);
    cm->unregisterCommand(taker);
    ASSERT_TRUE(cm->registerCommand(UncooperativeSleepForCommand::desc(), victim, [victim] {
        return new UncooperativeSleepForCommand(victim, std::chrono::milliseconds(800));
    }));
    ASSERT_TRUE(cm->registerCommand<ExclusiveTakeOverCommand>(taker));
    ExclusiveTakeOverCommand::s_ran         = false;
    UncooperativeSleepForCommand::s_running = false;
    cm->clearHistory();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // 旧链在跑，且它无视取消 ⇒ 排他命令会停在"等待旧链收尾"上（上界 2s）。
    auto                     victim_task = cm->executeCommandAsync(victim);
    vn::appfw::CommandResult victim_result;
    std::thread              victim_runner([&] { victim_result = std::move(victim_task).result(); });
    while (!UncooperativeSleepForCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(UncooperativeSleepForCommand::s_running.load());

    auto                     taker_task = cm->executeCommandAsync(taker);
    vn::appfw::CommandResult taker_result;
    std::thread              taker_runner([&] { taker_result = std::move(taker_task).result(); });

    // 让排他命令确实进入等待：它自己不执行，没有别的可观察点。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto start = std::chrono::steady_clock::now();
    cm->cancelAll();
    taker_runner.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // 被打断：立刻以 Cancelled 结束（而不是等满 2s 后 Failed），命令体从未执行。
    EXPECT_EQ(taker_result.status(), vn::appfw::CommandStatus::Cancelled);
    EXPECT_FALSE(ExclusiveTakeOverCommand::s_ran);
    EXPECT_LT(elapsed, 1000) << "cancelAll() 应立即结束等待中的接管";

    while (UncooperativeSleepForCommand::s_running.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    victim_runner.join();
    EXPECT_EQ(victim_result.status(), vn::appfw::CommandStatus::Success) << "不配合取消的旧链自己跑完";

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
class ReentrancyProbeCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    explicit ReentrancyProbeCommand(vn::String name)
      : name_(std::move(name))
    {}

    vn::String name() const override
    {
        return name_;
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"reentrancy probe";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::None;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        s_runs.fetch_add(1);
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<int> s_runs{ 0 };

  private:
    vn::String name_;
};

VN_OBJECT_META_IMPL(ReentrancyProbeCommand, vn::appfw::Command)

// 慢排他命令：记录自身并发数。
class SlowExclusiveProbeCommand : public vn::appfw::Command {
    VN_OBJECT_META_DECL;

  public:
    vn::String name() const override
    {
        return u8"exclusiveSlow";
    }

    vn::String group() const override
    {
        return u8"Test";
    }

    vn::String description() const override
    {
        return u8"slow exclusive";
    }

    vn::appfw::CommandFlags flags() const override
    {
        return vn::appfw::CommandFlags::Exclusive;
    }

    vn::async::Task<vn::appfw::CommandResult> execute(vn::appfw::CommandExecutionContext*) override
    {
        const int now  = s_in_flight.fetch_add(1) + 1;
        int       prev = s_max.load();
        while (prev < now && !s_max.compare_exchange_weak(prev, now)) {}
        co_await vn::async::sleepFor(std::chrono::milliseconds(200));
        s_in_flight.fetch_sub(1);
        co_return vn::appfw::CommandResult(vn::appfw::CommandStatus::Success);
    }

    inline static std::atomic<int> s_in_flight{ 0 };
    inline static std::atomic<int> s_max{ 0 };
};

VN_OBJECT_META_IMPL(SlowExclusiveProbeCommand, vn::appfw::Command)

} // namespace

// 重入 1：工厂在注册过程中重入管理器的注册表（注册别的名字、列举、注销自己）。
// 工厂是在 registry_mutex 之外调用的，所以这些都不得死锁。
TEST_F(GuiTest, CommandManager_ReentrantFactoryIsSafe)
{
    auto* app = GuiEnv::app.get();
    ASSERT_NE(app, nullptr);
    auto* cm = app->commandManager();
    ASSERT_NE(cm, nullptr);

    const auto name  = vn::String(u8"reentrantFactory");
    const auto other = vn::String(u8"reentrantFactoryOther");
    cm->unregisterCommand(name);
    cm->unregisterCommand(other);
    ReentrancyProbeCommand::s_runs = 0;

    bool inside_registered_other  = false;
    bool inside_infos_ok          = false;
    bool inside_unregistered_self = false;

    const bool registered = cm->registerCommand(ReentrancyProbeCommand::desc(), name, [&]() -> vn::appfw::Command* {
        inside_registered_other  = cm->registerCommand(ReentrancyProbeCommand::desc(), other, [other] { return new ReentrancyProbeCommand(other); });
        inside_infos_ok          = !cm->commandInfos().empty();
        inside_unregistered_self = cm->unregisterCommand(name);
        return new ReentrancyProbeCommand(name);
    });

    EXPECT_TRUE(registered);
    EXPECT_TRUE(inside_registered_other);
    EXPECT_TRUE(inside_infos_ok);
    EXPECT_FALSE(inside_unregistered_self) << "探测时它还没注册成功";
    EXPECT_TRUE(cm->isRegistered(name));
    EXPECT_EQ(cm->executeCommandAndWait(name).status(), vn::appfw::CommandStatus::Success);
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

    const auto first  = vn::String(u8"reentrantFirst");
    const auto second = vn::String(u8"reentrantSecond");
    cm->unregisterCommand(first);
    cm->unregisterCommand(second);
    ASSERT_TRUE(cm->registerCommand(ReentrancyProbeCommand::desc(), first, [first] { return new ReentrancyProbeCommand(first); }));
    ASSERT_TRUE(cm->registerCommand(ReentrancyProbeCommand::desc(), second, [second] { return new ReentrancyProbeCommand(second); }));
    cm->clearHistory();
    ReentrancyProbeCommand::s_runs = 0;

    std::atomic<int> notifications{ 0 };
    std::atomic<int> nested_status{ -1 };

    // 处理函数在通知里增删处理函数：本轮新增的不应被调用，注销的也不应。
    std::atomic<int> late_handler_calls{ 0 };
    vn::Connection   late_handler_id{};
    auto             subscription = cm->executed.connect([&](vn::appfw::CommandManager& manager, vn::appfw::CommandExecutedEventArgs& args) {
        const auto* c = args.command();
        if (c == nullptr || c->name() != first) {
            return;
        }
        notifications.fetch_add(1);
        // 重入：通知里再跑一条命令（同步入口，当前线程就是主线程）。
        nested_status.store(static_cast<int>(manager.executeCommandAndWait(second).status()));
        // 重入：注销刚跑完的命令（实例与注册项是两回事，不应影响本次上报）。
        static_cast<void>(manager.unregisterCommand(first));
        // 重入：本轮新增的处理函数不应在本次通知里被调用。
        late_handler_id = manager.executed.connect(
            [&late_handler_calls](vn::appfw::CommandManager&, vn::appfw::CommandExecutedEventArgs&) { late_handler_calls.fetch_add(1); });
    });

    const auto result = cm->executeCommandAndWait(first);
    subscription.disconnect();

    // 触发期间新增的处理函数：本次不生效，但从下一条命令开始生效。
    const auto after = cm->executeCommandAndWait(second);

    // 它捕获的是本用例栈上的对象：用例结束前必须注销，否则会挂到下一条命令上。
    late_handler_id.disconnect();

    EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(after.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(notifications.load(), 1);
    EXPECT_EQ(nested_status.load(), static_cast<int>(vn::appfw::CommandStatus::Success));
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

    const auto name = vn::String(u8"exclusiveSlow");
    cm->unregisterCommand(name);
    ASSERT_TRUE(cm->registerCommand<SlowExclusiveProbeCommand>(name));
    cm->clearHistory();
    SlowExclusiveProbeCommand::s_in_flight = 0;
    SlowExclusiveProbeCommand::s_max       = 0;

    vn::appfw::CommandResult result_a;
    vn::appfw::CommandResult result_b;
    std::thread              runner_a([&] { result_a = cm->executeCommandAndWait(name); });
    std::thread              runner_b([&] { result_b = cm->executeCommandAndWait(name); });
    runner_a.join();
    runner_b.join();

    EXPECT_EQ(SlowExclusiveProbeCommand::s_max.load(), 1) << "两个排他命令不得并发执行";

    // 两种正确结局：后到者接管（前者已收尾 ⇒ 串行成功），或在前者建链前抢先落败
    // 被串行门拒绝。至少有一个真的执行了。
    const bool a_ok = result_a.status() == vn::appfw::CommandStatus::Success;
    const bool b_ok = result_b.status() == vn::appfw::CommandStatus::Success;
    EXPECT_TRUE(a_ok || b_ok) << "至少有一个排他命令应该执行";
    EXPECT_EQ(cm->runningCount(), 0);

    cm->clearHistory();
    cm->unregisterCommand(name);
}

namespace
{

/// 面板用例用的文档类型：最小实现，dirty 由构造参数给。
class PanelDocument : public vn::appfw::Document {
  public:
    explicit PanelDocument(bool dirty)
      : dirty_(dirty)
    {}

    vn::String typeId() const override
    {
        return u8"panel_type";
    }

    vn::String title() const override
    {
        return u8"面板文档";
    }

    bool isDirty() const override
    {
        return dirty_;
    }

  private:
    bool dirty_ = false;
};

/// 面板用例用的打开载荷：框架只按它的 C++ 类型（它的元数据）匹配，内容是什么由用例与类型自己约定。
struct PanelPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    bool dirty = false;
};

VN_OBJECT_META_IMPL(PanelPayload, vn::Object)

} // namespace

// 文档管理器面板：列表是**注册表**（类型 + 谁注册的 + 能开什么），加上当前打开的文档。
// 这条用例钉三件最容易悄悄坏掉的事：①“能开什么”真的列了出来（这就是“为什么这个文件打不开”的答案）；
// ②实时状态（当前文档标记、未保存）跟得上；③面板**只读** —— 没有能改注册表的按钮。
TEST(DocumentManagerDialogTest, ListsWhatIsRegisteredAndWhatIsOpen)
{
    // 独立的管理器：这条用例不需要应用，也不该把类型注册进共享的那一个。
    vn::appfw::DocumentManager documents;

    vn::appfw::DocumentTypeRegistration creatable;
    creatable.type_id      = u8"panel_type";
    creatable.display_name = u8"面板类型";
    creatable.description  = u8"面板上的一行说明";
    creatable.create       = [] { return new PanelDocument(false); };
    ASSERT_TRUE(documents.registerType(creatable));

    vn::appfw::DocumentTypeRegistration open_only;
    open_only.type_id = u8"panel_open_only";
    ASSERT_TRUE(documents.registerType(open_only));

    ASSERT_TRUE(documents.registerOpener<PanelPayload>({
      .type_id       = u8"panel_type",
      .priority      = 1,
      .open          = [](const PanelPayload& payload) -> vn::appfw::Document* { return new PanelDocument(payload.dirty); },
      .source_scheme = u8"file",
    }));

    vn::appfw::Document* current = documents.create(u8"panel_type");
    ASSERT_NE(current, nullptr);
    ASSERT_TRUE(documents.setCurrent(current));
    PanelPayload dirty_payload;
    dirty_payload.dirty = true;
    ASSERT_NE(documents.open(&dirty_payload), nullptr);

    auto* dialog = new guifw::DocumentManagerDialog(&documents);
    ASSERT_NE(dialog, nullptr);
    ASSERT_NO_FATAL_FAILURE(dialog->refresh());

    auto* root = dialog->impl<QWidget>();
    ASSERT_NE(root, nullptr);

    // 两张表：类型表 6 列、文档表 4 列（按列数区分，比按 findChild 顺序稳）。
    QTableWidget* types_table     = nullptr;
    QTableWidget* documents_table = nullptr;
    for (QTableWidget* table : root->findChildren<QTableWidget*>()) {
        if (table->columnCount() == 6) {
            types_table = table;
        }
        if (table->columnCount() == 4) {
            documents_table = table;
        }
    }
    ASSERT_NE(types_table, nullptr) << "类型表";
    ASSERT_NE(documents_table, nullptr) << "文档表";

    ASSERT_EQ(types_table->rowCount(), 2);
    EXPECT_EQ(types_table->item(0, 0)->text(), QStringLiteral("panel_open_only")) << "按 id 排序";
    EXPECT_EQ(types_table->item(0, 3)->text(), QStringLiteral("否（只能打开）")) << "没有 create 工厂要如实说";
    EXPECT_EQ(types_table->item(1, 0)->text(), QStringLiteral("panel_type"));
    EXPECT_EQ(types_table->item(1, 1)->text(), QStringLiteral("面板类型"));
    EXPECT_EQ(types_table->item(1, 2)->text(), QStringLiteral("主机")) << "没有注册者标签 ⇒ 主机";
    EXPECT_EQ(types_table->item(1, 3)->text(), QStringLiteral("是"));
    EXPECT_TRUE(types_table->item(1, 4)->text().contains(QStringLiteral("PanelPayload"))) << "“能开什么”来自打开器，而载荷名来自载荷自己的元数据";
    EXPECT_EQ(types_table->item(1, 5)->text(), QStringLiteral("file"));

    ASSERT_EQ(documents_table->rowCount(), 2);
    EXPECT_TRUE(documents_table->item(0, 0)->text().startsWith(QStringLiteral("● "))) << "当前文档有标记";
    EXPECT_EQ(documents_table->item(0, 1)->text(), QStringLiteral("panel_type"));
    EXPECT_EQ(documents_table->item(0, 2)->text(), QStringLiteral("—（没有来源）")) << "新建的文档没有来源，面板要说清楚而不是留空";
    EXPECT_EQ(documents_table->item(0, 3)->text(), QStringLiteral("已保存"));
    EXPECT_EQ(documents_table->item(1, 3)->text(), QStringLiteral("未保存"));
    EXPECT_FALSE(documents_table->item(1, 0)->text().startsWith(QStringLiteral("● "))) << "不是当前文档";

    // 过滤器对两张表都生效。
    auto* filter = root->findChild<QLineEdit*>();
    ASSERT_NE(filter, nullptr) << "筛选框";
    filter->setText(QStringLiteral("panel_open_only"));
    EXPECT_FALSE(types_table->isRowHidden(0));
    EXPECT_TRUE(types_table->isRowHidden(1)) << "类型表也吃过滤";
    EXPECT_TRUE(documents_table->isRowHidden(0)) << "文档表也吃过滤";

    // 面板只读：这里没有能改注册表的按钮。
    const auto buttons = root->findChildren<QPushButton*>();
    ASSERT_FALSE(buttons.isEmpty());
    for (QPushButton* button : buttons) {
        const QString text = button->text();
        EXPECT_TRUE(text == QStringLiteral("刷新") || text == QStringLiteral("关闭"))
            << "文档管理器只有刷新与关闭（不该有启用/禁用），实际是: " << text.toStdString();
    }

    delete dialog;
}
