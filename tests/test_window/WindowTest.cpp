#include <gtest/gtest.h>

#include <vine/window/InputEvent.hpp>
#include <vine/window/KeyCode.hpp>
#include <vine/window/MouseButton.hpp>
#include <vine/window/WindowContext.hpp>
#include <vine/window/WindowEvent.hpp>

#include <vine/String.hpp>

using vn::String;
using vn::testFlag;
using vn::window::KeyCode;
using vn::window::KeyEvent;
using vn::window::ModifierKey;
using vn::window::MouseButton;
using vn::window::MouseEvent;
using vn::window::ScrollEvent;
using vn::window::WindowContext;
using vn::window::WindowEvent;

namespace
{

/**
 * @brief A fake native window the wrapper queries.
 *
 * Stands in for a real OS/GUI window (HWND, vsg::Window, ...) whose live state
 * the WindowContext wrapper exposes through its overridden accessors.
 */
struct FakeNativeWindow {
    String title;
    int    width = 0;
    int    height = 0;
    bool   visible = false;
    void*  handle = nullptr;
};

/**
 * @brief A WindowContext wrapper holding a pointer to a native window.
 *
 * Mirrors how a real backend wrapper (e.g. a VSG or Qt based wrapper) queries
 * the underlying window: it stores the native window pointer and overrides
 * the read-only accessors to forward the live state.
 */
class TestWindowContext : public WindowContext {
    VN_OBJECT_META_DECL;

  public:
    explicit TestWindowContext(FakeNativeWindow* native)
      : native_(native)
    {
    }
    ~TestWindowContext() override = default;

  public:
    String title() const override { return native_ != nullptr ? native_->title : String(); }
    int width() const override { return native_ != nullptr ? native_->width : 0; }
    int height() const override { return native_ != nullptr ? native_->height : 0; }
    bool isVisible() const override { return native_ != nullptr && native_->visible; }
    void* nativeHandle() const override { return native_ != nullptr ? native_->handle : nullptr; }

  private:
    FakeNativeWindow* native_ = nullptr;
};

}  // namespace

VN_OBJECT_META_IMPL(TestWindowContext, vn::window::WindowContext);

// ============ Read-only queries (from the native window) ============

TEST(WindowContextTest, DefaultsFromNativeWindow)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    EXPECT_TRUE(context.title().empty());
    EXPECT_EQ(context.width(), 0);
    EXPECT_EQ(context.height(), 0);
    EXPECT_FALSE(context.isVisible());
    EXPECT_EQ(context.nativeHandle(), nullptr);
}

TEST(WindowContextTest, QueriesForwardNativeState)
{
    FakeNativeWindow native;
    native.title = u8"Render View";
    native.width = 800;
    native.height = 600;
    native.visible = true;
    int fake_handle = 0x1234;
    native.handle = &fake_handle;

    TestWindowContext context(&native);
    EXPECT_EQ(context.title(), u8"Render View");
    EXPECT_EQ(context.width(), 800);
    EXPECT_EQ(context.height(), 600);
    EXPECT_TRUE(context.isVisible());
    EXPECT_EQ(context.nativeHandle(), &fake_handle);
}

TEST(WindowContextTest, NullNativeWindowYieldsDefaults)
{
    TestWindowContext context(nullptr);
    EXPECT_TRUE(context.title().empty());
    EXPECT_EQ(context.width(), 0);
    EXPECT_EQ(context.height(), 0);
    EXPECT_FALSE(context.isVisible());
    EXPECT_EQ(context.nativeHandle(), nullptr);
}

// ============ Resize signal ============

TEST(WindowContextTest, ResizeSignalFires)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    int captured_w = 0;
    int captured_h = 0;
    context.resized.connect([&](int w, int h) {
        captured_w = w;
        captured_h = h;
    }).detach();

    context.resized.trigger(640, 480);
    EXPECT_EQ(captured_w, 640);
    EXPECT_EQ(captured_h, 480);
}

// ============ Key event ============

TEST(WindowContextTest, KeyEventDispatched)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    KeyCode received = KeyCode::Unknown;
    context.key.connect([&](const KeyEvent& e) { received = e.code; }).detach();

    KeyEvent e;
    e.code = KeyCode::Escape;
    e.pressed = true;
    context.postEvent(e);
    EXPECT_EQ(received, KeyCode::Escape);
}

TEST(WindowContextTest, KeyEventModifiers)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    ModifierKey received = ModifierKey::None;
    context.key.connect([&](const KeyEvent& e) { received = e.modifiers; }).detach();

    KeyEvent e;
    e.code = KeyCode::A;
    e.modifiers = ModifierKey::Shift | ModifierKey::Control;
    context.postEvent(e);
    EXPECT_TRUE(testFlag(received, ModifierKey::Shift));
    EXPECT_TRUE(testFlag(received, ModifierKey::Control));
}

// ============ Mouse event ============

TEST(WindowContextTest, MouseEventDispatched)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    MouseEvent received;
    bool fired = false;
    context.mouse.connect([&](const MouseEvent& e) {
        received = e;
        fired    = true;
    }).detach();

    MouseEvent e;
    e.button = MouseButton::Left;
    e.x = 12.0;
    e.y = 34.0;
    e.pressed = true;
    context.postEvent(e);
    EXPECT_TRUE(fired);
    EXPECT_EQ(received.button, MouseButton::Left);
    EXPECT_DOUBLE_EQ(received.x, 12.0);
    EXPECT_DOUBLE_EQ(received.y, 34.0);
    EXPECT_TRUE(received.pressed);
}

// ============ Scroll event ============

TEST(WindowContextTest, ScrollEventDispatched)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    ScrollEvent received;
    context.scroll.connect([&](const ScrollEvent& e) { received = e; }).detach();

    ScrollEvent e;
    e.deltaY = 3.0;
    context.postEvent(e);
    EXPECT_DOUBLE_EQ(received.deltaY, 3.0);
}

// ============ WindowEvent ============

TEST(WindowContextTest, WindowEventDispatched)
{
    FakeNativeWindow native;
    TestWindowContext context(&native);
    WindowEvent::Type received = WindowEvent::Type::Resize;
    context.windowEvent.connect([&](const WindowEvent& e) { received = e.type; }).detach();

    WindowEvent e;
    e.type = WindowEvent::Type::FocusIn;
    context.postEvent(e);
    EXPECT_EQ(received, WindowEvent::Type::FocusIn);
}
