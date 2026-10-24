// 视图层：注册表（DocumentViewRegistry）+ 中央区宿主（CentralDocumentHost）。
//
// 这条用例钉住宿主那五条契约里最容易被写坏的四条：
//   ① 没东西要显示之前**不接管**中央区（外壳自己放在那里的东西还在）；
//   ② 视图**按文档实例**懒建：没显示过的文档没有视图；同一份切回来复用同一个实例，它的状态还在；
//   ③ 文档关闭 ⇒ 它的视图被销毁（不攒着）；
//   ④ 没登记视图的类型 ⇒ 说明页（点出类型 id），不是空白、也不是别的类型的视图。
//
// 用例自带文档管理器 / 注册表 / 窗口，不碰进程里那个真应用，所以彼此与别的用例互不影响。

#include <gtest/gtest.h>

#include <QLabel>
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
class TestDocument final : public vn::appfw::Document {
  public:
    explicit TestDocument(vn::String type_id) : type_id_(std::move(type_id)) {}

    vn::String typeId() const override { return type_id_; }
    vn::String title() const override { return u8"用例文档"; }
    bool       isDirty() const override { return false; }

  private:
    vn::String type_id_;
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

/// 当前那份视图，取不回 TestView 时是空（工厂只造 TestView，所以这里是在断言这件事）。
TestView* shownView(vn::appfw::gui::CentralDocumentHost& host)
{
    return dynamic_cast<TestView*>(host.currentView());
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
