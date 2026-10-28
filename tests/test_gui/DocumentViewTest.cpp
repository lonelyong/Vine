// 视图层：注册表（DocumentViewRegistry）+ 中央区宿主（CentralDocumentHost）。
//
// 这条用例钉住宿主契约里最容易被写坏的四条：
//   ① 没东西要显示之前**不接管**中央区（外壳自己放在那里的东西还在）；
//   ② 视图**按文档实例**懒建：没显示过的文档没有视图；同一份切回来复用同一个实例，它的状态还在；
//   ③ 文档关闭 ⇒ 它的视图被销毁（不攒着）；
//   ④ 没登记视图的类型 ⇒ 说明页（点出类型 id），不是空白、也不是别的类型的视图。
//
// 用例自带文档管理器 / 注册表 / 窗口，不碰进程里那个真应用，所以彼此与别的用例互不影响。

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QLabel>
#include <QPointer>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <vector>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/CommandTarget.hpp>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

namespace
{

constexpr const char8_t* kShownType = u8"view_test";       ///< 登记了视图的类型
constexpr const char8_t* kBlindType = u8"view_test_blind"; ///< 没登记视图的类型

/// 用例文档：报一个类型 id，标题固定，方便断言"显示的是哪一份"。
class TestDocument : public vn::appfw::Document {
  public:
    explicit TestDocument(vn::String type_id) : type_id_(std::move(type_id)) {}

    vn::String typeId() const override { return type_id_; }
    vn::String title() const override { return u8"用例文档"; }
    bool       isDirty() const override { return false; }

  private:
    vn::String type_id_;
};

/// 拒绝关闭的用例文档：浮动窗口的 X 必须走管理器，管理器则尊重 canClose()。
class VetoDocument final : public TestDocument {
  public:
    explicit VetoDocument(vn::String type_id) : TestDocument(std::move(type_id)) {}

    bool canClose() const override { return false; }
};

/// 用例载荷：框架只按类型匹配，这里只要有个能造文档的载荷。
struct TestPayload final : public vn::Object {
    VN_OBJECT_META_DECL;

    vn::String type_id;
};

VN_OBJECT_META_IMPL(TestPayload, vn::Object)

/// 视图的记账本：造过几份、销毁过几份。
struct ViewBook {
    int                   created   = 0;
    int                   destroyed = 0;
};

/// 用例视图：带自己的状态（被激活过几次），用来钉"按实例缓存 ⇒ 状态还在"。
class TestView final : public vn::appfw::gui::DocumentView {
  public:
    TestView(vn::appfw::Document& document, ViewBook& book)
      : DocumentView(document, new QWidget())
      , book_(&book)
    {
        ++book_->created;
    }

    ~TestView() override { ++book_->destroyed; }

    vn::appfw::gui::UIElement* content() override { return this; }
    void                       activate() override { ++activations; }

    /// 视图自己的状态：切走再切回来必须还在（相机/选择/滚动在那个真视图里就是这个位置）。
    int activations = 0;

  private:
    ViewBook* book_ = nullptr;
};

/// 登记一个文档类型 + 一个只会造这一种文档的打开器（refine 按载荷里的 id 认领）。
bool registerType(vn::appfw::DocumentManager& documents, const char8_t* type_id)
{
    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id = type_id;
    if (!documents.registerType(registration)) {
        return false;
    }

    return documents.registerOpener<TestPayload>({
        .type_id = type_id,
        .open    = [type_id](const TestPayload&) -> vn::appfw::Document* { return new TestDocument(type_id); },
        .refine  = [type_id](const TestPayload& payload) { return payload.type_id == vn::String(type_id) ? 1 : 0; },
    });
}

/// 打开一份文档（类型由载荷里的 id 决定）。
vn::appfw::Document* openDocument(vn::appfw::DocumentManager& documents, const char8_t* type_id)
{
    TestPayload payload;
    payload.type_id = type_id;
    return documents.open(&payload);
}

/// 登记一个只造“拒绝关闭”文档的类型（浮动窗口的关闭路径用它）。
bool registerVetoType(vn::appfw::DocumentManager& documents, const char8_t* type_id)
{
    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id = type_id;
    if (!documents.registerType(registration)) {
        return false;
    }

    return documents.registerOpener<TestPayload>({
        .type_id = type_id,
        .open    = [type_id](const TestPayload&) -> vn::appfw::Document* { return new VetoDocument(type_id); },
        .refine  = [type_id](const TestPayload& payload) { return payload.type_id == vn::String(type_id) ? 1 : 0; },
    });
}

/// 当前那份视图，取不回 TestView 时是空（工厂只造 TestView，所以这里是在断言这件事）。
TestView* shownView(vn::appfw::gui::CentralDocumentHost& host)
{
    return dynamic_cast<TestView*>(host.currentView());
}

/// 宿主自己的 tab 条（宿主根控件是容器的子控件，不是公开 API：用例直接看它）。
QTabBar* tabStrip(vn::appfw::gui::CentralDocumentHost& host)
{
    auto* root = host.impl<QWidget>();
    return root != nullptr ? root->findChild<QTabBar*>() : nullptr;
}

/// 说明页上那句话（把页里的所有 QLabel 拼起来），没有说明页时返回空。
QString messageOnPage(vn::appfw::gui::UIElement* page)
{
    if (page == nullptr) {
        return {};
    }
    auto* widget = page->impl<QWidget>();
    if (widget == nullptr) {
        return {};
    }

    QString text;
    for (QLabel* label : widget->findChildren<QLabel*>()) {
        text += label->text();
    }
    return text;
}

/// 两窗格的用例视图：一块区里两块 3D 区，**哪一块算数由视图自己回答**（用户最后在那块里干活）。
///
/// 框架不建窗格注册表、不做焦点路由（appfw-render-view.md §6.4）：视图想切哪块就改自己这个指针，命令每次派发
/// 读到的就是它 —— 这也是“多窗格时框架不用改”这句话的可执行版本。
class TwoPaneView final : public vn::appfw::gui::DocumentView {
  public:
    TwoPaneView(vn::appfw::Document& document, ViewBook& book)
      : DocumentView(document, new QWidget())
      , book_(&book)
    {
        ++book_->created;
        panes_[0] = new vn::appfw::gui::RenderControl();
        panes_[1] = new vn::appfw::gui::RenderControl();

        auto* layout = new QVBoxLayout(impl<QWidget>());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(panes_[0]->impl<QWidget>());
        layout->addWidget(panes_[1]->impl<QWidget>());
    }

    ~TwoPaneView() override { ++book_->destroyed; }

    vn::appfw::gui::UIElement* content() override { return this; }

    /// @return 用户最后在里面干活的那一块（框架只搬家，不猜）。
    vn::appfw::gui::RenderControl* renderControl() const override { return panes_[active_]; }

    /// 用户在这块里干活（真视图会在获得焦点/被点击时这么做）。
    ///
    /// @param index 0 或 1。
    void usePane(int index) { active_ = index; }

    /// @param index 0 或 1。
    /// @return 那一块的控件。
    vn::appfw::gui::RenderControl* pane(int index) const { return panes_[index]; }

  private:
    ViewBook*                      book_ = nullptr;
    vn::appfw::gui::RenderControl* panes_[2]{ nullptr, nullptr };
    int                            active_ = 0;
};

} // namespace

TEST(DocumentViewTest, ATypeHasOneViewAndAnEmptyOrNullRegistrationIsRefused)
{
    vn::appfw::gui::DocumentViewRegistry views;

    EXPECT_FALSE(views.hasView(kShownType));
    EXPECT_TRUE(views.types().empty());

    ViewBook book;
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));
    EXPECT_TRUE(views.hasView(kShownType));
    ASSERT_EQ(views.types().size(), 1u);
    EXPECT_EQ(views.types()[0], kShownType);

    // 一类型一份视图：第二次登记被拒（不悄悄换掉已经造出来的那些视图的来源）。
    EXPECT_FALSE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));
    EXPECT_FALSE(views.registerView(vn::String(), [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    // 没登记的类型：create() 返回空（不猜、不兜底）。
    TestDocument blind(kBlindType);
    EXPECT_EQ(views.create(blind), nullptr);
}

TEST(DocumentViewTest, TheGuiApplicationIsWhereTheRegistryAndTheHostComeFrom)
{
    // 视图层是 GUI 宿主的东西：这个应用自己就该有注册表和宿主（在 GuiApplication 建主窗时建好），
    // 插件只需要往注册表里登记。无头 Application 两样都没有 —— 那是另一条用例的事（它连视图都没有）。
    auto* app = dynamic_cast<vn::appfw::gui::GuiApplication*>(vn::appfw::Application::current());
    ASSERT_NE(app, nullptr) << "这条用例要在 GUI 应用里跑";

    EXPECT_NE(app->viewRegistry(), nullptr) << "GUI 宿主必须自带视图注册表";
    EXPECT_NE(app->centralDocumentHost(), nullptr) << "GUI 宿主必须自带中央区宿主";
    EXPECT_NE(app->mainWindow(), nullptr);
}

TEST(DocumentViewTest, TheHostLeavesTheCentralContentAloneUntilADocumentIsShown)
{
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    // 外壳自己往中央区放的东西（真应用里就是那块 3D 演示视图）。
    vn::appfw::gui::Control shell_content(new QWidget());
    docks->setCentralWidget(&shell_content);
    ASSERT_EQ(docks->centralWidget(), &shell_content);

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    EXPECT_FALSE(host.ownsCentralArea()) << "没有文档可显示之前不该接管中央区";
    EXPECT_EQ(docks->centralWidget(), &shell_content) << "外壳的内容还在";
    EXPECT_EQ(host.shownContent(), nullptr);

    // 没有当前文档：同样不动外壳的东西。
    host.syncToCurrentDocument();
    EXPECT_FALSE(host.ownsCentralArea());
    EXPECT_EQ(docks->centralWidget(), &shell_content);
    EXPECT_EQ(host.shownContent(), nullptr);
}

TEST(DocumentViewTest, ViewsAreBuiltPerDocumentInstanceAndKeptUntilTheDocumentCloses)
{
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_EQ(book.created, 0) << "没显示过的文档不该有视图";

    // 第一份成为当前：这时才建视图，而且接管中央区。
    ASSERT_TRUE(documents.setCurrent(first));
    EXPECT_EQ(book.created, 1);
    ASSERT_NE(shownView(host), nullptr);
    EXPECT_EQ(&shownView(host)->document(), first);
    EXPECT_EQ(shownView(host)->activations, 1);
    EXPECT_TRUE(host.ownsCentralArea());
    EXPECT_EQ(host.shownContent(), host.currentView()->content());

    // 同类型的另一份：**另一份**视图（一实例一份），不是复用。
    ASSERT_TRUE(documents.setCurrent(second));
    EXPECT_EQ(book.created, 2) << "同类型的另一份要有自己的视图";
    ASSERT_NE(shownView(host), nullptr);
    EXPECT_EQ(&shownView(host)->document(), second);

    // 切回第一份：复用那一个实例，它的状态还在。
    ASSERT_TRUE(documents.setCurrent(first));
    EXPECT_EQ(book.created, 2) << "切回来是复用，不重建";
    ASSERT_NE(shownView(host), nullptr);
    EXPECT_EQ(&shownView(host)->document(), first);
    EXPECT_EQ(shownView(host)->activations, 2) << "同一个实例被再次激活（它的相机/选择就是这里的 activations）";

    // 关掉非当前那一份：它的视图跟着走，当前那份不动。
    ASSERT_TRUE(documents.close(second));
    EXPECT_EQ(book.destroyed, 1);
    ASSERT_NE(shownView(host), nullptr);
    EXPECT_EQ(&shownView(host)->document(), first);

    // 关掉当前那一份：说明页（管理器先发 currentChanged(nullptr)），视图也销毁了。
    ASSERT_TRUE(documents.close(first));
    EXPECT_EQ(book.destroyed, 2);
    EXPECT_EQ(host.currentView(), nullptr);
    EXPECT_NE(host.shownContent(), nullptr) << "中央区已经是我们管了：要有说明页，不是空白";
    EXPECT_TRUE(messageOnPage(host.shownContent()).contains(QStringLiteral("没有当前文档")));
}

TEST(DocumentViewTest, ATypeWithoutAViewGetsAPageThatNamesIt)
{
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kBlindType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* blind = openDocument(documents, kBlindType);
    ASSERT_NE(blind, nullptr);
    ASSERT_TRUE(documents.setCurrent(blind));

    EXPECT_EQ(book.created, 0) << "没登记视图的类型：不建视图，也不借别的类型的视图";
    EXPECT_EQ(host.currentView(), nullptr);
    EXPECT_TRUE(host.ownsCentralArea()) << "有文档要显示（只是没有视图）⇒ 该接管来说清楚";

    const QString message = messageOnPage(host.shownContent());
    EXPECT_TRUE(message.contains(QStringLiteral("view_test_blind"))) << "说明页要点出类型 id，实际是: "
                                                                     << message.toStdString();
    EXPECT_TRUE(message.contains(QStringLiteral("没有注册视图")));
}

TEST(DocumentViewTest, NoActiveDocumentMeansNoCommandTarget)
{
    // 窗口的 3D 区是**派生**的（当前文档视图 → 它的活动窗格），不是谁发布进窗口的槽：没有当前文档就没有可回答的
    // 东西 —— "没有目标"是正常状态，不是失败。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    EXPECT_TRUE(vn::appfw::gui::resolveCommandTarget(&window).isEmpty()) << "没有当前文档：没有目标";

    // 文档活跃，但它的视图是 2D（不回答 3D 区）⇒ 仍然没有目标：**文档活跃 ≠ 有 3D 目标**。
    vn::appfw::Document* document = openDocument(documents, kShownType);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(documents.setCurrent(document));
    ASSERT_NE(host.currentView(), nullptr) << "视图在屏幕上";

    // 派发解析给出的是**只有文档**的目标：文档层有了，3D 区那层没有 —— 于是 NeedsView 的命令会得到统一拒绝，
    // 而 NeedsDocument 的命令照常跑（2D 文档不是二等公民）。
    const auto target = vn::appfw::gui::resolveCommandTarget(&window);
    EXPECT_EQ(target.document, document);
    EXPECT_EQ(target.view, nullptr) << "2D 视图没有可作用的 3D 区";

    // 文档不再活跃 ⇒ 回到"没有目标"。视图对象还在（视图不随激活消失），只是没人问它。
    ASSERT_TRUE(documents.setCurrent(nullptr));
    EXPECT_EQ(host.currentView(), nullptr);
    EXPECT_EQ(book.destroyed, 0) << "下屏不是销毁";
    EXPECT_TRUE(vn::appfw::gui::resolveCommandTarget(&window).isEmpty());
}

TEST(DocumentViewTest, AViewWithTwoPanesAnswersForTheOneTheUserWorkedInLast)
{
    // ADR-2026-10-24 的最后一条：目标带的是**视图**，不是某一块控件 —— 视图自己有多个窗格时，哪一块算数由它
    // 回答。所以“一视图两窗格”不需要框架加注册表、加焦点路由，也不需要命令认识窗格：它问目标，目标问视图。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TwoPaneView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    vn::appfw::Document*                 document = openDocument(documents, kShownType);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(documents.setCurrent(document));

    auto* view = dynamic_cast<TwoPaneView*>(host.currentView());
    ASSERT_NE(view, nullptr) << "中央区里应该是那个两窗格视图";
    ASSERT_NE(view->pane(0), view->pane(1)) << "两块真的不是同一块";

    // 派发的目标就是**这个视图**：两块都在它里面，框架不替它选。
    const auto first_target = vn::appfw::gui::resolveCommandTarget(&window);
    EXPECT_EQ(first_target.document, document);
    EXPECT_EQ(vn::appfw::gui::targetView(first_target), view);
    EXPECT_EQ(vn::appfw::gui::targetView(first_target)->renderControl(), view->pane(0))
        << "命令从目标拿到的就是视图当前那一块";

    // 用户换到第二块：视图改自己的指针，下一次派发的目标随之改答 —— 框架代码一行没动。
    view->usePane(1);
    const auto second_target = vn::appfw::gui::resolveCommandTarget(&window);
    EXPECT_EQ(vn::appfw::gui::targetView(second_target), view) << "仍然是同一个视图";
    EXPECT_EQ(vn::appfw::gui::targetView(second_target)->renderControl(), view->pane(1))
        << "另一块窗格不会让命令改错东西";
}

// ============ 多文档：tab 与浮动 ============

TEST(DocumentViewTest, EveryOpenDocumentGetsATabAndATabSelectionSelectsTheDocument)
{
    // tab 是“打开集合”的呈现，不是“视图缓存”的呈现：打开的每一份都有自己的格子，而视图仍然懒建。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    // 管理器 → 界面这条路的观察点：用户换文档必须真的产生一次 currentChanged，而不是 UI 自己改了个选中项。
    int                  changes      = 0;
    vn::appfw::Document* last_changed = nullptr;
    const vn::Connection recording    = documents.currentChanged.connect(
        [&changes, &last_changed](vn::appfw::DocumentManager&, vn::appfw::DocumentEventArgs& args) {
            ++changes;
            last_changed = args.document();
        });

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    vn::appfw::Document* third  = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(third, nullptr);

    EXPECT_EQ(host.documentCount(), 3) << "打开的每一份都有 tab";
    EXPECT_EQ(host.documentAt(0), first);
    EXPECT_EQ(host.documentAt(1), second);
    EXPECT_EQ(host.documentAt(2), third);
    EXPECT_EQ(host.documentAt(3), nullptr) << "越界不含糊";
    EXPECT_EQ(book.created, 0) << "tab 不是视图：没显示过就没有视图";

    auto* strip = tabStrip(host);
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->count(), 3);
    EXPECT_FALSE(strip->isHidden()) << "多于一份文档时 tab 条是显示的";

    // 管理器 → tab 条：选中哪份，格子就选哪个。
    ASSERT_TRUE(documents.setCurrent(second));
    EXPECT_EQ(strip->currentIndex(), 1);
    EXPECT_EQ(changes, 1) << "API 选中文档 ⇒ 正好一次 currentChanged";
    EXPECT_EQ(last_changed, second);

    // tab 条 → 管理器：用户点格子就是“选中这份文档”（管理器仍是唯一的所有者），而且真的发事件。
    strip->setCurrentIndex(2);
    EXPECT_EQ(documents.current(), third);
    EXPECT_EQ(host.documentAt(strip->currentIndex()), third);
    EXPECT_EQ(changes, 2) << "鼠标点 tab 也必须产生 currentChanged";
    EXPECT_EQ(last_changed, third);

    // 关一份 ⇒ 它那一格消失，其余顺序不变。
    ASSERT_TRUE(documents.close(second));
    EXPECT_EQ(host.documentCount(), 2);
    EXPECT_EQ(host.documentAt(0), first);
    EXPECT_EQ(host.documentAt(1), third);
    EXPECT_EQ(strip->count(), 2);
    EXPECT_EQ(documents.current(), third) << "关掉的不是当前那份：当前文档不动";
    EXPECT_EQ(strip->currentIndex(), 1) << "格子前移后，选中项仍指向当前那份";
}

TEST(DocumentViewTest, ClosingTheCurrentDocumentSelectsTheTabThatTakesItsPlaceAndTheStripDisappearsAtOne)
{
    // 关掉当前文档时管理器先发 currentChanged(nullptr)、再发 closed（§3.1.1 N1），这次关闭本身没有留下当前文档。
    // 但 tab 条表达不了“什么都没选中”（Qt 6.10 实测：有 tab 时 setCurrentIndex(-1) 是空操作），所以宿主在关闭后向
    // 管理器**请求**选中接替那一格：仍是请求，管理器仍是唯一所有者（R7），否则就会出现“条上选着某一份，中央区却说
    // 没有当前文档”（N8）。另外只剩一份时条要收起来——和“打开第一份时不显示”必须是同一条规则，否则打开与关闭不对称。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    auto* strip = tabStrip(host);
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->count(), 2);
    EXPECT_FALSE(strip->isHidden()) << "两份文档时 tab 条显示";

    ASSERT_TRUE(documents.setCurrent(second));
    EXPECT_EQ(strip->currentIndex(), 1);

    // 关掉当前那份：接替它的那一格成为当前文档，并且真的被显示出来。
    ASSERT_TRUE(documents.close(second));
    EXPECT_EQ(documents.current(), first) << "关掉当前那份之后要有一份接替，否则条会指着一份没有显示的文档";
    EXPECT_EQ(strip->currentIndex(), 0) << "条上选中的就是当前文档";
    ASSERT_NE(host.currentView(), nullptr);
    EXPECT_EQ(&host.currentView()->document(), first) << "中央区显示的就是条上选中的那份";
    EXPECT_EQ(host.shownContent(), host.currentView()->content());
    EXPECT_EQ(book.destroyed, 1) << "关掉的那份视图销毁了，接替的那份还在";

    // 只剩一份：条跟着消失，和“打开第一份时不显示”对称。
    EXPECT_EQ(strip->count(), 1);
    EXPECT_TRUE(strip->isHidden()) << "只剩一份文档时 tab 条必须像打开第一份时那样不显示";

    // 再来一份：条回来，选中项仍指向当前那份（新开的不自动成为当前）。
    vn::appfw::Document* third = openDocument(documents, kShownType);
    ASSERT_NE(third, nullptr);
    EXPECT_FALSE(strip->isHidden()) << "第二份打开后条回来";
    EXPECT_EQ(documents.current(), first) << "新开一份不改变当前文档";
    EXPECT_EQ(strip->currentIndex(), 0);
    EXPECT_EQ(host.documentAt(strip->currentIndex()), first);
}

TEST(DocumentViewTest, SwitchingTabsReusesEachViewAndNeverReinstallsTheCentralArea)
{
    // 契约 1 与契约 3 在多文档下仍然成立：切 tab 只切页，不重建视图、不重父化宿主。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    ASSERT_TRUE(documents.setCurrent(first));
    ASSERT_EQ(book.created, 1);

    TestView* first_view = shownView(host);
    ASSERT_NE(first_view, nullptr);
    auto* first_page = first_view->content()->impl<QWidget>();
    ASSERT_NE(first_page, nullptr);

    QWidget* const central_window = first_page->window();
    ASSERT_NE(central_window, nullptr);

    ASSERT_TRUE(documents.setCurrent(second));
    EXPECT_EQ(book.created, 2) << "第二份自己一份视图";

    ASSERT_TRUE(documents.setCurrent(first));
    EXPECT_EQ(book.created, 2) << "切回来是复用，不是重建";
    EXPECT_EQ(shownView(host), first_view);
    EXPECT_EQ(first_page->window(), central_window) << "切 tab 不重父化 ⇒ 原生窗口不重建";
    EXPECT_EQ(docks->centralWidget(), &host) << "中央槽只装一次";
    EXPECT_TRUE(host.ownsCentralArea());
}

TEST(DocumentViewTest, ADocumentFloatsIntoItsOwnWindowAndDocksBackIntoItsTab)
{
    // 浮动是“搬到一个自己的顶层窗口”，不是“停靠到某个停靠区”：页控件换窗口，文档与视图都不变。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* document = openDocument(documents, kShownType);
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(documents.setCurrent(document));

    TestView* view = shownView(host);
    ASSERT_NE(view, nullptr);
    auto* page = view->content()->impl<QWidget>();
    ASSERT_NE(page, nullptr);

    QWidget* const central_window = page->window();
    ASSERT_NE(central_window, nullptr);

    host.setFloating(*document, true);

    EXPECT_TRUE(host.isFloating(*document));
    EXPECT_EQ(host.documentCount(), 1) << "浮动不改变 tab 集合";
    EXPECT_EQ(book.created, 1) << "浮动不换视图";
    EXPECT_EQ(host.currentView(), view) << "当前文档还是它，视图还是同一个";
    ASSERT_NE(page->window(), nullptr);
    EXPECT_NE(page->window(), central_window) << "页控件搬到了自己的顶层窗口";
    EXPECT_NE(host.shownContent(), view->content()) << "页搬走了，中央区不再显示它";
    EXPECT_TRUE(messageOnPage(host.shownContent()).contains(QStringLiteral("独立窗口")))
        << "中央区说明当前文档在自己的窗口里，而不是显示别的文档";

    // 停回来：页控件回到中央区那个窗口，视图还是同一个。
    host.setFloating(*document, false);

    EXPECT_FALSE(host.isFloating(*document));
    EXPECT_EQ(page->window(), central_window);
    EXPECT_EQ(host.currentView(), view);
    EXPECT_EQ(host.shownContent(), view->content()) << "停回来，中央区重新显示它的页";
    EXPECT_EQ(book.destroyed, 0) << "浮动/停回都不是销毁";
}

TEST(DocumentViewTest, TheCentralAreaOnlyEverShowsTheCurrentDocument)
{
    // 一个不变量：中央区显示的**永远**是当前文档的页，或说明页；绝不显示另一份文档的页。
    // 这正是 tab 选中、current 与中央内容在任意操作后都一致的原因。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    ASSERT_TRUE(documents.setCurrent(first));
    auto* first_view = shownView(host);
    ASSERT_NE(first_view, nullptr);
    auto* first_page = first_view->content()->impl<QWidget>();
    ASSERT_NE(first_page, nullptr);
    EXPECT_EQ(host.shownContent(), first_view->content());

    // 非当前那份浮出去：中央区不动，还是当前那份的页。
    host.setFloating(*second, true);
    ASSERT_TRUE(host.isFloating(*second));
    EXPECT_EQ(host.shownContent(), first_view->content()) << "浮一个非当前文档不改变中央区";
    EXPECT_EQ(shownView(host), first_view);

    // 当前那份浮出去：页搬走了，中央区不能再声称它还在。
    host.setFloating(*first, true);
    ASSERT_TRUE(host.isFloating(*first));
    EXPECT_EQ(host.currentView(), first_view) << "当前视图没变：它只是换了窗口";
    EXPECT_NE(host.shownContent(), first_view->content());
    EXPECT_TRUE(messageOnPage(host.shownContent()).contains(QStringLiteral("独立窗口")));

    // 停回第一份、把当前切到仍在浮动的第二份：中央区说“在独立窗口”，不是第一份的页。
    host.setFloating(*first, false);
    ASSERT_TRUE(documents.setCurrent(second));
    EXPECT_NE(host.shownContent(), first_view->content()) << "当前文档是浮动的第二份，不是第一份";
    EXPECT_TRUE(messageOnPage(host.shownContent()).contains(QStringLiteral("独立窗口")));

    // 第二份停回来：中央区是它的页。
    auto* second_view = host.viewFor(*second);
    ASSERT_NE(second_view, nullptr);
    host.setFloating(*second, false);
    EXPECT_EQ(host.shownContent(), second_view->content());
}

TEST(DocumentViewTest, ActivatingAFloatingDocumentsWindowSelectsIt)
{
    // 焦点跟随文档：几个文档各有窗口时，被激活的那个就是当前文档，tab 也跟着选它。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    ASSERT_TRUE(documents.setCurrent(first));
    auto* first_view = shownView(host);
    ASSERT_NE(first_view, nullptr);

    host.setFloating(*first, true);
    ASSERT_TRUE(host.isFloating(*first));

    // 当前换成另一份（标签条选第二个格子）。
    ASSERT_TRUE(documents.setCurrent(second));
    auto* strip = tabStrip(host);
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->currentIndex(), 1);

    // 用户去点第一个文档的窗口：激活即“我在做这一份”。
    QWidget* const page = first_view->content()->impl<QWidget>();
    ASSERT_NE(page, nullptr);
    ASSERT_NE(page->window(), nullptr);
    page->window()->activateWindow();
    QCoreApplication::processEvents();

    if (!page->window()->isActiveWindow()) {
        GTEST_SKIP() << "this platform does not activate widget windows (offscreen)";
    }

    EXPECT_EQ(documents.current(), first) << "哪个文档窗口被激活，哪个就是当前文档";
    EXPECT_EQ(strip->currentIndex(), 0) << "tab 自动跟到当前文档";
}

TEST(DocumentViewTest, ClosingAFloatedDocumentsWindowGoesThroughTheManagerSoAVetoKeepsIt)
{
    // 浮动窗口的 X 不是“删窗口”，而是“请求关闭文档”：canClose() 否决时窗口和文档都还在（否则关一半），
    // 不否决时文档真的关掉、窗口跟着消失。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    constexpr const char8_t* kVetoType = u8"view_test_veto";

    ASSERT_TRUE(registerVetoType(documents, kVetoType));
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kVetoType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* veto   = openDocument(documents, kVetoType);
    vn::appfw::Document* normal = openDocument(documents, kShownType);
    ASSERT_NE(veto, nullptr);
    ASSERT_NE(normal, nullptr);

    // 否决：X 之后文档与窗口都还在。
    ASSERT_TRUE(documents.setCurrent(veto));
    host.setFloating(*veto, true);
    ASSERT_TRUE(host.isFloating(*veto));

    auto* veto_page = shownView(host)->content()->impl<QWidget>();
    ASSERT_NE(veto_page, nullptr);
    ASSERT_NE(veto_page->window(), nullptr);

    veto_page->window()->close();
    QCoreApplication::processEvents(); // 关闭是排到下一轮事件循环的（窗口不能在 closeEvent 里被删）

    EXPECT_TRUE(host.isFloating(*veto)) << "canClose() 否决 ⇒ 窗口还在";
    EXPECT_EQ(documents.current(), veto) << "文档也还在、仍是当前";
    EXPECT_EQ(host.documentCount(), 2);
    EXPECT_EQ(book.destroyed, 0);

    // 不否决：X 真的关掉文档，窗口随之销毁。
    ASSERT_TRUE(documents.setCurrent(normal));
    host.setFloating(*normal, true);
    ASSERT_TRUE(host.isFloating(*normal));

    auto* normal_page = shownView(host)->content()->impl<QWidget>();
    ASSERT_NE(normal_page, nullptr);
    ASSERT_NE(normal_page->window(), nullptr);

    normal_page->window()->close();
    QCoreApplication::processEvents();

    EXPECT_FALSE(host.isFloating(*normal)) << "关掉的文档不该还有浮动窗口";
    EXPECT_EQ(host.documentCount(), 1);
    EXPECT_EQ(book.destroyed, 1);
}

TEST(DocumentViewTest, ClosingADocumentThroughTheApiAlsoClosesItsTabAndFloatingWindow)
{
    // 关闭不只发生在“点 tab / 点窗口的 X”上：DocumentManager::close() 是 API，界面必须同样收干净 —— tab、页控件、
    // 浮动窗口都不留。否则 API 关闭之后，屏幕上还挂着一份已经不存在的文档。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ViewBook book;
    ASSERT_TRUE(registerType(documents, kShownType));
    ASSERT_TRUE(views.registerView(kShownType, [&book](vn::appfw::Document& document) {
        return new TestView(document, book);
    }));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::appfw::Document* first  = openDocument(documents, kShownType);
    vn::appfw::Document* second = openDocument(documents, kShownType);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    ASSERT_TRUE(documents.setCurrent(first));
    host.setFloating(*first, true);
    ASSERT_TRUE(host.isFloating(*first));

    auto* first_page = shownView(host)->content()->impl<QWidget>();
    ASSERT_NE(first_page, nullptr);
    QPointer<QWidget> floating_window = first_page->window();
    ASSERT_FALSE(floating_window.isNull());

    // API 关闭当前（浮动）文档：窗口销毁、tab 消失、邻格接替。
    ASSERT_TRUE(documents.close(first));
    EXPECT_TRUE(floating_window.isNull()) << "API 关闭后浮动窗口必须被销毁";
    EXPECT_EQ(host.documentCount(), 1);
    EXPECT_EQ(documents.current(), second) << "接替的是另一份";
    ASSERT_NE(host.shownContent(), nullptr);

    auto* strip = tabStrip(host);
    ASSERT_NE(strip, nullptr);
    EXPECT_EQ(strip->count(), 1);
    EXPECT_TRUE(strip->isHidden()) << "只剩一份时条隐藏";

    // 再 API 关闭最后一份：条清空、中央区是说明页（已经接管过，不再交还）。
    ASSERT_TRUE(documents.close(second));
    EXPECT_EQ(host.documentCount(), 0);
    EXPECT_EQ(strip->count(), 0);
    EXPECT_EQ(host.currentView(), nullptr);
    ASSERT_NE(host.shownContent(), nullptr);
    EXPECT_TRUE(messageOnPage(host.shownContent()).contains(QStringLiteral("没有当前文档")));
    EXPECT_EQ(book.destroyed, 2) << "两份视图都随文档销毁";
}
