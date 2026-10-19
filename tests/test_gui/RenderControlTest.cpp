#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPoint>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/appfw/gui/Viewer.hpp>

#include <vine/async/DetachedTask.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/CameraManipulator.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderPipelineBuilder.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/SceneView.hpp>
#include <vine/window/InputEvent.hpp>

namespace
{

using vn::appfw::gui::RenderControl;

/// Backend stub.
///
/// The surface lifecycle is about *when* the control attaches, not about pixels, so what the
/// control asks of the backend is all the test needs: the handle it was handed, the size it was
/// told, and how often initialize() was called. A stub also makes the failure paths reachable
/// without a GPU, which is what pins "retries, then gives up" and "never attaches to an empty
/// surface".
class StubBackend : public vn::graphics::RenderBackend
{
  public:
    /// The WINDOW HALF of the stub (see vn::graphics::RenderDevice::createSurfaceTarget): everything the control
    /// asks about the surface - the window the target was made for, the size it was told, the presents it drove.
    class StubSurface : public vn::graphics::WindowPresentTarget
    {
      public:
        void setWindowHandle(void* handle) override
        {
            last_handle = handle;
            ++handle_calls;
        }
        [[nodiscard]] void* nativeHandle() const override { return last_handle; }
        void resize(int w, int h) override
        {
            width  = w;
            height = h;
            ++resize_calls;
        }
        void present() override { ++presents; }

      public:
        int   handle_calls{ 0 };
        int   resize_calls{ 0 };
        int   presents{ 0 };
        void* last_handle{ nullptr };
        int   width{ 0 };
        int   height{ 0 };
    };

    bool initialize() override
    {
        ++initialize_calls;
        return up = initialize_calls > failures;
    }

    void shutdown() override
    {
        ++shutdown_calls;
        up = false;
    }

    /// Whether this device is up (see RenderDevice::initialized): the CONTROL is its creator, so the attach asks -
    /// the engine, which only borrows it, never brings it up.
    [[nodiscard]] bool initialized() const noexcept override { return up; }

    void executeFrame(const vn::graphics::FrameDescription& frame,
                      vn::graphics::PresentTarget* present_to = nullptr) override
    {
        ++frames;
        present_targets.push_back(present_to);
        for (const vn::graphics::FrameDescription::Pass& entry : frame.passes) {
            draws += entry.description.has_value() ? entry.description->draws.size() : 0u;
        }
        // The frame's tail is the executor's (see RenderFrameExecutor::executeFrame): this stub presents through the
        // target the frame named, exactly as the real device does.
        if (present_to != nullptr) {
            present_to->present();
        }
    }

    void setRenderTarget(vn::graphics::RenderTarget*) override {}

    [[nodiscard]] vn::graphics::RenderFrameExecutor& executor() override { return *this; }

    /// Hands out the one window half this stub has (see RenderDevice::createSurfaceTarget): the window the control
    /// asks for is remembered, so "the target was made for THIS window" is checkable without a GPU. A stub whose
    /// `presents_nothing` is set answers the way a display-less device does, without a report.
    [[nodiscard]] vn::raw_ptr<vn::graphics::WindowPresentTarget>
    createSurfaceTarget(void* native_handle, vn::graphics::SurfaceTargetResult* why = nullptr) override
    {
        ++target_calls;
        if (presents_nothing) {
            if (why != nullptr) {
                *why = vn::graphics::SurfaceTargetResult::NoPresentation;
            }
            return nullptr;
        }
        surface_half.setWindowHandle(native_handle);
        if (why != nullptr) {
            *why = vn::graphics::SurfaceTargetResult::Ok;
        }
        return &surface_half;
    }

  public:
    StubSurface surface_half;

    /// How many initialize() calls should report failure.
    int failures{ 0 };

    /// Set to make this stub a device that presents nothing (see createSurfaceTarget).
    bool presents_nothing{ false };

    /// Whether this device is up (see RenderDevice::initialized): false until its creator brings it up.
    bool up{ false };

    int   initialize_calls{ 0 };
    int   shutdown_calls{ 0 };
    int   target_calls{ 0 };
    /// How many frames the control's views ran, how many drawing calls they carried, and what each frame named as
    /// the surface it resolves to: the stub's own reading of the value the engine handed over (see
    /// RenderFrameExecutor::executeFrame).
    int   frames{ 0 };
    std::size_t draws{ 0 };
    std::vector<vn::graphics::PresentTarget*> present_targets;
};

/// Pumps the event loop until 'done' holds or 'timeout_ms' elapses.
/// @param done Predicate to wait for.
/// @param timeout_ms Upper bound on the wait.
/// @return true when the predicate held before the timeout.
bool pumpUntil(const std::function<bool()>& done, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        QApplication::processEvents();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QApplication::processEvents();
    return done();
}

/// 记录型操纵器：只回答"哪些事件到了这个视图"。分屏那两条用例用它把**路由**变成可断言的事实 —— 相机怎么动是
/// OrbitCameraManipulator 自己的用例的事，这里问的是"这条输入到底去了哪一格"。
class RecordingManipulator : public vn::graphics::CameraManipulator
{
  public:
    /// 需要一个相机（基类要它）：用哪个都行 —— 这个双只是"事件到了没有"的记录器，不驱动任何相机。
    explicit RecordingManipulator(vn::raw_ptr<vn::graphics::Camera> camera)
      : vn::graphics::CameraManipulator(camera)
    {}

  public:
    void apply() override {}
    void onMousePress(const vn::window::MouseEvent&) override { ++mouse_presses; }
    void onMouseMove(const vn::window::MouseEvent&) override { ++mouse_moves; }
    void onMouseRelease(const vn::window::MouseEvent&) override { ++mouse_releases; }
    void onScroll(const vn::window::ScrollEvent&) override { ++scrolls; }
    void onKeyDown(const vn::window::KeyEvent&) override { ++key_downs; }
    void onKeyUp(const vn::window::KeyEvent&) override { ++key_ups; }
    void onResize(const vn::window::ResizeEvent&) override { ++resizes; }

  public:
    int mouse_presses{ 0 };
    int mouse_moves{ 0 };
    int mouse_releases{ 0 };
    int scrolls{ 0 };
    int key_downs{ 0 };
    int key_ups{ 0 };
    int resizes{ 0 };
};

/// Hosts a RenderControl in a window, with a stub backend unless asked not to have one.
class HostedControl
{
  public:
    explicit HostedControl(bool with_backend = true)
    {
        // The control keeps its surface to itself (it creates it in its own constructor), so the test
        // takes it the one way left open: it is the window that appears while the control is built.
        const QWindowList before = QGuiApplication::allWindows();

        layout_.reset(new QVBoxLayout(&window_));
        control_ = new RenderControl();
        layout_->addWidget(control_->impl<QWidget>());

        for (QWindow* candidate : QGuiApplication::allWindows()) {
            if (!before.contains(candidate) && candidate->surfaceType() == QSurface::VulkanSurface) {
                surface_ = candidate;
                break;
            }
        }

        if (with_backend) {
            stub_ = new StubBackend();
            control_->engine()->setDevice(vn::intrusive_ptr<vn::graphics::RenderBackend>(stub_));
        }

        window_.resize(320, 240);
    }

    void show() { window_.show(); }

    /// Resizes the window the control lives in (the layout then gives the surface the new area).
    /// @param w New window width.
    /// @param h New window height.
    void resizeWindow(int w, int h) { window_.resize(w, h); }

    RenderControl* control() const { return control_; }
    StubBackend*   stub() const { return stub_; }
    QWindow*       surface() const { return surface_; }

  private:
    QWidget                      window_;
    std::unique_ptr<QVBoxLayout> layout_;
    RenderControl*               control_{ nullptr };
    StubBackend*                 stub_{ nullptr };
    QWindow*                     surface_{ nullptr };
};

/**
 * @brief A second pane on @p engine: its own view with the chain a HOST builds for it, adopted.
 *
 * The order a host has on a surface of its own is build -> adopt -> declare: RenderSurface::present resolves a
 * view's declaration to the window pass of the pipeline the view adopted and builds nothing itself (a pane whose
 * chain nobody built has nothing to point at, and is refused). The control does this for ITS view (see
 * RenderControl::viewerOptions); a pane the test adds is the test's own.
 *
 * @param engine Engine the pane's passes are registered on (the control's, in these tests).
 * @return The view, ready to present.
 */
vn::intrusive_ptr<vn::graphics::SceneView> secondPane(vn::graphics::RenderEngine* engine)
{
    auto view = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    view->setEngine(engine);
    vn::graphics::RenderPipelineBuilder builder(engine);
    builder.setCamera(view->camera());
    builder.setContent(view->scene());
    view->setPipeline(builder.build({}));
    return view;
}

/**
 * @brief A pane that waits for its chain: a view bound to @p engine, with nothing adopted and nothing declared.
 *
 * The shape Viewer::addView takes (it builds the pane from its request itself), so the test states a request and
 * lets the viewer own the build rather than pre-building a chain the way secondPane does.
 *
 * @param engine Engine the pane's chain will be registered on (the control's, in these tests).
 * @return The view, ready to be added to a viewer.
 */
vn::intrusive_ptr<vn::graphics::SceneView> barePane(vn::graphics::RenderEngine* engine)
{
    auto view = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    view->setEngine(engine);
    return view;
}

} // namespace

// 控件自驱：宿主把后端装上、把窗口显示出来就够了 —— 不需要自己挑时机调 init()（它仍然是显式入口，幂等）。
TEST(RenderControlTest, AttachesItselfOnceTheBackendAndWindowAreThere)
{
    HostedControl host; // 控件先建、后端后装：这正是 SDK 里写的次序（engine()->setDevice() 在 init() 之前）
    host.show();

    // 窗口一显示（容器的 show ⇒ 控件自己的重试）会话就起来了，那一帧也落了进去。
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_NE(host.stub()->surface_half.last_handle, nullptr);
    EXPECT_GT(host.stub()->surface_half.width, 0);
    EXPECT_GT(host.stub()->surface_half.height, 0);

    ASSERT_NE(host.surface(), nullptr);
    EXPECT_TRUE(host.surface()->isVisible()); // 有帧可看 ⇒ 表面就位
}

// 这块表面由**谁**画：控件把自己的视图声明在表面自己的呈现表上（`RenderControl::surface()`）。这条表是
// "谁呈现"的唯一记录（今天只有一条；分屏就是多声明几条），宿主要接手时也声明在这里。
TEST(RenderControlTest, TheControlsSurfacePresentsItsOwnViewByDefault)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    auto* control = host.control();
    auto* surface = control->surface();
    ASSERT_NE(surface, nullptr);
    ASSERT_TRUE(surface->hasPresentation());
    EXPECT_EQ(surface->presentedView(), control->view());
    EXPECT_NE(surface->presentingPass(), nullptr);

    // 那块 pass 就是该视图默认管线里那条窗口 pass（视图重建时表跟着走 —— 见 RenderSurfaceTest）。
    ASSERT_NE(control->view()->pipeline(), nullptr);
    EXPECT_EQ(surface->presentingPass(), control->view()->pipeline()->windowPass());
    // 这块表面的**唯一一口清屏**留在要清的那条 pass 上（默认画面照旧清成窗口灰）：区不碰它的清屏策略。
    EXPECT_TRUE(surface->presentingPass()->isClearEnabled());

    // 一块区只画一幅画：再挂一个视图是**被拒**的（而且说出来 —— 以前第二条会静默丢掉自己那个 clearColor，或者
    // 悄悄擦掉第一条画的东西）。
    auto right = secondPane(control->engine());
    EXPECT_FALSE(surface->present(*right));
    EXPECT_EQ(surface->presentedView(), control->view());
    EXPECT_TRUE(surface->presentingPass()->isClearEnabled());
    control->renderFrame();
}

// 查看器预设与运行期换档是两件事：预设说"走哪条链、哪些档、档要能表达什么"，换档问的是"这条链能不能表达、
// 以及**这个请求**是不是就该由它来画"。延迟预设 + 非 Lit 档 ⇒ 链按前向建（effectivePath）；这时选回 Lit，
// 前向链当然表达得了 Lit，但这个请求要的是延迟 ⇒ **建回延迟**（老规矩，见 RenderPipelineBuilder::effectivePath）。
TEST(RenderControlTest, ADeferredViewerSwitchesChainOnlyWhenTheRequestNeedsIt)
{
    HostedControl host;
    auto*         control = host.control();
    ASSERT_NE(control, nullptr);

    vn::appfw::gui::ViewerOptions options;  // 预设：延迟（宿主写的就是它，链只是它的实现）
    options.path = vn::graphics::ShadingPath::Deferred;
    ASSERT_TRUE(control->setViewerOptions(options));
    auto* pipeline = control->view()->pipeline();
    ASSERT_NE(pipeline, nullptr);
    EXPECT_EQ(pipeline->shadingPath(), vn::graphics::ShadingPath::Deferred);
    EXPECT_NE(pipeline->offscreenTarget(), nullptr) << "延迟链的输入是 G-buffer";
    EXPECT_EQ(control->viewerOptions().path, vn::graphics::ShadingPath::Deferred) << "预设仍是宿主写的那个";

    // 非 Lit 档：延迟链表达不了它（没有几何趟可改）⇒ 换档就是**重建**，链变前向、G-buffer 退场。
    ASSERT_TRUE(control->setShadingMode(vn::graphics::ShadingMode::Unlit));
    auto* forward = control->view()->pipeline();
    ASSERT_NE(forward, nullptr);
    EXPECT_NE(forward, pipeline) << "重建了：链不一样了";
    EXPECT_EQ(forward->shadingPath(), vn::graphics::ShadingPath::Forward);
    EXPECT_EQ(forward->shadingMode(), vn::graphics::ShadingMode::Unlit);
    EXPECT_EQ(forward->offscreenTarget(), nullptr) << "非 Lit 档不需要 G-buffer";
    EXPECT_EQ(control->viewerOptions().mode, vn::graphics::ShadingMode::Unlit) << "运行期那一档要记进预设";

    // 同一条（前向）链上表达得了的另一档：零重建 —— 预设跟着走（否则下次读-改-写会把旧档带回来）。
    auto held = vn::intrusive_ptr<vn::graphics::RenderPipeline>(forward);
    EXPECT_TRUE(control->setShadingMode(vn::graphics::ShadingMode::Wireframe));
    EXPECT_EQ(control->view()->pipeline(), held.get()) << "同一条链上换档不该重建";
    EXPECT_EQ(control->viewerOptions().mode, vn::graphics::ShadingMode::Wireframe);

    // 选回 Lit：链能表达它，但请求要的是延迟 ⇒ 建回延迟，且 G-buffer 回来。
    EXPECT_TRUE(control->setShadingMode(vn::graphics::ShadingMode::Lit));
    auto* deferred = control->view()->pipeline();
    ASSERT_NE(deferred, nullptr);
    EXPECT_EQ(deferred->shadingPath(), vn::graphics::ShadingPath::Deferred) << "请求说了算：回到延迟";
    EXPECT_NE(deferred->offscreenTarget(), nullptr);
    EXPECT_EQ(control->viewerOptions().mode, vn::graphics::ShadingMode::Lit);
}

// 构造只组装：`new RenderControl()` 返回时**没有** attach（状态停在 Pending），配置写在它之后的同步代码里；// 第一拍兜底才 attach —— 而兜底用的正是宿主刚装上的那个后端（不是注册表里的默认）。这就是"配置窗口"，
// 也是 SDK 写的次序（构造 ⇒ setDevice() ⇒ init()）不再依赖"构造那一刻注册表里有没有后端"的原因。
TEST(RenderControlTest, ConstructionOnlyComposesAndTheFirstTurnAttaches)
{
    HostedControl host; // 后端在构造之后才装

    // 构造返回时：引擎与视图在、会话没开始。
    ASSERT_NE(host.control()->engine(), nullptr);
    ASSERT_NE(host.control()->view(), nullptr);
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Pending);
    EXPECT_EQ(host.stub()->initialize_calls, 0);

    // 不 show、不 init()：只让事件循环转一圈，兜底就把会话开起来（"有帧可看"不要求窗口可见）。
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_EQ(host.control()->engine()->device(), host.stub()); // 宿主自己装的后端赢，兜底只是补时机
}

// 尺寸与比例**一起**到视图：控件把自己的逻辑尺寸与 devicePixelRatio 推给视图，视图再从这一对推出它自己的
// 矩形（相机宽高比、窗口 pass 视口、离屏目标/HUD 尺寸）—— 宿主一个 DPR 都不用记，也不会出现"只推了尺寸"。
TEST(RenderControlTest, TheSurfaceSizeAndRatioReachTheViewTogether)
{
    HostedControl host;

    bool   got    = false;
    int    got_w  = 0;
    int    got_h  = 0;
    double got_pr = 0.0;
    host.control()->view()->addSurfaceLayout([&](int w, int h, double ratio) {
        got    = true;
        got_w  = w;
        got_h  = h;
        got_pr = ratio;
    });

    host.show();
    host.resizeWindow(640, 480);

    ASSERT_TRUE(pumpUntil([&] { return got; }, 3000));
    EXPECT_GT(got_w, 0);
    EXPECT_GT(got_h, 0);
    EXPECT_DOUBLE_EQ(got_pr, host.control()->devicePixelRatio());
}

// 一块区只画一幅画（多格已下线）：输入没有"路由"可谈 —— 每个事件都属于这个视图，而**按下捕获**仍然有用：
// 拖拽飞出控件也继续由它接管，松手之后下一次悬停不再转相机。两个按键各自捕获，互不打断。
TEST(RenderControlTest, TheViewReceivesInputAndADragKeepsSteeringIt)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    host.resizeWindow(640, 480);

    auto* control = host.control();
    ASSERT_TRUE(pumpUntil([&] { return control->width() > 0 && control->height() > 0; }, 3000));

    auto own = vn::intrusive_ptr<vn::graphics::SceneView>(control->view());

    QWindow* const surface = host.surface();
    ASSERT_NE(surface, nullptr);
    const auto send = [&](QEvent::Type type, int x, int y, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, y), surface->mapToGlobal(QPoint(x, y)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(surface, &event);
    };
    const auto moved = [](const vn::math::Vec3d& a, const vn::math::Vec3d& b) { return (a - b).length() > 1.0e-9; };

    // 左键按下并拖动 ⇒ 这个视图的相机跟着动（事件确实到了它手上）。
    const vn::math::Vec3d before = own->camera()->eye();
    send(QEvent::MouseButtonPress, 100, 120, Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseMove, 140, 150, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(moved(own->camera()->eye(), before)) << "按下并拖动 ⇒ 视图接管这次拖拽";

    // 拖拽中的移动**飞出了控件**（负坐标）也继续由它接管：捕获的是按下时那个视图，不是位置。
    const vn::math::Vec3d dragged = own->camera()->eye();
    send(QEvent::MouseMove, -40, -30, Qt::NoButton, Qt::LeftButton);
    EXPECT_TRUE(moved(own->camera()->eye(), dragged)) << "捕获期间指针漂到控件外仍然steering这个视图";

    // 右键的拖拽同时进行，松开右键不打断左键那次。
    const vn::math::Vec3d both_held = own->camera()->eye();
    send(QEvent::MouseButtonPress, 200, 140, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    send(QEvent::MouseMove, 230, 170, Qt::NoButton, Qt::LeftButton | Qt::RightButton);
    send(QEvent::MouseButtonRelease, 230, 170, Qt::RightButton, Qt::LeftButton);
    EXPECT_TRUE(moved(own->camera()->eye(), both_held)) << "右键的拖拽会动相机（另一档操作）";

    // 左键也松开 ⇒ 之后没有任何键按下的悬停不许再动相机。
    send(QEvent::MouseButtonRelease, 230, 170, Qt::LeftButton, Qt::NoButton);
    const vn::math::Vec3d settled = own->camera()->eye();
    send(QEvent::MouseMove, 260, 190, Qt::NoButton, Qt::NoButton);
    EXPECT_FALSE(moved(own->camera()->eye(), settled)) << "松手之后纯悬停不该再转相机";
}

// 按需渲染是默认（每帧都由"有变化"触发），连续渲染是另一档：开着一滴答一帧，关掉就回到按需。
// 两件事同时被钉住：开着真的会自己出帧（不需要任何输入），关掉之后**一点都不再出**（不是"变慢"）。
TEST(RenderControlTest, ContinuousRenderingDrivesFramesWithoutInput)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    host.resizeWindow(640, 480);
    ASSERT_TRUE(pumpUntil([&] { return host.control()->width() > 0; }, 3000));

    // 先安静一会儿：按需模式下没有输入就没有帧（这条本身也是"按需"的判据）。
    const int idle_before = host.stub()->surface_half.presents;
    pumpUntil([] { return false; }, 150);
    EXPECT_EQ(host.stub()->surface_half.presents, idle_before) << "按需模式：没人输入就不出帧";

    host.control()->setContinuousRendering(true, 10);
    EXPECT_TRUE(pumpUntil([&] { return host.stub()->surface_half.presents >= idle_before + 3; }, 3000))
        << "连续渲染开着时应当自己出帧（不需要任何输入）";

    // 关掉：帧数必须停住。给足两倍以上的滴答周期，避免把"刚好在跑的那一帧"当成还在继续。
    host.control()->setContinuousRendering(false);
    const int stopped_at = host.stub()->surface_half.presents;
    pumpUntil([] { return false; }, 120);
    EXPECT_EQ(host.stub()->surface_half.presents, stopped_at) << "关掉之后不该再出帧";

    // 可以再开一次（幂等：重复调用不叠计时器、换周期按新的跑）。
    host.control()->setContinuousRendering(true, 5);
    EXPECT_TRUE(pumpUntil([&] { return host.stub()->surface_half.presents >= stopped_at + 2; }, 3000));
    host.control()->setContinuousRendering(false);
}

// initAsync() 不挂起 —— 它返回时 attach 已经结束，宿主的内容装配天然排在它之后，不需要任何等待。
TEST(RenderControlTest, InitAsyncCompletesWithoutSuspending)
{
    HostedControl host;
    host.show();

    bool done     = false;
    bool attached = false;

    // DetachedTask 是急启动的（`initial_suspend = suspend_never`）：这一句返回时整个 attach 已经跑完了。
    auto task = [](RenderControl* control, bool* done, bool* attached) -> vn::async::DetachedTask {
        *attached = co_await control->initAsync();
        *done     = true;
    }(host.control(), &done, &attached);
    static_cast<void>(task);

    EXPECT_TRUE(done) << "initAsync() 不该挂起：attach 就在调用它的那一段里做";
    EXPECT_TRUE(attached);
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Presenting);
    EXPECT_GT(host.stub()->initialize_calls, 0);
}

TEST(RenderControlTest, ReportsTheLifecycleThroughStateChanges){
    HostedControl host;

    std::vector<RenderControl::SurfaceState> seen;
    auto subscription = host.control()->state_changed.connect([&seen](RenderControl::SurfaceState state) {
        seen.push_back(state);
    });

    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    ASSERT_GE(seen.size(), 1u);
    // 一帧落进表面就上屏：中间没有“绑上了但还没画面”那一格。
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Presenting);
    EXPECT_TRUE(seen.size() <= 2u); // 没有重复转换
}

// 后端拒绝 ⇒ init() 报 false、状态停在 Pending，由宿主决定什么时候再试（控件不排重试）。
TEST(RenderControlTest, RefusedAttachReportsFalseAndWaitsForTheHost)
{
    HostedControl host;
    host.stub()->failures = 1000;
    host.show();

    EXPECT_FALSE(pumpUntil([&] { return host.control()->init(); }, 300));
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Pending);
    EXPECT_TRUE(host.control()->failureReason().empty());

    // 没有人在背后偷偷重试：不再调 init()，后端就不会再被碰。
    const int attempts = host.stub()->initialize_calls;
    pumpUntil([] { return false; }, 300);
    EXPECT_EQ(host.stub()->initialize_calls, attempts);
}

// 从拒绝里回来：宿主再调一次 init() 就行。
TEST(RenderControlTest, HostCanRetryAfterTheBackendRefused)
{
    HostedControl host;
    host.stub()->failures = 1000;
    host.show();
    EXPECT_FALSE(pumpUntil([&] { return host.control()->init(); }, 300));

    host.stub()->failures = 0;
    EXPECT_TRUE(host.control()->init());
    EXPECT_NE(host.control()->state(), RenderControl::SurfaceState::Pending);
}

// 没有可用的后端插件 ⇒ Failed（等下去也不会变），原因交给宿主。
TEST(RenderControlTest, ReportsFailedWhenNoRenderBackendIsRegistered)
{
    if (!vn::graphics::RenderBackendRegistry::instance().entries().empty()) {
        GTEST_SKIP() << "a render backend plugin is registered in this binary";
    }

    HostedControl host(/* with_backend */ false);
    host.show();

    EXPECT_FALSE(pumpUntil([&] { return host.control()->init(); }, 300));
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Failed);
    EXPECT_FALSE(host.control()->failureReason().empty());

    // 之后后端来了（插件晚加载 / 宿主自己设）：下一次 attach 就成功，而**旧判决要一起消失** ——
    // failureReason() 只在 Failed 那一格上是答案，会话起来了还留着它，宿主会一直显示一个早就不成立的失败原因。
    auto* late = new StubBackend();
    host.control()->engine()->setDevice(vn::intrusive_ptr<vn::graphics::RenderBackend>(late));

    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Presenting);
    EXPECT_TRUE(host.control()->failureReason().empty()) << "会话起来了，旧判决就该清掉";
}

// 没有会话就没有帧：attach 之前的 fitToScreen() / renderFrame() 不得把表面推到 Presenting —— Presenting 就是
// "有帧在面里"那一格，也是控件把渲染区域显示出来的依据（没有会话还显示，等于给宿主留一个空洞）。
TEST(RenderControlTest, AFrameAskedForBeforeAnySessionDoesNotPutTheSurfaceOnScreen)
{
    HostedControl host; // 有后端，但还没人 attach（构造只组装）
    host.show();

    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Pending);

    host.control()->fitToScreen();
    host.control()->renderFrame();

    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Pending) << "没有会话就不算有画面";
    EXPECT_FALSE(host.surface()->isVisible()) << "区域不该在没画面的时候显示出来";
    EXPECT_EQ(host.stub()->initialize_calls, 0) << "这两句也不该顺手 attach";

    // 同一句在 attach 之后就真的出帧了：区别只在会话在不在。
    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Presenting);
    EXPECT_GE(host.stub()->surface_half.presents, 1) << "attach 那一帧就是第一张画面";
}

// 一帧就是一帧：attach 就在调用它的那一段里出一帧，所以窗口还没 show 时 init() 也让表面就位 —— 判据是
// “有帧可看”，不是“窗口可见”。
TEST(RenderControlTest, TheFrameTheAttachRendersPutsTheSurfaceOnScreen)
{
    HostedControl host; // 布局好了，但窗口还没 show()

    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));

    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Presenting);
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_FALSE(host.surface()->isVisible()); // 顶层窗口还没显示，表面上不了屏

    // 上屏是窗口的事：窗口一显示，表面就在了（控件早就把区域标成可显示了）。
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.surface()->isVisible(); }, 3000));
}

// 宿主把控件丢进窗口后可以立刻 init()：此刻表面的尺寸还是退化值，真实尺寸等布局下落，
// 之后由 resize 路径把 swapchain 对齐过去 —— 没有任何定时器兜底，把这件事做成的是控件自己的事件
// （控件的 show 与容器的 resize）。
TEST(RenderControlTest, HostCanAttachRightAfterEmbeddingBeforeTheLayoutSettles)
{
    HostedControl host; // 控件在布局里，但窗口还没 show()
    host.show();

    // 没等布局/事件循环，直接 attach：设备与管线在这个同步调用里就建好了。
    EXPECT_TRUE(host.control()->init());
    EXPECT_GE(host.stub()->initialize_calls, 1);
    EXPECT_NE(host.control()->state(), RenderControl::SurfaceState::Pending);

    // 布局落下后尺寸被对齐：swapchain 按真实尺寸重建，随后首帧呈现。
    host.resizeWindow(640, 480);
    ASSERT_TRUE(pumpUntil([&] {
        return host.stub()->surface_half.width == host.surface()->width() && host.stub()->surface_half.height == host.surface()->height();
    }, 3000));
    EXPECT_GT(host.stub()->surface_half.width, 1);
    EXPECT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
}

// 平台窗口被重建（换屏 / reparent / 把 dock 拖出去）：控件的已建立会话自己把新句柄重新公告给
// 后端，宿主一次 init() 都不用调。
TEST(RenderControlTest, FollowsARecreatedSurfaceWithoutTheHost)
{
    HostedControl host;
    host.show();

    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    ASSERT_NE(host.surface(), nullptr);

    void* first_handle = host.stub()->surface_half.last_handle;
    ASSERT_NE(first_handle, nullptr);
    const int handle_calls_before = host.stub()->surface_half.handle_calls;

    std::vector<RenderControl::SurfaceState> seen;
    auto subscription = host.control()->state_changed.connect([&seen](RenderControl::SurfaceState state) {
        seen.push_back(state);
    });

    // 平台窗口重建，就是 RenderControl 钩子里的那三件事（换屏/reparent 在测试里无法按需触发）。
    QWindow* surface = host.surface();
    surface->destroy();
    surface->create();
    surface->show();

    ASSERT_TRUE(pumpUntil([&] {
        return host.stub()->surface_half.last_handle != first_handle
               && host.control()->state() == RenderControl::SurfaceState::Presenting;
    }, 3000));
    EXPECT_GT(host.stub()->surface_half.handle_calls, handle_calls_before);
    EXPECT_TRUE(surface->isVisible()); // 新窗口里重新落一帧之后才再上屏

    // 重建期间状态回到 Pending（窗口没了就不该声称"在出画面"），然后落一帧重新上屏。
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Pending);
    EXPECT_EQ(seen[1], RenderControl::SurfaceState::Presenting);
}

// 查看器（`RenderControl::viewer()`）是"这块区画什么"的**请求半边**：一条请求 = 一条链，建好、采纳、声明，全在
// 同一个引擎（同一个设备、同一个会话）上。一块区只画一幅画（多格已下线）—— 第二条呈现是被拒的。
TEST(RenderControlTest, TheViewerDrawsTheAreaFromItsRequest)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    auto* control = host.control();
    auto* viewer  = control->viewer();
    auto* surface = control->surface();
    ASSERT_NE(viewer, nullptr);
    ASSERT_NE(surface, nullptr);
    EXPECT_TRUE(viewer->isActive()) << "默认路径：这块区由查看器的视图呈现";
    EXPECT_EQ(viewer->view(), control->view());
    EXPECT_EQ(surface->presentedView(), control->view());

    const int sessions_before = host.stub()->initialize_calls;  // 设备 = 会话：换请求不该多一个设备

    // 一条请求：要坐标轴、不要读数。它建的链就采纳在视图上（并仍然是这块区的呈现）。
    vn::appfw::gui::ViewerOptions wanted;
    wanted.gizmo = true;
    ASSERT_TRUE(viewer->setViewerOptions(wanted));
    auto* pipeline = control->view()->pipeline();
    ASSERT_NE(pipeline, nullptr);
    EXPECT_NE(pipeline->gizmo(), nullptr) << "请求要的是坐标轴";
    EXPECT_EQ(pipeline->fpsOverlay(), nullptr) << "请求没有读数这一档";
    EXPECT_EQ(viewer->viewerOptions().gizmo, true) << "请求被记着：下一次重建从它开始";
    EXPECT_TRUE(viewer->isActive());

    // 一条链、一个设备：换请求是**同一个引擎**上重建，不是再来一份设备/会话。
    EXPECT_EQ(host.stub()->initialize_calls, sessions_before);

    // 一块区只画一幅画：再挂一个视图是被拒的（查看器不替它去抢这块区的呈现）。
    auto second = barePane(control->engine());
    EXPECT_FALSE(surface->present(*second));
    EXPECT_EQ(surface->presentedView(), control->view());
    control->renderFrame();
}

// 宿主自建 pass 就是"接管这块表面"（见 RenderSurface）：查看器一条都不声明（次序 = demo_plugin 那样，声明先落、
// attach 后到），已经上线之后再接管也立刻为 false —— 判据是表，不是查看器自己记的一个开关。
TEST(RenderControlTest, AHostPassTakesTheAreaAndTheViewerStandsDown)
{
    // 前半：声明先落、attach 后到 —— 查看器连链都不建（默认画面不许在主人的 pass 旁边再画一遍）。
    HostedControl host;
    auto*         control = host.control();
    auto*         surface = control->surface();
    auto*         view    = control->view();
    ASSERT_NE(control->viewer(), nullptr);

    vn::graphics::RenderPipelineBuilder builder(control->engine());
    builder.setCamera(view->camera());
    builder.setContent(view->scene());
    auto host_pipeline = builder.build({});
    ASSERT_NE(host_pipeline, nullptr);
    ASSERT_TRUE(surface->present(host_pipeline->windowPass()));

    host.show();
    ASSERT_TRUE(pumpUntil([&] { return control->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_FALSE(control->viewer()->isActive()) << "有宿主自建的 pass 在 ⇒ 查看器不是这块表面的呈现者";
    EXPECT_EQ(surface->presentedView(), nullptr) << "呈现者是宿主自建的 pass，没有视图";
    EXPECT_EQ(surface->presentingPass(), host_pipeline->windowPass());
    EXPECT_EQ(view->pipeline(), nullptr) << "主人自己搭 pass 时，默认链不该被建出来";

    // 后半：先让查看器上线，再接管 —— 一块区一次只画一幅画，所以接管是"查看器先放手，宿主再声明"（第二条呈现会被
    // 拒并说出来，这正是"谁在这块窗口上"只能有一个答案的意思）。
    HostedControl live;
    live.show();
    ASSERT_TRUE(pumpUntil([&] { return live.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_TRUE(live.control()->viewer()->isActive());

    auto* live_view = live.control()->view();
    vn::graphics::RenderPipelineBuilder live_builder(live.control()->engine());
    live_builder.setCamera(live_view->camera());
    live_builder.setContent(live_view->scene());
    auto taken_over = live_builder.build({});
    ASSERT_NE(taken_over, nullptr);
    EXPECT_FALSE(live.control()->surface()->present(taken_over->windowPass()))
        << "查看器还在呈现这块区：第二条呈现是被拒的";
    ASSERT_TRUE(live.control()->viewer()->release(live_view)) << "查看器放手";
    ASSERT_TRUE(live.control()->surface()->present(taken_over->windowPass()));
    EXPECT_FALSE(live.control()->viewer()->isActive()) << "接管之后：呈现者是宿主自建的 pass";
    EXPECT_EQ(live.control()->surface()->presentedView(), nullptr);
    EXPECT_EQ(live.control()->surface()->presentingPass(), taken_over->windowPass());
}

// release(view)：撤掉查看器为这个视图做的那条声明 —— 表里没有它，isActive() 也立刻为 false。还给宿主的只是这个
// 视图：它采纳的那条链仍是它的（链属于视图）。
TEST(RenderControlTest, ReleasingTheViewTakesItOutOfTheArea)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    auto* control = host.control();
    auto* viewer  = control->viewer();
    auto* surface = control->surface();
    auto  view    = vn::intrusive_ptr<vn::graphics::SceneView>(control->view());

    EXPECT_TRUE(viewer->isActive());
    ASSERT_TRUE(surface->hasPresentation());
    EXPECT_EQ(surface->presentedView(), view.get());

    EXPECT_TRUE(viewer->release(view.get())) << "这个视图是查看器的";
    EXPECT_EQ(viewer->view(), nullptr);
    EXPECT_FALSE(viewer->isActive()) << "撤掉的就是它那条声明";
    EXPECT_FALSE(surface->hasPresentation());
    EXPECT_EQ(surface->presentedView(), nullptr);
    EXPECT_NE(view->pipeline(), nullptr) << "链是视图的：还给宿主的是那个视图，不是它的画";
    EXPECT_FALSE(viewer->release(view.get())) << "已经不是查看器的视图了";
}

// 重建把运行期选择搬到新链（S7 的账，现在记在查看器的请求上）：档由请求记着（换档 = 改请求），两个 HUD 档的
// 可见性读**活着那条链** —— 使用者关掉的读数不许因重建复活。
TEST(RenderControlTest, ARebuildCarriesTheLiveChainsRuntimeChoices)
{
    HostedControl host;
    auto*         control = host.control();
    auto*         viewer  = control->viewer();
    ASSERT_NE(viewer, nullptr);

    vn::appfw::gui::ViewerOptions options;  // 延迟链 + 两个档都要：坐标轴 + 读数
    options.path = vn::graphics::ShadingPath::Deferred;
    options.gizmo = true;
    options.stats = true;
    ASSERT_TRUE(control->setViewerOptions(options));  // 控件的老入口 = 作用于第一格
    auto* first = control->view()->pipeline();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->shadingPath(), vn::graphics::ShadingPath::Deferred);
    ASSERT_NE(first->gizmo(), nullptr);
    ASSERT_NE(first->fpsOverlay(), nullptr);

    // 运行期：把读数关掉（零重建的开关）。
    ASSERT_TRUE(first->setStatsVisible(false));

    // 结构变化（非 Lit 档：延迟链表达不了它）⇒ 重建 —— 关掉的读数必须跟着走，没动过的坐标轴照旧亮着。
    vn::intrusive_ptr<vn::graphics::RenderPipeline> held(first);
    ASSERT_TRUE(viewer->setShadingMode(vn::graphics::ShadingMode::Unlit));
    auto* rebuilt = control->view()->pipeline();
    ASSERT_NE(rebuilt, nullptr);
    ASSERT_NE(rebuilt, held.get()) << "延迟链表达不了这一档 ⇒ 换档就是重建（链变前向）";
    EXPECT_EQ(rebuilt->shadingPath(), vn::graphics::ShadingPath::Forward);
    EXPECT_TRUE(rebuilt->isGizmoVisible()) << "没被动过的档照旧亮着";
    EXPECT_FALSE(rebuilt->isStatsVisible()) << "使用者关掉的读数不许跟着重建回来";

    // 档记在**请求**里：下一次从请求重建照样是那一档，运行期选择照样跟着搬。
    EXPECT_EQ(viewer->viewerOptions().mode, vn::graphics::ShadingMode::Unlit);
    EXPECT_EQ(viewer->viewerOptions().path, vn::graphics::ShadingPath::Deferred);
    ASSERT_TRUE(viewer->setViewerOptions(viewer->viewerOptions()));
    auto* again = control->view()->pipeline();
    ASSERT_NE(again, nullptr);
    ASSERT_NE(again, held.get());
    EXPECT_EQ(again->shadingMode(), vn::graphics::ShadingMode::Unlit);
    EXPECT_FALSE(again->isStatsVisible()) << "重建读的是活着的链，不是请求里的字段";
}

// 两格的路由与焦点（S8）已随多格一起下线；控件仍然向**设备**要这个窗口的目标，区持有它、引擎在帧尾经它呈现。
TEST(RenderControlTest, TheControlAsksItsDeviceForTheTargetOfTheWindowItGot)
{
    HostedControl host;
    auto*         control = host.control();
    ASSERT_TRUE(pumpUntil([&] { return control->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    auto* surface = control->surface();
    ASSERT_NE(surface, nullptr);
    ASSERT_NE(surface->target(), nullptr) << "区持有设备为这个窗口造的那把目标";
    EXPECT_EQ(surface->target(), &host.stub()->surface_half);
    EXPECT_EQ(host.stub()->target_calls, 1) << "只问一次：控件所在的这个窗口";
    EXPECT_EQ(host.stub()->surface_half.handle_calls, 1) << "窗口句柄是造目标时的输入";
    EXPECT_NE(surface->target()->nativeHandle(), nullptr) << "设备拿到的就是宿主这个窗口";

    // 一帧解析到的是**区**那把目标：引擎交给执行器的 present_to 就是它，帧尾的 present 也走它。
    control->renderFrame();
    ASSERT_FALSE(host.stub()->present_targets.empty());
    EXPECT_EQ(host.stub()->present_targets.back(), surface->target());
    EXPECT_GE(host.stub()->surface_half.presents, 1);
}

// 造不出目标就是挂不上：设备"不呈现"的答案（SurfaceTargetResult::NoPresentation）变成控件的失败理由，
// 而不是一个永远看不见的画面。
TEST(RenderControlTest, ADeviceThatPresentsNothingFailsTheAttachWithItsReason)
{
    HostedControl host;
    auto*         control = host.control();
    host.stub()->presents_nothing = true;  // 这台设备没有窗口那一半（构造只组装，所以这一句赶在 attach 前）

    ASSERT_TRUE(pumpUntil([&] { return control->state() == RenderControl::SurfaceState::Failed; }, 3000));
    EXPECT_EQ(host.stub()->target_calls, 1) << "控件向设备要过目标：它问的就是这个窗口";
    EXPECT_EQ(host.stub()->initialize_calls, 0) << "没有窗口就没有会话：设备不白起一次";
    EXPECT_EQ(host.stub()->surface_half.presents, 0);
    EXPECT_NE(control->failureReason().as_std_str().find("presents nothing"), std::string::npos)
        << "宿主读到的理由说了为什么（无呈现能力）";
}

// 内容掩码（`.ai/design/graphics-layers.md` 的 L1）在 appfw 这一侧的入口：`ViewerOptions::layer_mask` 原样进配方，
// 于是这条链的**内容** pass 都带它 —— 而 HUD 档（坐标轴 / 帧率读数）不受内容选择影响：藏内容不该把读者的工具
// 一起藏掉。这是"一块区画什么"的请求之一，所以它和 path / mode 一样是一次建链（见 ViewerOptions）。
TEST(RenderControlTest, TheViewerOptionsLayerMaskReachesTheContentPassesAndNotTheHud)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    auto* control = host.control();
    auto* viewer  = control->viewer();
    auto* engine  = control->engine();
    ASSERT_NE(viewer, nullptr);
    ASSERT_NE(engine, nullptr);
    EXPECT_EQ(viewer->viewerOptions().layer_mask, vn::graphics::kAllLayers) << "默认不选层：一切照旧";

    vn::appfw::gui::ViewerOptions wanted;
    wanted.layer_mask = 1u << 1;
    wanted.gizmo      = true;  // 两个 HUD 档，用来钉"内容选择不碰 UI"
    wanted.stats      = true;
    ASSERT_TRUE(viewer->setViewerOptions(wanted));
    EXPECT_EQ(viewer->viewerOptions().layer_mask, 1u << 1) << "请求被记着：下一次重建从它开始";

    std::size_t content_passes = 0;
    std::size_t hud_passes     = 0;
    for (const vn::graphics::PassPlan& pass : engine->framePlan().passes) {
        if (pass.tag == u8"Hud") {
            EXPECT_EQ(pass.layer_mask, vn::graphics::kAllLayers) << "HUD 是 UI，不受内容选择影响";
            ++hud_passes;
            continue;
        }
        if (pass.content != nullptr) {
            EXPECT_EQ(pass.layer_mask, 1u << 1) << "内容 pass 带的是配方的选择";
            ++content_passes;
        }
    }
    EXPECT_EQ(content_passes, 1u) << "前向路径只有那条窗口 pass 画内容";
    EXPECT_EQ(hud_passes, 2u) << "请求了坐标轴与读数，两条都在";
    control->renderFrame();
}


