// 模型查看器（测试插件）的**模型信息面板**：顶点数与三角形数要显示对，而且要跟着当前文档走。
//
// 这条用例不加载插件：面板是 header-only 的（没有元数据、没有信号槽），用例直接 include 它、自己接上
// DocumentManager —— 和插件 load() 里做的是同一件事（followDocumentManager），所以面板的"跟当前文档走"
// 这一段是被真跑到的，而不是靠人去点。

#include <gtest/gtest.h>

#include <QLabel>
#include <QWidget>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>

#include "ModelDocument.hpp"
#include "ModelInfoPanel.hpp"
#include "ModelRenderView.hpp"
#include "TestModelData.hpp"

namespace
{

/// 用例自己的载荷：内容和插件里那个一样，但要在这里有元数据（一处 IMPL 就够）。
struct TestMeshPayload : public vn::Object {
    VN_OBJECT_META_DECL;

    vn::String                                                 name;
    vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh> mesh;
};

VN_OBJECT_META_IMPL(TestMeshPayload, vn::Object)

/// 一个**不是模型**的文档：面板该照样报它的类型与标题，两个数则说"不是模型文档"。
class OtherDocument : public vn::appfw::Document {
  public:
    vn::String typeId() const override { return u8"other"; }
    vn::String title() const override { return u8"别的文档"; }
    bool       isDirty() const override { return false; }
};

constexpr const char8_t* kModelType = vn::model_viewer::ModelDocument::kTypeId;

bool registerModelType(vn::appfw::DocumentManager& documents)
{
    vn::appfw::DocumentTypeRegistration registration;
    registration.type_id      = kModelType;
    registration.display_name = u8"模型";
    return documents.registerType(registration);
}

/// 同一个打开器（插件里那两个的简化版：载荷直接带网格）。
bool registerModelOpener(vn::appfw::DocumentManager& documents)
{
    return documents.registerOpener<TestMeshPayload>({
        .type_id = kModelType,
        .open    = [](const TestMeshPayload& payload) -> vn::appfw::Document* {
            return new vn::model_viewer::ModelDocument(payload.name, payload.mesh);
        },
    });
}

/// 开一份模型文档并把它设为当前（"打开就显示这一份"是宿主的策略，用例里这样模拟）。
vn::appfw::Document* openModel(vn::appfw::DocumentManager& documents, vn::String name,
                               vn::intrusive_ptr<const vn::geometry::IndexedTriangleMesh> mesh)
{
    TestMeshPayload payload;
    payload.name = std::move(name);
    payload.mesh = std::move(mesh);

    vn::appfw::Document* document = documents.open(&payload);
    if (document != nullptr) {
        documents.setCurrent(document);
    }
    return document;
}

} // namespace

TEST(ModelViewerTest, TheInfoPanelIsEmptyWhileThereIsNoCurrentDocument)
{
    vn::appfw::DocumentManager documents;
    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));

    vn::model_viewer::ModelInfoPanel panel;
    panel.followDocumentManager(documents);

    EXPECT_EQ(panel.typeText(), QStringLiteral("—（没有当前文档）"));
    EXPECT_EQ(panel.titleText(), QStringLiteral("—（没有当前文档）"));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("—（没有当前文档）"));
    EXPECT_EQ(panel.trianglesText(), QStringLiteral("—（没有当前文档）"));

    // 打开一份但**不设为当前**：面板不动（它跟的是"当前"，不是"有哪些"）。
    ASSERT_NE(openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh()), nullptr);
    documents.setCurrent(nullptr);
    EXPECT_EQ(panel.verticesText(), QStringLiteral("—（没有当前文档）")) << "打开不等于当前";
}

TEST(ModelViewerTest, TheInfoPanelShowsTheVertexAndTriangleCountsOfTheCurrentDocument)
{
    vn::appfw::DocumentManager documents;
    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));

    vn::model_viewer::ModelInfoPanel panel;
    panel.followDocumentManager(documents);

    // 立方体：8 个顶点 / 12 个三角形（makeBoxMesh 里数得出来）。
    vn::appfw::Document* box = openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh());
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(panel.typeText(), QStringLiteral("model"));
    EXPECT_EQ(panel.titleText(), QStringLiteral("立方体"));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("8"));
    EXPECT_EQ(panel.trianglesText(), QStringLiteral("12"));

    // 网格 2×2：顶点 (2+1)² = 9，三角形 2·2² = 8 —— 换一份当前文档，两个数跟着换。
    vn::appfw::Document* grid = openModel(documents, u8"网格", vn::model_viewer::makeGridMesh(2));
    ASSERT_NE(grid, nullptr);
    ASSERT_NE(grid, box);
    EXPECT_EQ(panel.titleText(), QStringLiteral("网格"));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("9"));
    EXPECT_EQ(panel.trianglesText(), QStringLiteral("8"));

    // 切回去：面板显示的是**当前**那一份，不是最后打开的那一份。
    ASSERT_TRUE(documents.setCurrent(box));
    EXPECT_EQ(panel.titleText(), QStringLiteral("立方体"));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("8"));
    EXPECT_EQ(panel.trianglesText(), QStringLiteral("12"));

    // 关掉当前文档 ⇒ 管理器先发 currentChanged(nullptr)，面板回到空状态。
    ASSERT_TRUE(documents.close(box));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("—（没有当前文档）"));
    EXPECT_EQ(documents.current(), nullptr);
}

TEST(ModelViewerTest, TheInfoPanelSaysSoWhenTheCurrentDocumentIsNotAModel)
{
    vn::appfw::DocumentManager documents;
    ASSERT_TRUE(registerModelType(documents));

    vn::appfw::DocumentTypeRegistration other;
    other.type_id = u8"other";
    other.create  = [] { return new OtherDocument(); };
    ASSERT_TRUE(documents.registerType(other));

    vn::model_viewer::ModelInfoPanel panel;
    panel.followDocumentManager(documents);

    vn::appfw::Document* stranger = documents.create(u8"other");
    ASSERT_NE(stranger, nullptr);
    ASSERT_TRUE(documents.setCurrent(stranger));

    // 类型与标题照样显示；两个数说清楚"不是模型文档"，而不是假装 0。
    EXPECT_EQ(panel.typeText(), QStringLiteral("other"));
    EXPECT_EQ(panel.titleText(), QStringLiteral("别的文档"));
    EXPECT_EQ(panel.verticesText(), QStringLiteral("—（不是模型文档）"));
    EXPECT_EQ(panel.trianglesText(), QStringLiteral("—（不是模型文档）"));
}

TEST(ModelViewerTest, EachDocumentViewOwnsItsOwnRenderSurface)
{
    // 视图层那三段（注册表 / 宿主 / 中央容器）在 DocumentViewTest 里各自被钉过；这条钉的是**插件自己**那一段，
    // 以及"一个文档 = 一个视图 = 它自己那份资源"：视图自带渲染控件（自己的面/会话/设备），两份文档的控件与引擎
    // **都不是同一个**；宿主看到的是一实例一页（`content()` 就是视图自己）。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));
    ASSERT_TRUE(views.registerView(kModelType, vn::model_viewer::createRenderView));

    // 外壳自己放的东西（真应用里是那块 3D 演示视图）：宿主一显示文档就该把它换下去。
    vn::appfw::gui::Control shell_content(new QWidget());
    docks->setCentralWidget(&shell_content);

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    EXPECT_FALSE(host.ownsCentralArea()) << "没有当前文档之前不接管中央区";

    vn::appfw::Document* box_document = openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh());
    ASSERT_NE(box_document, nullptr);

    auto* first = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(first, nullptr) << "中央区里应该是插件登记的那个渲染视图";
    EXPECT_EQ(first->content(), first) << "视图交出去的就是它自己：一实例一页";
    EXPECT_EQ(host.shownContent(), first);
    EXPECT_NE(first->renderControl(), nullptr);
    EXPECT_EQ(first->renderControl()->engine(), first->renderControl()->view()->engine())
        << "自己的控件服务自己的 view";

    // 第二份：另一个视图、另一块渲染面、另一个引擎 —— 资源不共享（共享的只有 DockPanel/Ribbon 那套外壳）。
    ASSERT_NE(openModel(documents, u8"网格", vn::model_viewer::makeGridMesh(2)), nullptr);
    auto* second = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(second, nullptr);
    ASSERT_NE(second, first);
    EXPECT_NE(second->renderControl(), first->renderControl()) << "每份文档一块自己的渲染面";
    EXPECT_NE(second->renderControl()->engine(), first->renderControl()->engine()) << "各自的会话，不共享";
    EXPECT_NE(second->renderControl()->view(), first->renderControl()->view()) << "各自的 view";

    // 关掉一份：只有它那份资源跟着消失，另一份还在（视图按实例缓存）。
    ASSERT_NE(documents.current(), nullptr);
    ASSERT_TRUE(documents.close(documents.current()));
    EXPECT_EQ(host.currentView(), nullptr) << "没有当前文档了";
    EXPECT_EQ(host.viewFor(*box_document), first) << "另一份的视图还带着自己的资源在";
}
