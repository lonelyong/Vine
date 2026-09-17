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

/// Hosts a RenderControl in a shown window with a stub backend.
class HostedControl
{
  public:
    HostedControl()
    {
        layout_.reset(new QVBoxLayout(&window_));
        control_ = new RenderControl();
        layout_->addWidget(control_->impl<QWidget>());

        stub_ = new StubBackend();
        control_->engine()->setBackend(vine::intrusive_ptr<vine::graphics::RenderBackend>(stub_));

        window_.resize(320, 240);
    }

    void show() { window_.show(); }

    RenderControl* control() const { return control_; }
    StubBackend*   stub() const { return stub_; }

  private:
    QWidget                 window_;
    std::unique_ptr<QVBoxLayout> layout_;
    RenderControl*          control_{ nullptr };
    StubBackend*            stub_{ nullptr };
};

/// The control's native surface.
///
/// Recovered the way RenderControl itself recovers it (windowHandle() on a non-top-level
/// container returns null): the host widget carries the surface pointer as a property.
///
/// @param control Control to inspect.
/// @return The render surface.
QWindow* surfaceOf(RenderControl* control)
{
    return static_cast<QWindow*>(control->impl<QWidget>()->property("_vine_surface").value<void*>());
}

} // namespace

// 默认（自驱）：控件在布局完成后自己 attach——不需要宿主猜延迟、也不需要调 init()。
TEST(RenderControlTest, AttachesByItselfOnceTheWidgetIsLaidOut)
{
    HostedControl host;
    host.show();

    EXPECT_TRUE(pumpUntil([&] { return host.control()->state() != RenderControl::SurfaceState::Pending; }, 3000));

    const auto state = host.control()->state();
    EXPECT_TRUE(state == RenderControl::SurfaceState::Attached || state == RenderControl::SurfaceState::Presenting);
    EXPECT_GT(host.stub()->initialize_calls, 0);
    EXPECT_NE(host.stub()->last_handle, nullptr);
    EXPECT_GT(host.stub()->width, 0);
    EXPECT_GT(host.stub()->height, 0);

    // 表面只在后端绑上之后才显示：显示的窗口期不再是"一个还没画过东西的原生窗口"。
    EXPECT_TRUE(surfaceOf(host.control())->isVisible());
}

// 状态序列是 Pending -> Attached -> Presenting，且每个转换只报一次。
TEST(RenderControlTest, ReportsTheLifecycleThroughStateChanged)
{
    HostedControl host;

    std::vector<RenderControl::SurfaceState> seen;
    auto subscription = host.control()->stateChanged.subscribe([&seen](RenderControl::SurfaceState state) {
        seen.push_back(state);
    });

    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));

    ASSERT_GE(seen.size(), 2u);
    EXPECT_EQ(seen[0], RenderControl::SurfaceState::Attached);
    EXPECT_EQ(seen[1], RenderControl::SurfaceState::Presenting);
    EXPECT_TRUE(seen.size() <= 3u); // 没有重复转换
}

// 关掉自驱：宿主自己掌握时机，控件不会自作主张。
TEST(RenderControlTest, AutoInitializeOffWaitsForTheHost)
{
    HostedControl host;
    host.control()->setAutoInitialize(false);
    host.show();

    // 布局完成、窗口也显示了，但没人让控件 attach。
    EXPECT_FALSE(pumpUntil([&] { return host.control()->state() != RenderControl::SurfaceState::Pending; }, 300));
    EXPECT_EQ(host.stub()->initialize_calls, 0);
    EXPECT_FALSE(surfaceOf(host.control())->isVisible()); // 还没绑上，表面就不该占屏幕

    EXPECT_TRUE(host.control()->init());
    EXPECT_NE(host.control()->state(), RenderControl::SurfaceState::Pending);
    EXPECT_GT(host.stub()->initialize_calls, 0);
}

// 后端一直拒绝 ⇒ 有界重试后转 Failed，并把原因交出来（不是无限重试、也不是永远 Pending）。
TEST(RenderControlTest, RetriesThenGivesUpWhenTheBackendKeepsRefusing)
{
    HostedControl host;
    host.stub()->failures = 1000;
    host.show();

    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Failed; }, 5000));

    // 一次"隐藏表面被拒"的探测（平台回退，不计入失败）+ kMaxAttachFailures 次真失败。
    EXPECT_LE(host.stub()->initialize_calls, 4);
    EXPECT_GE(host.stub()->initialize_calls, 3);
    EXPECT_FALSE(host.control()->failureReason().empty());

    // 放弃之后不再骚扰后端。
    const int attempts = host.stub()->initialize_calls;
    pumpUntil([] { return false; }, 300);
    EXPECT_EQ(host.stub()->initialize_calls, attempts);
}

// 从失败里回来：宿主显式 init() 会重新开始一轮预算。
TEST(RenderControlTest, HostCanRetryAfterFailure)
{
    HostedControl host;
    host.stub()->failures = 1000;
    host.show();
    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Failed; }, 5000));

    host.stub()->failures = 0;
    EXPECT_TRUE(host.control()->init());
    EXPECT_NE(host.control()->state(), RenderControl::SurfaceState::Failed);
}

// 窗口还没显示就能先把后端热起来（attach 只需要句柄+尺寸），但不会往还看不到的表面里 present。
TEST(RenderControlTest, WarmsUpWhileInvisibleAndPresentsOnlyWhenShown)
{
    HostedControl host; // 布局好了，但窗口还没 show()

    ASSERT_TRUE(pumpUntil([&] { return host.control()->state() != RenderControl::SurfaceState::Pending; }, 3000));

    EXPECT_EQ(host.control()->state(), RenderControl::SurfaceState::Attached); // 看不到 ⇒ 不到 Presenting
    EXPECT_GT(host.stub()->initialize_calls, 0);
    // 注意：这里不能断言 surface->isVisible() 为假——attach 成功就会把"显示表面"的意图打开
    // （Qt 因为顶层窗口没显示而不会真的映射它）；"还没有画出去"由状态停在 Attached 表达。

    host.show();
    EXPECT_TRUE(pumpUntil([&] { return host.control()->state() == RenderControl::SurfaceState::Presenting; }, 3000));
}
