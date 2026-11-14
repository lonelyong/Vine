# demo 建了 `PassChain` 却从未采纳：链拥有的场景不发布、尺寸也不由视图维护

- **状态**：已修复（2026-11-14，`demo_plugin`）
- **发现日期**：2026-11-14
- **模块**：`src/plugins/demo_plugin`（宿主侧接线；引擎契约本身正确）
- **涉及**：`src/plugins/demo_plugin/src/DemoScene.cpp`（`addDemoPipeline`）、
  `src/viz/graphics/src/PassChain.cpp`（`PassChain::build` 不自我采纳）、`src/viz/graphics/src/SceneView.cpp`
  （`setChain` / `publishContent` / `applySurfaceGeometry`）

## 现象与影响

打开 demo 后日志每帧第一帧报一条

```
[graphics] pass 'forward_transparent' draws a scene that was never published: publish it (Scene::publish),
or bind a publication before the frame (RenderEngine::bindSnapshot)
```

这条 pass 于是**画 nothing**：deferred 路径的叠加内容（天空盒、混合体）整块缺失。同时链的离屏目标与 HUD 盒不由
视图维护，demo 只好在自己的 `addSurfaceLayout` 里补一次 `pipeline->resize(...)`——两份尺寸来源，谁改视图的布局
行为都会让另一份漂移。

**为什么不是后端的错**：`vsg` 与 `vk` 都报同一条 warning；发布是引擎/宿主约定，与后端无关。

## 根因

`PassChain::build()` 只**构建并登记**（`ChainBuilder` + `apply`），**不把链交给任何视图**；采纳是宿主的动词
（`SceneView::setChain`）。`Viewer` 做对了，demo 漏了：

- `SceneView::publishContent()` 的行为是「先发布 `scene_`，再 `pipeline_->publishContent()`」（后者发布链拥有的
  `transparent` 场景）；
- `SceneView::applySurfaceGeometry()` 在自己的布局步里 `pipeline_->resize(width, height, ratio)`。

视图的 `pipeline_` 为空 ⇒ 两件事都不发生。链之所以还活着，只是因为它被 demo 的布局 lambda 捕获了；一个只被
capture 的链在视图看来并不存在。

## 修复

`addDemoPipeline` 在建链之后立刻 `view->setChain(pipeline)`；demo 自己的布局步只保留「按源图纵横比重排 G-buffer
预览视口」那一半，不再重复 `resize`（视图的步先跑，且已把链调整到表面尺寸）。`surface->present(presentingPass())`
保持不变（那是「谁上屏」，与采纳是两件事）。

## 验证

- 应用（vk 与 vsg 各一次，`--open demo`）：`never published` 计数 **0**（修复前每次运行 1）。
- 画面：修复后两后端的天空/叠加内容出现；vk 与 vsg 同尺寸渲染区逐像素比较 **1.88%** 不同（只剩 FPS 数字与
  gizmo 的计时像素）。
- 窗口 resize（`xwinresize.py 0x<top> 1200 800`）后画面跟随、无警告——链的尺寸确实由视图维护。

## 设计约束

- **G3**（创建 → 配置 → 运行 → 取结果 → 释放要一遍读懂）：建链与采纳是两条动词，`PassChain::build` 不自我
  采纳是**故意**的（宿主决定哪块视图画它），但文档与示例必须把两步写成一步的用法。
- **G4**（接口语义）：`publishContent` 的「链拥有的场景由链发布」只有在链被采纳后成立；不采纳时没有任何错误，
  只有一条一次性 warning——这是本次症状隐蔽的原因。
- **G6**（统一而一致）：同一件事（链的尺寸）曾有两个驱动者（视图步 + demo 步），修复后只剩视图一处。
