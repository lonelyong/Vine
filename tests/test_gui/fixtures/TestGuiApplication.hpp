#pragma once

#include <vine/appfw/gui/GuiApplication.hpp>

/**
 * @brief test_gui 共享的 GUI 应用：把受保护的启动收尾钩子公开给用例。
 *
 * `Application::startupEnd()` 是启动阶段三拍里的最后一拍，而且**受保护**：收尾是框架的动作，宿主不从外面插手
 * 到一次正在跑的启动里去。本进程从不跑循环（用例要它活着，不能被 `exec()` 占住），所以启动得由用例手动收掉——
 * 这也是"叶子自己驱动启动时怎么做"的活例，所以在这里用 using 把它提上来。
 *
 * 它是**懒**任务（`vn::async::Task` 挂起在第一行之前），所以用例要 `.result()` 把它驱动完——
 * 只在那几拍不会真挂起时才安全（`GuiApplication::startupEnd()` 就是全同步的那一拍）。
 */
class TestGuiApplication : public vn::appfw::gui::GuiApplication {
  public:
    using GuiApplication::GuiApplication;

    using GuiApplication::startupEnd;
};
