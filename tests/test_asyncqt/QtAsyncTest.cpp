#include <vine/async/Task.hpp>
#include <vine/async/DetachedTask.hpp>
#include <vine/async/Cancellation.hpp>
#include <vine/async/Sleep.hpp>

#include <vine/CancellationToken.hpp>
#include <vine/appfw/MainThreadDispatcher.hpp>

#include "async/Scheduler.hpp"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>

using namespace vn;

namespace {

class QtAsyncTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        static int argc = 1;
        static char arg0[] = "test_asyncqt";
        static char* argv[] = { arg0, nullptr };
        app_ = std::make_unique<QCoreApplication>(argc, argv);
    }

    static void TearDownTestSuite()
    {
        app_.reset();
    }

    static std::unique_ptr<QCoreApplication> app_;
};

std::unique_ptr<QCoreApplication> QtAsyncTest::app_;

} // namespace

TEST_F(QtAsyncTest, SchedulerResumesOnOwningThread)
{
    appfw::async::Scheduler scheduler;
    bool ran = false;

    auto task = [&]() -> async::DetachedTask {
        co_await scheduler.schedule();
        ran = true;
        QCoreApplication::quit();
        co_return;
    }();

    QTimer::singleShot(5000, [] { QCoreApplication::quit(); });
    QCoreApplication::instance()->exec();

    EXPECT_TRUE(ran);
}

// 命令在任意线程恢复后要碰 UI 就得先回到应用线程：resumeOnMainThread() 把这段回归
// 变成一行 co_await（回调式的 postToMainThread() 在协程里写起来很别扭）。
//
// 这里刻意走 sleepFor()：它在 base 的进程级定时器线程上恢复协程，于是到达那个
// co_await 时确实不在应用线程上——正是命令里真实的形态。协程本身在应用线程创建，
// 且创建它的帧在 exec() 返回前一直活着（不能在会退出的线程上创建并挂起协程：
// 创建线程一退，帧所在的栈就没了，之后任何线程去恢复都是踩空）。
TEST_F(QtAsyncTest, MainThreadDispatcherResumesOnApplicationThread)
{
    appfw::MainThreadDispatcher dispatcher;
    QThread* const            app_thread = QCoreApplication::instance()->thread();

    bool ran         = false;
    bool off_app     = false;
    bool back_on_app = false;

    auto task = [&]() -> async::DetachedTask {
        co_await async::sleepFor(std::chrono::milliseconds(1));
        off_app = (QThread::currentThread() != app_thread);

        co_await dispatcher.resumeOnMainThread();
        back_on_app = (QThread::currentThread() == app_thread);
        ran         = true;
        QCoreApplication::quit();
        co_return;
    }();

    QTimer::singleShot(5000, [] { QCoreApplication::quit(); });
    QCoreApplication::instance()->exec();

    EXPECT_TRUE(ran);
    EXPECT_TRUE(off_app);
    EXPECT_TRUE(back_on_app);
}
