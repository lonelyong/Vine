// 模型查看器（测试插件）的**模型信息面板**：顶点数与三角形数要显示对，而且要跟着当前文档走。
//
// 这条用例不加载插件：面板是 header-only 的（没有元数据、没有信号槽），用例直接 include 它、自己接上
// DocumentManager —— 和插件 load() 里做的是同一件事（followDocumentManager），所以面板的"跟当前文档走"
// 这一段是被真跑到的，而不是靠人去点。
//
// 文件载荷那条路（`MeshFileOpener.hpp`）也是 header-only 的：插件登记的打开器就是那个函数，所以下面那条
// "文件载荷真的走到渲染几何"的用例验的是**插件自己那段代码**，不是它的复述。

#include <gtest/gtest.h>

#include <filesystem>

#include <QLabel>
#include <QWidget>

#include <vine/Object.hpp>
#include <vine/String.hpp>
#include <vine/appfw/Application.hpp>
#include <vine/appfw/CommandManager.hpp>
#include <vine/appfw/Document.hpp>
#include <vine/appfw/DocumentManager.hpp>
#include <vine/appfw/gui/CentralDocumentHost.hpp>
#include <vine/appfw/gui/Control.hpp>
#include <vine/appfw/gui/DocumentView.hpp>
#include <vine/appfw/gui/DocumentViewRegistry.hpp>
#include <vine/appfw/gui/DockPanelManager.hpp>
#include <vine/appfw/gui/GuiApplication.hpp>
#include <vine/appfw/gui/MainWindow.hpp>
#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/graphics/Geometry.hpp>
#include <vine/graphics/Group.hpp>
#include <vine/graphics/Material.hpp>
#include <vine/graphics/RenderPass.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/Scene.hpp>
#include <vine/window/InputEvent.hpp>
#include <vine/window/MouseButton.hpp>
#include <vine/graphics/SceneView.hpp>

#include "MeshFileOpener.hpp"
#include "ModelDocument.hpp"
#include "ModelInfoPanel.hpp"
#include "ModelRenderView.hpp"
#include "TestModelData.hpp"
#include "commands/ShadingModeCommands.hpp"

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

/// 文件那条路的打开器：和插件登记的是**同一个函数**（header-only，见 MeshFileOpener.hpp）。
bool registerMeshFileOpener(vn::appfw::DocumentManager& documents)
{
    return documents.registerOpener<vn::model_viewer::MeshFilePayload>({
        .type_id = kModelType,
        .open    = [](const vn::model_viewer::MeshFilePayload& payload) -> vn::appfw::Document* {
            return vn::model_viewer::openMeshFilePayload(payload);
        },
        .source_scheme = u8"file",
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

/// 几何里存在这一个顶点吗（顺序不保证：加载器会去重顶点、按缓存局部性重排）。
bool hasVertex(const vn::graphics::Geometry& geometry, float x, float y, float z)
{
    const auto* positions = geometry.buffer(0);
    if (positions == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < positions->vertexCount(); ++i) {
        const std::array<float, 3> vertex = positions->xyz(i);
        if (std::abs(vertex[0] - x) < 1.0e-5f && std::abs(vertex[1] - y) < 1.0e-5f && std::abs(vertex[2] - z) < 1.0e-5f) {
            return true;
        }
    }
    return false;
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

TEST(ModelViewerTest, TheSceneCarriesItsOwnLights)
{
    // 灯是**场景**的属性（`Scene::addLight`），而每个视图有自己的场景：没人给这份文档装灯，它就落在"零灯"
    // 那一档 —— 后端在没有环境光时只补一个 0.15 常量填充，网格被画成一片平色，看起来就是"没有光照效果"。
    // 这条钉住"视图自带灯"，以及灯是**世界固定**的方向光（不跟相机走：绕行时明暗只随几何变）。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));
    ASSERT_TRUE(views.registerView(kModelType, vn::model_viewer::createRenderView));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    ASSERT_NE(openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh()), nullptr);

    auto* view = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(view, nullptr);
    view->activate();

    auto* control = view->renderControl();
    ASSERT_NE(control, nullptr);
    auto* scene_view = control->view();
    ASSERT_NE(scene_view, nullptr);
    const auto scene = scene_view->scene();
    ASSERT_NE(scene, nullptr);

    const auto& lights = scene->lights();
    ASSERT_GE(lights.size(), 3u) << "环境光 + 主光 + 补光：少了就只剩平色";

    std::size_t ambient = 0;
    std::size_t directional = 0;
    for (const auto& light : lights) {
        if (light == nullptr || !light->isEnabled()) {
            continue;
        }
        if (light->type() == vn::graphics::LightType::Ambient) {
            ++ambient;
        }
        if (light->type() == vn::graphics::LightType::Directional) {
            ++directional;
            // 方向光必须有真实方向（零向量照不出明暗差别）。
            const auto direction = light->direction();
            EXPECT_GT(std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z), 0.0)
                << "方向光的朝向不能是零向量";
        }
    }
    EXPECT_EQ(ambient, 1u) << "恰好一盏环境光：多了会洗掉明暗";
    EXPECT_GE(directional, 2u) << "主光 + 背面补光";

    // 视图自己的场景：它给的就是控件那个 view 的场景，不是另外造的一个。
    EXPECT_EQ(scene.get(), control->view()->scene().get());
}

TEST(ModelViewerTest, TheSceneCarriesItsOwnFrameRateReadout)
{
    // 帧率读数现在是**控件查看器预设**里的 HUD 档（`control->setViewerOptions({ .stats = true })`，见
    // ModelRenderView::buildScene）：查看器不再自己造 pass、不再自己接尺寸与比例。这条钉四件事：①读数在、且在画；
    // ②它是**查看器那条链**的一部分（不是随手挂在引擎上的孤儿）；③`activate()` 可重复（不重建、不叠 pass）；
    // ④显示/隐藏是零重建的开关。
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));
    ASSERT_TRUE(views.registerView(kModelType, vn::model_viewer::createRenderView));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    ASSERT_NE(openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh()), nullptr); // 宿主显示 ⇒ activate()

    auto* view = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(view, nullptr);
    auto* control = view->renderControl();
    ASSERT_NE(control, nullptr);
    auto* engine = control->engine();
    ASSERT_NE(engine, nullptr);

    auto* scene_view = control->view();
    ASSERT_NE(scene_view, nullptr);
    auto* pipeline = scene_view->pipeline();
    ASSERT_NE(pipeline, nullptr) << "上屏之后查看器该给视图一条链";
    // 预设说的是"这条链有没有读数"（结构决定），读数在不在是链上的 pass 说了算。
    EXPECT_TRUE(control->viewerOptions().stats) << "查看器的预设要带这个 HUD 档";

    // 读数（内容对象）与画它的 pass 是两件事：可见性问 pass（enabled 是声明状态），内容归对象。
    auto* readout = view->fpsOverlay();
    ASSERT_NE(readout, nullptr) << "查看器要让链带上帧率读数";
    EXPECT_EQ(static_cast<const void*>(readout), static_cast<const void*>(pipeline->fpsOverlay()))
        << "读数必须是那条链里的一个，不是另挂在引擎上的孤儿 pass";
    auto* readout_pass = pipeline->fpsOverlayPass();
    ASSERT_NE(readout_pass, nullptr);
    EXPECT_TRUE(readout_pass->isEnabled());
    EXPECT_TRUE(pipeline->isStatsVisible());

    // 再上屏几次只该重画：链不重建、pass 不重复注册。
    const std::size_t registered = engine->passCount();
    view->activate();
    view->activate();
    EXPECT_EQ(engine->passCount(), registered) << "activate() 可重复：读数不能挂两遍";
    EXPECT_EQ(scene_view->pipeline(), pipeline);

    // 宿主想关掉读数：开关只切 pass 的 enabled（引擎跳过被禁用的 pass），链与 pass 表都不动。
    EXPECT_TRUE(pipeline->setStatsVisible(false));
    EXPECT_FALSE(readout_pass->isEnabled());
    EXPECT_FALSE(pipeline->fpsOverlayPass()->isEnabled());
    EXPECT_EQ(engine->passCount(), registered);
    EXPECT_EQ(scene_view->pipeline(), pipeline) << "开关不该重建链";
    EXPECT_TRUE(pipeline->setStatsVisible(true));
    EXPECT_TRUE(readout_pass->isEnabled());

    // 关掉之后**重建**（预设加一个坐标轴 ⇒ 一条新链）不该把它偷偷点亮：重建读的是活着那条链的运行期选择。
    vn::intrusive_ptr<vn::graphics::RenderPipeline> old_pipeline(pipeline);  // 拿住它：地址会因为回收而被重用，比对象才作数
    auto options = control->viewerOptions();
    EXPECT_TRUE(pipeline->setStatsVisible(false));
    options.gizmo = true;
    ASSERT_TRUE(control->setViewerOptions(options)) << "结构变化 = 一次建链";
    auto* rebuilt = scene_view->pipeline();
    ASSERT_NE(rebuilt, nullptr);
    ASSERT_NE(rebuilt, old_pipeline.get()) << "结构变了，链是新的";
    EXPECT_TRUE(rebuilt->isGizmoVisible()) << "新要的坐标轴亮着";
    EXPECT_FALSE(rebuilt->isStatsVisible()) << "使用者关掉的读数不许跟着重建回来";
    EXPECT_TRUE(rebuilt->setStatsVisible(true));

    // 改大小安全：读数的摆位与尺寸由链在 resize 时算（不再有查看器那套 layout 步）。
    scene_view->resize(400, 300, control->devicePixelRatio());
    EXPECT_TRUE(scene_view->pipeline()->fpsOverlayPass()->isEnabled());
}

TEST(ModelViewerTest, TheShadingModeCommandsDriveTheCurrentModelView)
{
    // 走**真实**那条路：命令管理器执行命令（命令层的用途就是这个），命令通过宿主发布的那个"当前 3D 视图"找到
    // 要改的视图 —— 所以这条用例用的是**应用自己的**文档管理器与视图注册表（生产里只有一个），而不是用例临时
    // 造的那一套（那是给不碰命令管道的用例用的）。
    auto* app = dynamic_cast<vn::appfw::gui::GuiApplication*>(vn::appfw::Application::current());
    ASSERT_NE(app, nullptr) << "这条用例要在 GUI 应用里跑";
    auto* documents = app->documentManager();
    auto* views     = app->viewRegistry();
    auto* commands  = app->commandManager();
    ASSERT_NE(documents, nullptr);
    ASSERT_NE(views, nullptr);
    ASSERT_NE(commands, nullptr);

    ASSERT_TRUE(registerModelType(*documents));
    ASSERT_TRUE(registerModelOpener(*documents));
    ASSERT_TRUE(views->registerView(kModelType, vn::model_viewer::createRenderView));

    vn::appfw::gui::MainWindow window;
    auto*                      docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);
    vn::appfw::gui::CentralDocumentHost host(*documents, *views, *docks);

    // 打开并显示一份模型：宿主显示 ⇒ 视图 activate() ⇒ 把它的控件发布成"当前 3D 视图"。
    ASSERT_NE(openModel(*documents, u8"立方体", vn::model_viewer::makeBoxMesh()), nullptr);
    auto* view = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(view, nullptr);
    auto* control = view->renderControl();
    ASSERT_NE(control, nullptr);
    auto* scene_view = control->view();
    ASSERT_NE(scene_view, nullptr);
    ASSERT_NE(scene_view->pipeline(), nullptr) << "上屏之后视图该有自己的默认管线";
    // 命令靠"宿主发布的那个当前 3D 视图"找到要改的东西，所以先把这两件事说清楚：现在的主窗口就是这一个，
    // 而这份文档的控件被发布在它上面（视图在 activate() 里发布）。
    ASSERT_EQ(vn::appfw::gui::MainWindow::current(), &window);
    ASSERT_EQ(window.primaryRenderControl(), control) << "视图上屏时该把自己的控件发布到主窗口（命令层的唯一入口）";

    // 命令按名字注册（插件自己的自动登记队列只在插件模块里 flush，这里是可执行文件 ⇒ 用例显式登记）。
    ASSERT_TRUE(commands->registerCommand<vn::model_viewer::SetShadingLitCommand>(
        vn::String(vn::model_viewer::kShadingModeLitCommand)));
    ASSERT_TRUE(commands->registerCommand<vn::model_viewer::SetShadingFlatCommand>(
        vn::String(vn::model_viewer::kShadingModeFlatCommand)));
    ASSERT_TRUE(commands->registerCommand<vn::model_viewer::SetShadingUnlitCommand>(
        vn::String(vn::model_viewer::kShadingModeUnlitCommand)));
    ASSERT_TRUE(commands->registerCommand<vn::model_viewer::SetShadingNormalsCommand>(
        vn::String(vn::model_viewer::kShadingModeNormalsCommand)));
    ASSERT_TRUE(commands->registerCommand<vn::model_viewer::SetShadingWireframeCommand>(
        vn::String(vn::model_viewer::kShadingModeWireframeCommand)));

    // 五档各跑一遍：每一档都要求"视图那条链真的换了档"，而且**别的设置还在**（帧率读数是查看器开的，命令走的是链上
    // 那一处开关，不该顺手把它关掉）。
    struct Case {
        const char8_t*            command;
        vn::graphics::ShadingMode mode;
        bool                      wireframe;
    };
    const Case cases[] = {
        { vn::model_viewer::kShadingModeLitCommand, vn::graphics::ShadingMode::Lit, false },
        { vn::model_viewer::kShadingModeFlatCommand, vn::graphics::ShadingMode::Flat, false },
        { vn::model_viewer::kShadingModeUnlitCommand, vn::graphics::ShadingMode::Unlit, false },
        { vn::model_viewer::kShadingModeNormalsCommand, vn::graphics::ShadingMode::Normals, false },
        { vn::model_viewer::kShadingModeWireframeCommand, vn::graphics::ShadingMode::Wireframe, true },
    };
    for (const Case& item : cases) {
        const auto result = commands->executeCommandAndWait(vn::String(item.command));
        EXPECT_EQ(result.status(), vn::appfw::CommandStatus::Success)
            << "命令该成功（说清了什么就不算失败：" << result.message().as_std_str() << "）";
        EXPECT_EQ(scene_view->pipeline()->shadingMode(), item.mode);
        EXPECT_TRUE(scene_view->pipeline()->isStatsVisible()) << "命令不该把帧率读数顺手关掉";
        // 线框是"程序 + 光栅化"两个覆盖；其余档不许留下光栅化覆盖。
        const auto polygon = scene_view->pipeline()->windowPass() != nullptr
                                 ? scene_view->pipeline()->windowPass()->polygonModeOverride()
                                 : std::optional<vn::graphics::PolygonMode>{};
        if (item.wireframe) {
            ASSERT_TRUE(polygon.has_value());
            EXPECT_EQ(*polygon, vn::graphics::PolygonMode::Line);
        }
        else {
            EXPECT_FALSE(polygon.has_value()) << "非线框档不该留光栅化覆盖";
        }
    }

    // 命令改的是**这个控件的视图**（一块区只画一幅画，多格已下线）：`primaryRenderControl()` 找到窗口上那块 3D 控件，
    // 命令就作用在它的视图上 —— 不需要焦点、也不需要认识任何具体视图类。
    EXPECT_EQ(scene_view, control->view()) << "这份文档的视图就是控件那个视图";

    // 宿主自己搭的链：命令碰上它时只问那条链能不能换档，不替它重建（"别人搭的链别人管"）。把控件自己那个视图的链
    // 换成宿主的，就能看见这一条路。
    {
        vn::graphics::RenderPipelineBuilder builder(control->engine());
        builder.setCamera(scene_view->camera());
        builder.setContent(scene_view->scene());
        scene_view->setPipeline(builder.build({}));
    }
    ASSERT_NE(scene_view->pipeline(), nullptr);
    const auto host_chain_run = commands->executeCommandAndWait(vn::String(vn::model_viewer::kShadingModeUnlitCommand));
    EXPECT_EQ(host_chain_run.status(), vn::appfw::CommandStatus::Success);
    EXPECT_EQ(scene_view->pipeline()->shadingMode(), vn::graphics::ShadingMode::Unlit)
        << "宿主搭的链也换得了这一档（命令问的就是那条链）";

    // 守卫：当前文档不是模型（这里直接清掉当前文档）⇒ 命令如实说"不适用"，而不是去改别人的视图。
    ASSERT_TRUE(documents->setCurrent(nullptr));
    const auto refused = commands->executeCommandAndWait(vn::String(vn::model_viewer::kShadingModeWireframeCommand));
    EXPECT_EQ(refused.status(), vn::appfw::CommandStatus::NotApplicable);
    EXPECT_FALSE(refused.message().empty()) << "要说清为什么（不静默）";
    // 下屏的另一半：视图 deactivate() 时把自己发布的那个收回，窗口上不该留下已经下屏的控件。
    EXPECT_EQ(window.primaryRenderControl(), nullptr) << "下屏时该把发布出去的控件收回去";
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

TEST(ModelViewerTest, AFilePayloadReachesTheRenderSceneInTheSourceUnits)
{
    // 文件载荷那条路（打开器自己调 MeshLoader）。这条钉两件事：①读出来的模型真的走到**渲染几何**上（而不只是文档
    // 里有个网格）；②查看器**不做单位换算** —— 屏幕上就是文件里写的坐标。加载器默认的 Auto 会把这份 1 单位的三角形
    // 放大 1000 倍，而内存载荷那个盒子是 ±1，两份文档会差出三个数量级。
    const std::filesystem::path stl_path = std::filesystem::temp_directory_path() / "vine_model_viewer_units.stl";
    vn::model_viewer::writeTriangleStlFile(stl_path);

    vn::appfw::DocumentManager           documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow           window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerMeshFileOpener(documents));
    ASSERT_TRUE(views.registerView(kModelType, vn::model_viewer::createRenderView));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);

    vn::model_viewer::MeshFilePayload payload;
    payload.file_path = stl_path;

    vn::appfw::Document* document = documents.open(&payload);
    ASSERT_NE(document, nullptr) << "读不回来时打开器返回空，这条用例就没有意义了";
    ASSERT_TRUE(documents.setCurrent(document));

    // 打开器读完就只留文档手里的网格，之后不再看那个文件：用例自己收拾它（命令那边不再造临时文件了，
    // 它读的是用户给的文件，而别人的文件不该被命令删掉）。
    std::filesystem::remove(stl_path);

    auto* view = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(view, nullptr);
    view->activate(); // 场景是懒建的：上屏（这里显式叫一次）才造

    auto* control = view->renderControl();
    ASSERT_NE(control, nullptr);
    auto* scene_view = control->view();
    ASSERT_NE(scene_view, nullptr);
    const auto scene = scene_view->scene();
    ASSERT_NE(scene, nullptr) << "视图必须真的给这份文档建了场景";

    const auto root = scene->root();
    ASSERT_NE(root, nullptr);
    const auto* group = dynamic_cast<const vn::graphics::Group*>(root.get());
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->children().size(), 1u);
    const auto* geometry = dynamic_cast<const vn::graphics::Geometry*>(group->children()[0].get());
    ASSERT_NE(geometry, nullptr) << "场景里那一个孩子就是网格的几何";

    // 文件里就是一个三角形，坐标就是那三个 —— 没被单位换算碰过。
    const auto* positions = geometry->buffer(0);
    ASSERT_NE(positions, nullptr);
    EXPECT_EQ(positions->vertexCount(), 3u);
    EXPECT_TRUE(hasVertex(*geometry, 0.0f, 0.0f, 0.0f));
    EXPECT_TRUE(hasVertex(*geometry, 1.0f, 0.0f, 0.0f));
    EXPECT_TRUE(hasVertex(*geometry, 0.0f, 1.0f, 0.0f));
}

// 点击选中：视图自己带一个拾取型操纵器（`ModelPickManipulator`），导航原样转发、点击交回视图 —— 选择是**业务**
// （住在这份视图里），框架里没有"选择"这个概念。这条钉四件事：① 装的确实是拾取型操纵器；② 点在模型上 ⇒ 命中并
// 换上高亮材质；③ 点空白 ⇒ 取消选择、**原来那份材质原样还回去**（不是重造一份）；④ 拖拽（按下与松开不在一处）
// 不选中 —— 没有这条松弛量，转完相机就会顺手选中一个东西。
TEST(ModelViewerTest, AClickSelectsTheGeometryUnderTheCursorAndHighlightsIt)
{
    vn::appfw::DocumentManager          documents;
    vn::appfw::gui::DocumentViewRegistry views;
    vn::appfw::gui::MainWindow          window;

    auto* docks = window.dockPanelManager();
    ASSERT_NE(docks, nullptr);

    ASSERT_TRUE(registerModelType(documents));
    ASSERT_TRUE(registerModelOpener(documents));
    ASSERT_TRUE(views.registerView(kModelType, vn::model_viewer::createRenderView));

    vn::appfw::gui::CentralDocumentHost host(documents, views, *docks);
    ASSERT_NE(openModel(documents, u8"立方体", vn::model_viewer::makeBoxMesh()), nullptr);

    auto* view = dynamic_cast<vn::model_viewer::ModelRenderView*>(host.currentView());
    ASSERT_NE(view, nullptr);
    auto* control = view->renderControl();
    ASSERT_NE(control, nullptr);
    auto* scene_view = control->view();
    ASSERT_NE(scene_view, nullptr);

    // 相机与视口由用例定死：这条判的是拾取与高亮，不是"视图自己怎么对准内容"（那条归 fitToScreen）。
    // 站位刻意留了余量：立方体是 2×2×2（半边长 1），相机拉到 z=7、fov 30 ⇒ 画面角落那条射线在 z=1 平面上
    // 偏出 1.6 左右，**真的落空**（判"点空白"必须真的落空，否则量到的是别的东西）。
    auto* camera = scene_view->camera();
    ASSERT_NE(camera, nullptr);
    camera->lookAt(vn::math::Vec3d(0.0, 0.0, 7.0), vn::math::Vec3d(0.0, 0.0, 0.0),
                                  vn::math::Vec3d(0.0, 1.0, 0.0));
    camera->setPerspective(30.0, 0.1, 100.0);
    camera->setAspect(1.0);
    scene_view->resize(400, 400, 1.0);

    // ① 视图装的操纵器是自己的拾取型（不是默认那个裸轨道操纵器）。
    auto* manipulator = scene_view->manipulator();
    ASSERT_NE(manipulator, nullptr);
    EXPECT_NE(dynamic_cast<vn::model_viewer::ModelPickManipulator*>(manipulator), nullptr)
        << "视图必须把点击交回自己（ModelPickManipulator）";

    const auto scene = scene_view->scene();
    ASSERT_NE(scene, nullptr);
    const auto root = scene->root();
    ASSERT_NE(root, nullptr);
    const auto* group = dynamic_cast<const vn::graphics::Group*>(root.get());
    ASSERT_NE(group, nullptr);
    ASSERT_EQ(group->children().size(), 1u);
    auto* geometry = dynamic_cast<vn::graphics::Geometry*>(group->children()[0].get());
    ASSERT_NE(geometry, nullptr);
    const vn::graphics::Material* const original = geometry->material();
    ASSERT_NE(original, nullptr) << "模型几何自带那份中灰材质（见 makeGeometry()）";
    EXPECT_EQ(view->selectedGeometry(), nullptr) << "一开始没有选中任何东西";

    const auto click = [&](double x, double y) {
        vn::window::MouseEvent press;
        press.button  = vn::window::MouseButton::Left;
        press.x       = x;
        press.y       = y;
        press.pressed = true;
        manipulator->onMousePress(press);
        vn::window::MouseEvent release = press;
        release.pressed                = false;
        manipulator->onMouseRelease(release);
    };

    // ② 点画面正中（相机就是照原点的 ⇒ 射线命中那个立方体）：选中 + 高亮材质。
    click(200.0, 200.0);
    EXPECT_EQ(view->selectedGeometry(), geometry) << "命中的就是场景里那件几何";
    ASSERT_NE(geometry->material(), nullptr);
    EXPECT_NE(geometry->material(), original) << "选中要看得见：材质换成了高亮那份";
    const vn::graphics::Material* const highlight_material = geometry->material();
    const vn::Colorf highlight_colour = geometry->material()->diffuse();
    EXPECT_GT(highlight_colour.r, highlight_colour.b) << "高亮是暖色（橙），与中灰的模型分得开";

    // ③ 点空白（角落）：取消选择，材质**原样**还回去。
    // 角落必须真的落空（见上面站位的余量），否则这条量到的是"点中了别的东西"。
    click(4.0, 4.0);
    EXPECT_EQ(view->selectedGeometry(), nullptr) << "点空白 = 取消选择";
    EXPECT_EQ(geometry->material(), original) << "还回去的必须是原来那一份，不是一件复制品";

    // ④ 拖拽不选中，且**导航照旧**：按下与松开不在一处（超过松弛量）⇒ 不拾取；
    //    中间那几个移动事件原样转发给内层轨道操纵器 ⇒ 相机确实动了（拾取那一半没有把导航吃掉）。
    const vn::math::Vec3d eye_before = camera->eye();
    {
        vn::window::MouseEvent press;
        press.button  = vn::window::MouseButton::Left;
        press.x       = 200.0;
        press.y       = 200.0;
        press.pressed = true;
        manipulator->onMousePress(press);

        vn::window::MouseEvent move = press;
        move.x                      = 230.0;
        move.y                      = 240.0;
        manipulator->onMouseMove(move);
        move.x = 260.0;
        move.y = 240.0;
        manipulator->onMouseMove(move);

        vn::window::MouseEvent release = move;
        release.pressed                = false;
        manipulator->onMouseRelease(release);
    }
    EXPECT_NE(camera->eye().x, eye_before.x) << "拖拽仍然是转相机（事件转发给了轨道操纵器）";
    EXPECT_EQ(view->selectedGeometry(), nullptr) << "转相机不是选东西：拖拽结束不拾取";
    EXPECT_EQ(geometry->material(), original);

    // ⑤ 再点一次仍然选中同一件，且用的还是那份高亮材质（视图只备一份，不每次拾取再造一件）。
    click(200.0, 200.0);
    EXPECT_EQ(view->selectedGeometry(), geometry);
    ASSERT_NE(geometry->material(), nullptr);
    EXPECT_EQ(geometry->material(), highlight_material) << "高亮材质是视图自己那一份，跨次拾取复用";
    EXPECT_EQ(geometry->material()->diffuse().r, highlight_colour.r);
}
