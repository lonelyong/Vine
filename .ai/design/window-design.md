# window 模块设计（跨平台窗口上下文抽象）

> 状态：设计稿 v3（2026-09-02）
>
> **定位**：`window` 是 base 层的一个**平台无关窗口数据上下文**模块。
> 它定义跨平台的事件类型（键码/鼠标/滚轮/窗口事件）和 `WindowContext`
> **只读抽象接口**，**不负责创建/销毁窗口**。窗口由外部宿主（GUI shell、OS、
> 测试）创建；派生类持有实际窗口指针并重写只读查询；渲染层
> （RenderEngine/CameraManipulator/AppFramework）通过该接口 + Signal 订阅消费。

## 1. 架构定位

```
┌────────────────────────────────────────────┐
│ base/window（接口层，仅依赖 Core/Global）    │
│  KeyCode / MouseButton / ModifierKey       │
│  KeyEvent / MouseEvent / ScrollEvent       │
│  WindowEvent                               │
│  WindowContext（只读抽象接口 + Signal）      │
└───────────────────┬────────────────────────┘
                    │ 派生类包装真实窗口
      ┌─────────────┴──────────────┐
      │                            │
┌─────▼─────────┐          ┌───────▼────────┐
│ 派生窗口包装器  │          │ 渲染层消费      │
│ (持实际窗口指针,│          │ RenderEngine   │
│  重写只读查询)  │          │ CameraManipul. │
└───────────────┘          └────────────────┘
```

原则：
- `window` **只依赖** `vn::Core`（Signal/Object/RefCounted/intrusive_ptr）与 `vn::Global`（math）
- `WindowContext` 是**只读抽象接口**：不创建/销毁/轮询/存储窗口；只暴露查询 + 转发事件
- 派生类持实际窗口指针，重写 `title()/width()/height()/isVisible()/nativeHandle()` 查询实时状态
- 窗口真正创建/销毁/事件轮询全部在外部宿主；宿主通过 `postEvent()` 注入事件
- 上层只依赖抽象类型与 Signal

## 2. 模块结构

```
src/base/window/
  CMakeLists.txt            # 只链 vn::Core vn::Global
  sdk/vine/window/
    window_global.hpp       # API export 宏 + VN_WINDOW_NS 命名空间
    KeyCode.hpp             # 平台无关键码枚举
    MouseButton.hpp         # 鼠标按钮枚举 + ModifierKey 位标志
    InputEvent.hpp          # KeyEvent / MouseEvent / ScrollEvent
    WindowEvent.hpp         # 窗口生命周期事件（Resize/Close/Focus/...）
    WindowContext.hpp       # 数据载体 + Signal 事件 + postEvent 入口
  src/
    WindowContext.cpp       # 数据状态与 Signal 触发
```

## 3. 核心类设计

### 3.1 事件类型（平台无关，宿主负责映射）

```cpp
enum class KeyCode : std::uint32_t {
    Unknown = 0, A, B, ..., Z, D0, ..., D9,
    F1, ..., F12,
    Space, Enter, Tab, Backspace, Delete, Insert,
    Home, End, PageUp, PageDown,
    Left, Right, Up, Down,
    Shift, Control, Alt, Super,
    Minus, Equal, BracketLeft, BracketRight, Backslash,
    Semicolon, Apostrophe, Comma, Period, Slash, Grave,
    Numpad0, ..., NumpadEnter,
    CapsLock, NumLock, ScrollLock,
    Escape, PrintScreen, Pause, Menu, ContextMenu,
};

enum class MouseButton : std::uint32_t {
    None, Left, Right, Middle, XButton1, XButton2,
};

enum class ModifierKey : std::uint32_t {   // 位标志，VN_ENABLE_ENUM_FLAGS
    None = 0, Shift = 1<<0, Control = 1<<1, Alt = 1<<2, Super = 1<<3,
};

struct KeyEvent {
    KeyCode     code;
    ModifierKey modifiers;
    bool        pressed;   // false = released
    bool        repeat;    // true = auto-repeat
};

struct MouseEvent {
    MouseButton button;    // 纯移动时为 None
    ModifierKey modifiers;
    double x, y;           // 窗口客户区像素坐标
    double dx, dy;         // 相对上帧位移
    bool   pressed;        // true = 按下, false = 释放
};

struct ScrollEvent {
    double      deltaX, deltaY;  // 滚轮增量（行/格）
    ModifierKey modifiers;
};

struct WindowEvent {
    enum class Type { Resize, Move, Close, FocusIn, FocusOut, Minimize, Restore };
    Type type;
    int  width, height;   // Resize 时有效
    int  x, y;            // Move 时有效
};
```

### 3.2 `WindowContext`（只读抽象接口）

```cpp
class VN_WINDOW_API WindowContext : public Object, public RefCounted<WindowContext> {
    VN_OBJECT_META_DECL;
  public:
    ~WindowContext() override = default;

    // 只读查询（派生类持有实际窗口指针并重写）
    virtual String title() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool isVisible() const = 0;
    virtual void* nativeHandle() const = 0;

    // 事件（Signal，供订阅）
    Signal<int, int>            resized;    // (w, h)
    Signal<int, int>            moved;      // (x, y)
    Signal<>                    closed;
    Signal<>                    focusIn;
    Signal<>                    focusOut;
    Signal<const KeyEvent&>     key;
    Signal<const MouseEvent&>   mouse;
    Signal<const ScrollEvent&>  scroll;
    Signal<const WindowEvent&>  windowEvent;

    // 统一事件入口（模板，内联实现）
    template <typename TEvent> void postEvent(const TEvent& event);
  protected:
    WindowContext() = default;
};
```

### 3.3 职责边界（关键设计）

- **WindowContext 不做**：窗口创建、窗口销毁、事件轮询（poll）、平台翻译、数据存储
- **派生类（窗口包装器）做**：持有实际窗口指针（vsg::Window / Qt wrapper / HWND 封装），
  重写 `title()/width()/height()/isVisible()/nativeHandle()` 查询真实窗口的实时状态；
  翻译原生事件后调 `postEvent()` 注入
- **外部宿主做**：创建真实窗口 → 构造派生包装器 → 采集原生事件 →
  翻译成 `KeyEvent`/`MouseEvent`/`ScrollEvent`/`WindowEvent` → 调 `postEvent()` 注入
- **渲染层做**：读 `nativeHandle()` 建渲染表面；订阅 Signal 驱动相机操纵
- `postEvent()` 是唯一事件出口，外部注入与测试合成事件都经过它

## 4. 跨平台与 hwnd 初始化

- `nativeHandle()` 由派生类返回真实窗口句柄（HWND/X11 Window/...）
- 窗口创建完全在外部：宿主自建窗口后由包装器暴露 handle，或宿主包装已有窗口
  （如 Qt 应用把 QWidget 的 winId() 交给后端）
- 渲染后端（如 VSG）读取 handle 建立渲染表面，不拥有窗口

## 5. 依赖关系

```
Window (vn::Window)
  ├── vn::Core     (Object, RefCounted, intrusive_ptr, Signal)
  └── vn::Global   (String, math 等基础类型)
```

## 6. 测试

- `tests/test_window/WindowTest.cpp`：
  - 派生包装器（TestWindowContext 持 FakeNativeWindow）重写只读查询并转发实时状态
  - 空原生窗口时各查询返回默认值
  - `resized`/`key`/`mouse`/`scroll`/`windowEvent` Signal 经 `postEvent()` 分发

## 7. 后续计划

1. `graphics` 依赖 `vn::Window`；`RenderEngine` 增加 `setWindowContext(...)` 消费数据
2. `CameraManipulator` 增加输入接口（`onMouseMove/onScroll/onKeyDown`），
   订阅 WindowContext 事件驱动相机
3. vsg 层实现一个 host（窗口创建 + 事件翻译 + handle 注入 WindowContext），
   供应用层使用

