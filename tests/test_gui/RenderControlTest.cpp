#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <thread>
#include <vector>

#include <QApplication>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <vine/appfw/gui/RenderControl.hpp>
#include <vine/graphics/Camera.hpp>
#include <vine/graphics/RenderBackend.hpp>
#include <vine/graphics/RenderBackendRegistry.hpp>
#include <vine/graphics/RenderCommand.hpp>
#include <vine/graphics/RenderEngine.hpp>
#include <vine/graphics/RenderTarget.hpp>

namespace
{

using vine::appfw::gui::RenderControl;

/// Backend stub.
///
/// The surface lifecycle is about *when* the control attaches, not about pixels, so what the
/// control asks of the backend is all the test needs: the handle it was handed, the size it was
/// told, and how often initialize() was called. A stub also makes the failure paths reachable
/// without a GPU, which is what pins "retries, then gives up" and "never attaches to an empty
/// surface".
class StubBackend : public vine::graphics::RenderBackend
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

    void setRenderTarget(vine::graphics::RenderTarget*) override {}

    void render(const std::vector<vine::graphics::RenderCommand>&, const vine::graphics::Camera*) override {}

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
            control_->engine()->setBackend(vine::intrusive_ptr<vine::graphics::RenderBackend>(stub_));
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

// 控件不自驱：窗口显示、布局落下之后，没人调 init() 就一直是 Pending（后端一次都没被碰过）。
TEST(RenderControlTest, NothingAttachesBeforeTheHostAsks)
{
    HostedControl host;
    host.show();

    EXPECT_FALSE(pumpUntil([&] { return host.control()->state() != RenderControl::SurfaceState::Pending; }, 300));
    EXPECT_EQ(host.stub()->initialize_calls, 0);
    ASSERT_NE(host.surface(), nullptr);
    EXPECT_FALSE(host.surface()->isVisible()); // 还没绑上，表面就不该占屏幕

    // 宿主给出时机：一次 init() 就 attach。控件已经在屏幕上，所以这一帧也在 init() 里落进表面。
    EXPECT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_NE(host.stub()->last_handle, nullptr);
    EXPECT_GT(host.stub()->width, 0);
    EXPECT_GT(host.stub()->height, 0);
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Presenting);

    // 表面跟着首帧上屏：显示的窗口期里它一直是“有东西可看”的，不再是“一个还没画过东西的原生窗口”。
    ASSERT_NE(host.surface(), nullptr);
    EXPECT_TRUE(host.surface()->isVisible());
}

// 状态序列是 Pending -> Attached -> Presenting，且每个转换只报一次。
TEST(RenderControlTest, ReportsTheLifecycleThroughStateChanged)
{
    HostedControl host;

    std::vector<RenderControl::SurfaceState> seen;
    auto subscription = host.control()->stateChanged.connect([&seen](RenderControl::SurfaceState state) {
        seen.push_back(state);
    });

    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    ASSERT_GE(seen.size(), 2u);
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Attached);
    EXPECT_EQ(seen[1], RenderControl::SurfaceState::Presenting);
    EXPECT_TRUE(seen.size() <= 3u); // 没有重复转换
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
    if (!vine::graphics::RenderBackendRegistry::instance().entries().empty()) {
        GTEST_SKIP() << "a render backend plugin is registered in this binary";
    }

    HostedControl host(/* with_backend */ false);
    host.show();

    EXPECT_FALSE(pumpUntil([&] { return host.control()->init(); }, 300));
    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Failed);
    EXPECT_FALSE(host.control()->failureReason().empty());
}

// 控件还没上屏就能先把后端热起来（attach 只需要句柄+尺寸），但表面一直不在屏幕上：
// 本机推不动帧（推了也没人看得见），而没有帧的表面就是一块洞，不能露。
TEST(RenderControlTest, WarmsUpWhileInvisibleAndPutsTheSurfaceOnScreenWithItsFirstFrame)
{
    HostedControl host; // 布局好了，但窗口还没 show()

    ASSERT_TRUE(pumpUntil([&] { return host.control()->init(); }, 3000));

    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Attached); // 看不到 ⇒ 不到 Presenting
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_FALSE(host.surface()->isVisible()); // 没有帧就不上屏

    // 上屏本身就是那个事件：窗口一显示，控件自己的 show 事件把首帧做出来，表面随之出现。
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
    EXPECT_TRUE(host.surface()->isVisible());
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
    auto subscription = host.control()->stateChanged.connect([&seen](RenderControl::SurfaceState state) {
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

    // 重建期间状态回到 Pending（窗口没了就不该声称"在出画面"），然后重新走一遍 Attached -> Presenting。
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Pending);
    EXPECT_EQ(seen[1], RenderControl::SurfaceState::Attached);
    EXPECT_EQ(seen[2], RenderControl::SurfaceState::Presenting);
}
