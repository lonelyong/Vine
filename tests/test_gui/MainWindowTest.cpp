// MainWindowTest.cpp
//
// The main window's OPENING SIZE is a STATED default, not something the layout decides.
//
// Why this is a test and not a comment: without an explicit size Qt sizes the first show from the
// layout's size hint - and what this window's hint follows is its surroundings (ribbon, docks, their
// content). Measured on 2026-09-25: six starts of the demo produced six different render areas
// (378x247, 1037x509, 491x319, 378x406, 558x247 and one gate run 3418x1110). The gate states the
// judged size itself because of that (scripts/vsg_rewrite_gate.sh, "the app's own start-up size is
// NOT deterministic"); this window states 800x600 - the documented default, also its minimum.
// See .ai/design/vsg-reimplementation.md §11.16dc.
//
// The process shares one GuiApplication (see test_gui.cpp's global environment); this file builds its
// own MainWindow, as the GuiTest fixture does, and never touches the shared one.

#include <gtest/gtest.h>

#include <QSize>
#include <QWidget>

#include <vine/appfw/gui/MainWindow.hpp>

namespace guifw = vn::appfw::gui;

TEST(MainWindowTest, TheOpeningSizeIsStatedInsteadOfInheritedFromTheLayout)
{
    auto wnd = std::make_unique<guifw::MainWindow>();
    QWidget* widget = wnd->impl<QWidget>();
    ASSERT_NE(widget, nullptr);
    // The stated default, checked BEFORE any show(): without the constructor's resize() this is the
    // Qt default 640x480 and WA_Resized is unset - the exact state in which the first show falls back
    // to the layout's size hint. (The hint-following itself does not reproduce on this host's two
    // platforms today - neither offscreen nor xcb grew the window with a long console line - so what
    // this pins is the constructor's decision, which is the mechanism the app-level measurement is
    // about.)
    EXPECT_EQ(widget->minimumSize(), QSize(800, 600));
    EXPECT_EQ(widget->size(), QSize(800, 600));
    EXPECT_TRUE(widget->testAttribute(Qt::WA_Resized));
}
