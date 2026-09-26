// WindowPaintTest.cpp
//
// "The window is up" and "the window has painted" are DIFFERENT states, and the difference is what a user sees as a
// black window (or an invisible translucent frame) during a start-up.
//
// Why this is a test and not a comment: a boot never runs its event loop - it repaints and reports its progress
// directly - and Qt paints a window only once the window system's "it is visible now" notice has been dispatched from
// the event queue. Measured under WSLg on 2026-09-26: the main window's frame was mapped and viewable, sixteen
// progress updates were reported, and the window read back 0% painted for the whole boot; one dispatch (12-19 ms)
// turned that into 83.5%, and the frame's window went from fully transparent to 99.9% painted the same way.
// GuiApplication therefore shows its startup frame in startupStart() and waits for exactly this answer before the
// plugin load and the host's startup work (the boot goes on from that callback), and Qt's own splash screen pumps the
// queue for the same reason (QSplashScreen::repaint() calls processEvents(), documented as "even when there is no event
// loop present").
// See .ai/design/appfw-startup-splash.md.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QThread>
#include <QWidget>

#include <vine/appfw/gui/Window.hpp>

namespace guifw = vn::appfw::gui;

TEST(WindowPaintTest, APaintOfAnythingThatIsAlreadyInsideTheWindowCounts)
{
    QWidget        native;
    auto*          child = new QWidget(&native);
    guifw::Window window(&native, false);
    ASSERT_FALSE(window.hasPainted());

    // 顶层自己可能一个像素都不画（面积被不透明子控件盖住时就是这样），所以子控件的绘制也算。
    QEvent paint(QEvent::Paint);
    QCoreApplication::sendEvent(child, &paint);

    EXPECT_TRUE(window.hasPainted());
}

TEST(WindowPaintTest, APaintOfAWidgetThatArrivesLaterCounts)
{
    QWidget        native;
    guifw::Window window(&native, false);
    ASSERT_FALSE(window.hasPainted());

    // 窗口构造之后才加进来的子控件也要算：停靠面板、状态栏里的进度条就是启动期才出现的，
    // 它们是靠父控件的 ChildAdded 通知被接上的。
    auto*  late_child = new QWidget(&native);
    QEvent paint(QEvent::Paint);
    QCoreApplication::sendEvent(late_child, &paint);

    EXPECT_TRUE(window.hasPainted());
}

TEST(WindowPaintTest, TheFirstPaintIsReportedOnceThroughItsSignal)
{
    QWidget        native;
    guifw::Window window(&native, false);

    int reports = 0;
    const vn::Connection connection = window.first_paint.connect([&reports] { ++reports; });

    QEvent paint(QEvent::Paint);
    QCoreApplication::sendEvent(&native, &paint);
    EXPECT_TRUE(window.hasPainted());
    EXPECT_EQ(reports, 1);

    // 报的是"第一次画了"这个状态转移，不是每次绘制：等它的代码只该被叫醒一次。
    QCoreApplication::sendEvent(&native, &paint);
    EXPECT_EQ(reports, 1);
}

TEST(WindowPaintTest, AWindowHasNotPaintedUntilTheQueueHasBeenDispatched)
{
    QWidget        native;
    guifw::Window window(&native, false);

    // 还没 show：没有平台窗口，也就没有任何绘制。
    EXPECT_FALSE(window.hasPainted());

    int reports = 0;
    const vn::Connection connection = window.first_paint.connect([&reports] { ++reports; });

    window.show();

    // show() 只把窗口交给窗口系统；首帧是事件（信号），没派发队列就不会到。
    QElapsedTimer timer;
    timer.start();
    while (!window.hasPainted() && timer.elapsed() < 2000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }

    EXPECT_TRUE(window.hasPainted());
    EXPECT_EQ(reports, 1);
}
