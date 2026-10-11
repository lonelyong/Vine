#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <vector>

#include <QApplication>
#include <QMouseEvent>
#include <QPoint>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <vine/appfw/gui/RenderControl.hpp>

#include <vine/async/DetachedTask.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderSurface.hpp>
#include <vine/graphics/RenderTarget.hpp>
#include <vine/graphics/SceneView.hpp>

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
    bool initialize() override
    {
        ++initialize_calls;
        return initialize_calls > failures;
    }

    void shutdown() override { ++shutdown_calls; }

    void beginFrame() override {}
    void endFrame() override {}

    void setRenderTarget(vn::graphics::RenderTarget*) override {}

    void render(const std::vector<vn::graphics::RenderCommand>&, const vn::graphics::Camera*) override {}

    void swapBuffers() override { ++swaps; }

    void setWindowHandle(void* handle) override
    {
        last_handle = handle;
        ++handle_calls;
    }

    void resize(int w, int h) override
    {
        width  = w;
        height = h;
        ++resize_calls;
    }

  public:
    /// How many initialize() calls should report failure.
    int failures{ 0 };

    int   initialize_calls{ 0 };
    int   shutdown_calls{ 0 };
    int   handle_calls{ 0 };
    int   resize_calls{ 0 };
    int   swaps{ 0 };
    void* last_handle{ nullptr };
    int   width{ 0 };
    int   height{ 0 };
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
            control_->engine()->setBackend(vn::intrusive_ptr<vn::graphics::RenderBackend>(stub_));
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

} // namespace

// 控件自驱：宿主把后端装上、把窗口显示出来就够了 —— 不需要自己挑时机调 init()（它仍然是显式入口，幂等）。
TEST(RenderControlTest, AttachesItselfOnceTheBackendAndWindowAreThere)
{
    HostedControl host; // 控件先建、后端后装：这正是 SDK 里写的次序（engine()->setBackend() 在 init() 之前）
    host.show();

    // 窗口一显示（容器的 show ⇒ 控件自己的重试）会话就起来了，那一帧也落了进去。
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_NE(host.stub()->last_handle, nullptr);
    EXPECT_GT(host.stub()->width, 0);
    EXPECT_GT(host.stub()->height, 0);

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

    const std::vector<vn::graphics::RenderSurface::Presentation> table = surface->presentations();
    ASSERT_EQ(table.size(), 1u);
    EXPECT_EQ(table[0].view, control->view());

    // 表里那条 pass 就是该视图默认管线里那条窗口 pass（视图重建时表跟着走 —— 见 RenderSurfaceTest）。
    ASSERT_NE(control->view()->pipeline(), nullptr);
    EXPECT_EQ(table[0].pass, control->view()->pipeline()->windowPass());
    // 这块表面的**唯一一口清屏**落在那条 pass 上（默认画面照旧清成窗口灰）。
    EXPECT_TRUE(table[0].pass->isClearEnabled());

    // 同一块表面上再挂一个视图：它照样呈现，但清屏仍然只有第一条（表面说了算 —— 后端本来也只应用第一条公告，
    // 只是从前第二条会**静默**丢掉自己那个 clearColor）。
    auto right = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    right->setEngine(control->engine());
    EXPECT_TRUE(surface->present(*right));
    EXPECT_EQ(surface->presentations().size(), 2u);
    EXPECT_TRUE(table[0].pass->isClearEnabled());
    EXPECT_FALSE(right->pipeline()->windowPass()->isClearEnabled());
    control->renderFrame();
}

// 构造只组装：`new RenderControl()` 返回时**没有** attach（状态停在 Pending），配置写在它之后的同步代码里；
// 第一拍兜底才 attach —— 而兜底用的正是宿主刚装上的那个后端（不是注册表里的默认）。这就是"配置窗口"，
// 也是 SDK 写的次序（构造 ⇒ setBackend() ⇒ init()）不再依赖"构造那一刻注册表里有没有后端"的原因。
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
    EXPECT_EQ(host.control()->engine()->backend(), host.stub()); // 宿主自己装的后端赢，兜底只是补时机
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

// 多视图：一个表面、两个视图 —— 输入按"点"归视图。按下时路由、松开前捕获（`RenderControl::setInputRouter`）：
// 拖拽飞出自己那块矩形也继续由它接管，而 release 之后下一次按会重新路由。
TEST(RenderControlTest, TheInputRouterSendsEachDragToTheViewUnderTheCursor)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    host.resizeWindow(640, 480);

    auto* control = host.control();
    // 布局落地：控件拿到真实尺寸（路由按宽度对半分，0 宽会让这条用例失去意义）。
    ASSERT_TRUE(pumpUntil([&] { return control->width() > 0 && control->height() > 0; }, 3000));

    auto* own_view = control->view();
    ASSERT_NE(own_view, nullptr);

    // 右边那半：自己的视图 + 挂在**同一个引擎**上（视图属于引擎，不属于控件）+ 自己那条窗口管线。
    // 声明在这块表面上（`RenderSurface::present`）：它顺带把那条默认窗口管线建出来，同时"这块表面由谁画"
    // 有了唯一的记录（分屏的下一步 —— 矩形与输入路由 —— 都要读这张表）。
    auto right = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    right->setEngine(control->engine());
    control->surface()->present(*right);
    right->setViewport({ 0.5, 0.0, 0.5, 1.0 });
    right->onSurfaceResized(control->width(), control->height(), control->devicePixelRatio());

    control->setInputRouter([&](int x, int) { return x < control->width() / 2 ? own_view : right.get(); });

    QWindow* const surface = host.surface();
    ASSERT_NE(surface, nullptr);

    const auto send_press = [&](int x, int y) {
        QMouseEvent event(QEvent::MouseButtonPress, QPointF(x, y), surface->mapToGlobal(QPoint(x, y)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &event);
    };
    const auto send_move = [&](int x, int y) {
        QMouseEvent event(QEvent::MouseMove, QPointF(x, y), surface->mapToGlobal(QPoint(x, y)), Qt::NoButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &event);
    };
    const auto send_release = [&](int x, int y) {
        QMouseEvent event(QEvent::MouseButtonRelease, QPointF(x, y), surface->mapToGlobal(QPoint(x, y)),
                          Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(surface, &event);
    };
    const auto changed = [](const vn::math::Vec3d& a, const vn::math::Vec3d& b) {
        return (a - b).length() > 1.0e-9;
    };

    const vn::math::Vec3d own_before   = own_view->camera()->eye();
    const vn::math::Vec3d right_before = right->camera()->eye();

    // 在**右半**按下并拖动：只有右边那个视图的相机该动。
    send_press(control->width() * 3 / 4, control->height() / 2);
    send_move(control->width() * 3 / 4 + 40, control->height() / 2 + 20);
    // 中途"飞出"自己那一块（越过中线到左半一点点）：捕获仍然属于右边那个视图，拖拽不该断线。
    send_move(control->width() / 2 - 10, control->height() / 2 + 40);
    send_release(control->width() / 2 - 10, control->height() / 2 + 40);

    EXPECT_TRUE(changed(right->camera()->eye(), right_before)) << "右半按下 ⇒ 右边那个视图接管这次拖拽";
    EXPECT_TRUE(std::isfinite(right->camera()->eye().x) && std::isfinite(right->camera()->eye().y) &&
                std::isfinite(right->camera()->eye().z))
        << "接管之后那个相机仍然可用（拖过的距离再大也不该把它算成 NaN）";
    EXPECT_FALSE(changed(own_view->camera()->eye(), own_before)) << "左边那个视图不该收到这次拖拽";

    // 松手之后重新路由：这一次是**左半**，动的是控件自己那个视图（也让"捕获确实被释放"可观察）。
    const vn::math::Vec3d own_second = own_view->camera()->eye();
    send_press(control->width() / 8, control->height() / 2);
    send_move(control->width() / 8 + 40, control->height() / 2 + 20);
    send_release(control->width() / 8 + 40, control->height() / 2 + 20);

    EXPECT_TRUE(changed(own_view->camera()->eye(), own_second)) << "release 之后按左半 ⇒ 路由重新走一次";
    EXPECT_EQ(control->view(), own_view);
}

// 路由给出的视图**可以活不过这次拖拽**：控件在捕获期间持有它（借用 + 操作期间临时加引用，同一条通则）。
TEST(RenderControlTest, ACapturedViewSurvivesTheHostLettingGoOfIt)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    host.resizeWindow(640, 480);

    auto* control = host.control();
    ASSERT_TRUE(pumpUntil([&] { return control->width() > 0 && control->height() > 0; }, 3000));

    auto  right   = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    right->setEngine(control->engine());
    control->surface()->present(*right);
    right->setViewport({ 0.5, 0.0, 0.5, 1.0 });
    right->onSurfaceResized(control->width(), control->height(), control->devicePixelRatio());

    control->setInputRouter([&](int x, int) { return x < control->width() / 2 ? control->view() : right.get(); });

    QWindow* const surface = host.surface();
    ASSERT_NE(surface, nullptr);

    // 只留住相机（一个安全可读的观察点）：视图本身的引用马上就被宿主丢掉。
    auto camera_keep = vn::intrusive_ptr<vn::graphics::Camera>(right->camera());
    const vn::math::Vec3d before = camera_keep->eye();

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(control->width() * 3 / 4, 100),
                      surface->mapToGlobal(QPoint(control->width() * 3 / 4, 100)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(surface, &press);

    // 宿主丢掉自己的引用：拖拽还在进行，控件必须仍握着一个（引用计数在，视图就不死）。
    right = nullptr;

    QMouseEvent move(QEvent::MouseMove, QPointF(control->width() * 3 / 4 + 40, 120),
                     surface->mapToGlobal(QPoint(control->width() * 3 / 4 + 40, 120)), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QApplication::sendEvent(surface, &move);

    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(control->width() * 3 / 4 + 40, 120),
                        surface->mapToGlobal(QPoint(control->width() * 3 / 4 + 40, 120)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(surface, &release);

    // 视图活着 ⇒ 它的操纵器还在 ⇒ 这次拖动真的转了这个相机。视图若在 `right = nullptr` 那一刻就没了，
    // 下面这一句读到的要么是没被拖过的旧值（操纵器随视图一起消失），要么根本走不到这里。
    EXPECT_GT((camera_keep->eye() - before).length(), 1.0e-9)
        << "捕获期间控件必须替宿主握着这个视图（否则拖拽会作用在已释放的视图上）";
}

// 两个按钮各拖一个视图：松掉其中一个不能把另一个的拖拽也结束 —— 否则那个视图收不到自己的 release，
// 操纵器会卡在"正在拖"，之后**纯悬停**也会继续转相机（而没有任何键按下）。
TEST(RenderControlTest, ReleasingOneButtonDoesNotEndTheOtherButtonsDrag)
{
    HostedControl host;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    host.resizeWindow(640, 480);

    auto* control = host.control();
    ASSERT_TRUE(pumpUntil([&] { return control->width() > 0 && control->height() > 0; }, 3000));

    auto own   = vn::intrusive_ptr<vn::graphics::SceneView>(control->view());
    auto right = vn::intrusive_ptr<vn::graphics::SceneView>(new vn::graphics::SceneView());
    right->setEngine(control->engine());
    control->surface()->present(*right);
    right->setViewport({ 0.5, 0.0, 0.5, 1.0 });
    right->onSurfaceResized(control->width(), control->height(), control->devicePixelRatio());

    control->setInputRouter([&](int x, int) { return x < control->width() / 2 ? own.get() : right.get(); });

    QWindow* const surface = host.surface();
    ASSERT_NE(surface, nullptr);
    const auto send = [&](QEvent::Type type, int x, int y, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(x, y), surface->mapToGlobal(QPoint(x, y)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(surface, &event);
    };
    const auto moved = [](const vn::math::Vec3d& a, const vn::math::Vec3d& b) { return (a - b).length() > 1.0e-9; };

    const int half = control->width() / 2;

    // 左键在**左半**按下（拖控件自己那个视图）⇒ 左边的相机跟着动。
    send(QEvent::MouseButtonPress, half / 2, 120, Qt::LeftButton, Qt::LeftButton);
    const vn::math::Vec3d own_start = own->camera()->eye();
    send(QEvent::MouseMove, half / 2 + 30, 140, Qt::NoButton, Qt::LeftButton);
    ASSERT_TRUE(moved(own->camera()->eye(), own_start)) << "左半按下并拖动 ⇒ 左边那个视图接管这次拖拽";

    // 右键在**右半**按下（第二次拖拽：平移右边那个视图），再松开右键。
    const vn::math::Vec3d right_before = right->camera()->eye();
    send(QEvent::MouseButtonPress, half + half / 2, 120, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
    send(QEvent::MouseMove, half + half / 2 + 30, 140, Qt::NoButton, Qt::LeftButton | Qt::RightButton);
    send(QEvent::MouseButtonRelease, half + half / 2 + 30, 140, Qt::RightButton, Qt::LeftButton);
    EXPECT_TRUE(moved(right->camera()->eye(), right_before)) << "右键的拖拽该动右边那个视图（不是左边那个）";

    // 左键在**右半**松开（指针漂到别处了）：它必须还给自己那次拖拽的视图，而不是按位置送给右边那个。
    send(QEvent::MouseButtonRelease, half + 10, 140, Qt::LeftButton, Qt::NoButton);

    // 两次拖拽都结束了 ⇒ 之后没有任何键按下的悬停不许再动相机（左边那个如果没收到的 release，就会继续转）。
    const vn::math::Vec3d settled = own->camera()->eye();
    send(QEvent::MouseMove, half / 2 + 60, 160, Qt::NoButton, Qt::NoButton);
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
    const int idle_before = host.stub()->swaps;
    pumpUntil([] { return false; }, 150);
    EXPECT_EQ(host.stub()->swaps, idle_before) << "按需模式：没人输入就不出帧";

    host.control()->setContinuousRendering(true, 10);
    EXPECT_TRUE(pumpUntil([&] { return host.stub()->swaps >= idle_before + 3; }, 3000))
        << "连续渲染开着时应当自己出帧（不需要任何输入）";

    // 关掉：帧数必须停住。给足两倍以上的滴答周期，避免把"刚好在跑的那一帧"当成还在继续。
    host.control()->setContinuousRendering(false);
    const int stopped_at = host.stub()->swaps;
    pumpUntil([] { return false; }, 120);
    EXPECT_EQ(host.stub()->swaps, stopped_at) << "关掉之后不该再出帧";

    // 可以再开一次（幂等：重复调用不叠计时器、换周期按新的跑）。
    host.control()->setContinuousRendering(true, 5);
    EXPECT_TRUE(pumpUntil([&] { return host.stub()->swaps >= stopped_at + 2; }, 3000));
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
    host.control()->engine()->setBackend(vn::intrusive_ptr<vn::graphics::RenderBackend>(late));

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
    EXPECT_GE(host.stub()->swaps, 1) << "attach 那一帧就是第一张画面";
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
        return host.stub()->width == host.surface()->width() && host.stub()->height == host.surface()->height();
    }, 3000));
    EXPECT_GT(host.stub()->width, 1);
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

    void* first_handle = host.stub()->last_handle;
    ASSERT_NE(first_handle, nullptr);
    const int handle_calls_before = host.stub()->handle_calls;

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
        return host.stub()->last_handle != first_handle
               && host.control()->state() == RenderControl::SurfaceState::Presenting;
    }, 3000));
    EXPECT_GT(host.stub()->handle_calls, handle_calls_before);
    EXPECT_TRUE(surface->isVisible()); // 新窗口里重新落一帧之后才再上屏

    // 重建期间状态回到 Pending（窗口没了就不该声称"在出画面"），然后落一帧重新上屏。
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Pending);
    EXPECT_EQ(seen[1], RenderControl::SurfaceState::Presenting);
}
