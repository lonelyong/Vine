# window 模块设计（跨平台窗口上下文抽象）

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：平台无关的窗口事件类型与 `WindowContext` 只读接口；不负责创建/销毁窗口。
> **权威来源**：`src/base/window/sdk/vine/window/**`、`tests/test_window`。
> **待办**：`../backlog/`

## 1. 概要设计

### 1.1 问题与背景

渲染、相机操纵和应用框架都需要窗口的尺寸、可见性、原生句柄与输入事件，但它们不应该各自依赖某个平台
窗口库，也不应该由 base 层创建窗口。`window` 只提供**平台无关的窗口数据上下文**。

### 1.2 目标

- 定义跨平台事件类型（键码/鼠标/滚轮/窗口事件）。
- 定义 `WindowContext` 只读抽象接口 + Signal 事件。
- 派生类包装真实窗口，重写只读查询；宿主把原生事件翻译后注入。

### 1.3 非目标

- 不创建/销毁/轮询窗口；这些都在外部宿主。
- 不做平台翻译（Qt/X11/Win32 的映射在各自宿主）。
- 不存储窗口状态；查询直接读真实窗口。

### 1.4 一句话结论

`window` 是 base 层的**只读窗口上下文**：`WindowContext` 暴露标题/尺寸/可见性/原生句柄与 Signal，
宿主通过唯一的 `postEvent()` 注入事件；窗口的创建、销毁和轮询全在外部。

## 2. 设计要点

### 2.1 只读接口，不拥有窗口

`WindowContext` 不创建、不销毁、不轮询、不存储；派生类持有实际窗口指针，重写 `title()` / `width()` /
`height()` / `isVisible()` / `nativeHandle()` 查询实时状态。

### 2.2 依赖只向上

`window` 只依赖 `vn::Core`（Object/RefCounted/intrusive_ptr/Signal）与 `vn::Global`（String/math）。
渲染层依赖 `window`，`window` 不依赖渲染层。

### 2.3 事件只有一个入口

`postEvent()` 是唯一事件出口；外部注入与测试合成都走它。宿主负责把原生事件翻译成
`KeyEvent` / `MouseEvent` / `ScrollEvent` / `WindowEvent`。

### 2.4 职责边界

| 角色 | 职责 |
| --- | --- |
| `WindowContext` | 只读查询 + Signal + `postEvent()` |
| 派生包装器 | 持真实窗口指针，重写查询，翻译原生事件并注入 |
| 外部宿主 | 创建/销毁/轮询真实窗口 |
| 渲染层 | 读 `nativeHandle()` 建表面；订阅 Signal 驱动相机 |

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 平台无关键码/鼠标/修饰键 | `KeyCode` / `MouseButton` / `ModifierKey` 位标志 | 已落地 |
| F2 | 输入事件 | `KeyEvent` / `MouseEvent` / `ScrollEvent` | 已落地 |
| F3 | 窗口事件 | `WindowEvent`（Resize/Move/Close/Focus/Minimize/Restore） | 已落地 |
| F4 | 只读上下文 | `WindowContext` 查询 + Signal + `postEvent()` | 已落地 |

### 3.1.1 F1 键码/鼠标/修饰键

- **行为**：`KeyCode` 覆盖字母/数字/功能键/方向/修饰/编辑/小键盘；`MouseButton` 含 XButton1/2；
  `ModifierKey` 是位标志（`VN_ENABLE_ENUM_FLAGS`）。
- **约束**（C1）：枚举值平台无关；宿主负责把原生键码映射进来。
- **要求**（R1）：新增键码不改 `WindowContext` 接口。
- **验收**（T1）：`tests/test_window/WindowTest.cpp` 的事件分发用例。
- **注意**（N1）：`KeyEvent::repeat` 表示自动重复，不是按下次数。

### 3.1.2 F2 输入事件

- **行为**：`KeyEvent{code, modifiers, pressed, repeat}`；
  `MouseEvent{button, modifiers, x, y, dx, dy, pressed}`；`ScrollEvent{deltaX, deltaY, modifiers}`。
- **约束**（C2）：`MouseEvent::button` 在纯移动时为 `None`；坐标是窗口客户区像素。
- **要求**（R2）：事件类型是值，不持有窗口指针。
- **验收**（T2）：`postEvent()` 分发到对应 Signal。
- **注意**（N2）：滚轮增量单位是行/格，由宿主决定。

### 3.1.3 F3 窗口事件

- **行为**：`WindowEvent{type, width, height, x, y}`；`Resize` 时宽高有效，`Move` 时 x/y 有效。
- **约束**（C3）：窗口生命周期事件只通知，不替宿主做创建/销毁。
- **要求**（R3）：`Close` 是通知，不是关闭动作；宿主决定是否真的关。
- **验收**（T3）：`windowEvent` Signal 分发。
- **注意**（N3）：`Resize` 的尺寸是客户区，不含边框。

### 3.1.4 F4 只读上下文

- **行为**：`title()` / `width()` / `height()` / `isVisible()` / `nativeHandle()` 纯虚；
  `resized` / `moved` / `closed` / `focusIn` / `focusOut` / `key` / `mouse` / `scroll` / `windowEvent` Signal；
  `postEvent()` 模板入口。
- **约束**（C4）：查询是纯虚，派生类必须读真实窗口；`nativeHandle()` 返回平台句柄。
- **要求**（R4）：`postEvent()` 是唯一事件出口；外部注入与测试合成同路。
- **验收**（T4）：`TestWindowContext` 持 `FakeNativeWindow`，重写查询并转发实时状态。
- **注意**（N4）：空原生窗口时各查询返回默认值。

## 4. 公开接口

### 4.1 `WindowContext`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `title()` / `width()` / `height()` / `isVisible()` / `nativeHandle()` | 只读查询 | 派生类读真实窗口 |
| `resized` / `moved` / `closed` / `focusIn` / `focusOut` | 窗口 Signal | `Signal<int,int>` / `Signal<>` |
| `key` / `mouse` / `scroll` / `windowEvent` | 输入 Signal | 事件引用 |
| `postEvent(const TEvent&)` | 唯一事件入口 | 模板内联 |

### 4.2 事件类型

| 类型 | 字段 |
| --- | --- |
| `KeyEvent` | `code` / `modifiers` / `pressed` / `repeat` |
| `MouseEvent` | `button` / `modifiers` / `x,y` / `dx,dy` / `pressed` |
| `ScrollEvent` | `deltaX,deltaY` / `modifiers` |
| `WindowEvent` | `type` / `width,height` / `x,y` |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 枚举值平台无关；原生键码映射在宿主。 |
| C2 | `MouseEvent::button` 纯移动为 `None`；坐标是客户区像素。 |
| C3 | 窗口事件只通知，不替宿主创建/销毁。 |
| C4 | 查询纯虚，派生类读真实窗口；`nativeHandle()` 返回平台句柄。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | 新增键码不改 `WindowContext` 接口。 |
| R2 | 事件类型是值，不持有窗口指针。 |
| R3 | `Close` 是通知，是否关闭由宿主决定。 |
| R4 | `postEvent()` 是唯一事件出口。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | `window` 只依赖 `vn::Core` + `vn::Global`。 |
| N2 | `WindowContext` 不创建/销毁/轮询/存储窗口。 |
| N3 | 空原生窗口时查询返回默认值。 |

## 6. 注意事项与陷阱

### 6.1 句柄生命周期

- 会怎么错：宿主销毁真实窗口后，派生包装器仍返回旧句柄，后端拿它建表面。
- 怎么判：窗口销毁后 `nativeHandle()` 非空，或 attach 失败。
- 怎么防：宿主在销毁窗口前先断开渲染表面；`WindowContext` 不拥有句柄。

### 6.2 事件翻译在宿主

- 会怎么错：把 Qt/X11/Win32 的分支塞进 `window`，base 层被平台污染。
- 怎么判：`window` 出现平台头文件或 `#ifdef`。
- 怎么防：平台映射留在宿主；`window` 只收平台无关的事件值。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | `window` 创建/销毁/轮询窗口 | 平台细节应归宿主；base 层只给数据上下文。 | — |
| D2 | `window` 做平台翻译 | 会让 base 依赖 Qt/X11/Win32。 | — |
| D3 | 在 `WindowContext` 里存窗口状态 | 派生类读真实窗口即可，存一份会过期。 | — |
| D4 | 多个事件入口 | 外部注入与测试合成分叉；只留 `postEvent()`。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 派生包装器重写查询并转发实时状态 | `tests/test_window/WindowTest.cpp` |
| T2 | 空原生窗口返回默认值 | 同上 |
| T3 | `resized`/`key`/`mouse`/`scroll`/`windowEvent` 经 `postEvent()` 分发 | 同上 |

## 9. 状态与演进

### 9.1 当前状态

接口层已落地（纯头文件 + `WindowContext.cpp`）；测试覆盖查询与事件分发。

### 9.2 已知缺口 / 待办

1. `graphics` 依赖 `vn::Window`；`RenderEngine` 增加 `setWindowContext(...)`。
2. `CameraManipulator` 增加输入接口并订阅 `WindowContext` 事件。
3. vsg 层实现 host（窗口创建 + 事件翻译 + handle 注入）。

### 9.3 兼容性 / 迁移

新代码只依赖 `WindowContext` 抽象与 Signal；不要直接依赖某个平台包装器。

## 10. 参考

### 10.1 代码

- `src/base/window/sdk/vine/window/**`
- `src/base/window/src/WindowContext.cpp`
- `tests/test_window/WindowTest.cpp`

### 10.2 文档与 ADR

- `.ai/design/graphics-architecture.md`
- `.ai/design/appfw-render-surface.md`
- `../backlog/`

### 10.3 外部资料

- Qt / X11 / Win32 的原生事件模型（映射在各自宿主）。
