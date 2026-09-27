# Bug: 嵌入式渲染后端初始化崩溃（关闭窗口 / Qt 重建 surface 时）（历史登记）

- **日期**: 2026-09-02
- **模块**: gfx_backend_vsg 插件（嵌入式 Qt 子 HWND 渲染路径）
- **状态**: 已修复

## 现象

执行 `TestRenderLiveCommand` / 关闭窗口时进程崩溃；崩溃栈落在
`vsg::Device::Device`（Device.cpp:63），是未被捕获的 C++ 异常。嵌入式
（`RenderControl`，Qt 子 HWND）视图此前还表现为空白。

## 根因

vsg 默认 `VSG_MAX_DEVICES == 1`（本 build 的 `Version.h` 中即为 1）。
`RenderControl` 在 Qt 重建原生 surface（布局/首次显示/关闭时常重建 HWND）时，
会 `shutdown` 后端再重新 `initialize`。但 `VsgRenderer::shutdown()` 原来只释放
`window` / `viewer`，保留在 `vsg_scene`、`render_graph`、`SceneBridge` 缓存、
`VsgMaterialManager` 缓存里的**已编译 pipeline / descriptor 仍引用旧
`vsg::Device`**，导致旧 Device 不析构、设备 ID 不被释放。下一次
`Window::create()` 分配设备 ID 1，触发：

```
Number of vsg:Device allocated exceeds number supported
```

`initialize()` 原来没有 try/catch，异常一路抛到 Qt 事件循环 → `terminate` 崩溃。

## 修复

文件：`src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp`

1. `initialize()` 整体包 `try/catch`：捕获后打印 `[VsgRenderer] ...`，调用
   `shutdown()`，返回 `false`（不再把异常抛给 Qt）。
2. `shutdown()` 现在同时释放 `render_graph`、`vsg_scene`、`vsg_camera`，并调用
   `sceneBridge.clearCache()`、`materialManager.clear()`，确保下一次 `initialize()`
   创建新 Device 前旧 Device 已被销毁、设备 ID 已被回收。
3. `Window::create` 失败、`CameraBridge::create` 失败、`compile()` 失败等
   Window 创建之后的失败路径，统一先 `shutdown()`（带日志）再 `return false`，
   避免泄漏一个活着但未初始化的 Device，导致重试时再次触发设备超限。
4. `traits->debugLayer = false`：避免加载本机不可用的 validation layer。

## 验证

修复后嵌入式路径首次初始化成功（启动日志无 `[VsgRenderer]` 报错），surface
重建 / 关闭窗口不再触发设备超限崩溃。
