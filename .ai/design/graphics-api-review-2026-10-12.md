# 框架级 API 设计复核（骨架与抽象，2026-10-12）

> 一次**独立会话**的只读复核（不认同的文件用"`文件:行` + 反例 + 代价"写）；结论落地在本文件，判据/坑见
> `.ai/memory/graphics.md` 顶部 2026-10-12 条。**已裁决项不重复上报**（§8 否决清单、T1–T7、多格下线、
> `PresentTarget` 两层拆法、`RenderTarget` 不改名、云渲染 = 第二个呈递端）。
>
> 覆盖：`.ai/design/graphics-layering.md`、`graphics-window-device-viewer.md`（含 §8/§9）、`graphics-render-next.md`
> （§5.1 T1–T7、§6）、`appfw-render-view.md`（§2/§5/§7/§9）、`graphics-render-pipeline.md`（§14）、
> `graphics-redesign.md`；两份 memory 顶部；`docs/guides/render-views.md`；graphics SDK 全量公开头 + 实现
> （`RenderEngine` / `RenderSurface` / `RenderPass` / `EngineCompile` / `RenderPipelineBuilder` / `SceneView`）、
> appfw（`RenderControl` / `SurfaceWindow` / `Viewer` / `Application` / `Plugin` / `DocumentView*` / `EventBus` /
> `Config*`）、插件侧 `VsgBackend` 的目标实现与相关用例。未深挖：`CommandManager` / `UserIO` / `ConfigManager`
> 内部、iobase / robotics。

## 结论（一句话）

最脆的一环是 **"区 / 引擎 / 目标"的所有权与基数**：发现 1/2/3 是同一条链上的同一类错 —— **一条事实有多个持有者，
其中一个永远在说"我仍然有效"**；它们今天全靠"只有一块区、一块窗口、且后端把自己当目标"这三个巧合成立，
S10b（多窗口共享设备）一落地就会同时踩到。

## 发现与处置

| # | 判定 | 问题 | 处置 |
| --- | --- | --- | --- |
| 1 | 结构错 | 一个引擎挂两块区是**静默**的：`adoptArea` 后来者顶替（"last one announced wins"），被顶替那块的 `resize()` 照样改写 `frame_ctx_`；两块区各自 `present` 都成功，两幅画进同一把交换链 | **已修**：`adoptArea` 拒绝 + 报一次（`UnsupportedRequest`）；`announceAreaSize(from, w, h)` 只认引擎自己那块区；不是引擎的区 `present` 也拒绝 + 报一次 |
| 2 | 结构错 | `present(RenderPass*)` 不校验归属，`removePass` 又不通知区 ⇒ `presentingPass()` / `hasPresentation()` 会给"已不在帧里"的假答案，`Viewer::takeArea/isActive` 就吃这个答案 | **已修**：`present(pass)` 要求 `engine_->passOrder(pass)` 有值（否则拒 + 报一次）；`presentingPass()` / `hasPresentation()` 按"在帧里"过滤。顺带发现："视图已呈现另一块区"这条规则在"一引擎一区 + 视图必须同引擎"之下**不可达**，规则与用例一并退场 |
| 3 | 结构错 / 生命周期 | 借来的 `WindowPresentTarget` 没有撤销点，两条契约句互相矛盾（"设备可在关窗时放掉" vs "必须活过区的使用"），今天只因 `VsgBackend` 把自己当目标才不炸 | **已修（改口径，后端零改动）**：目标**活到设备销毁或区解绑**，关窗只是 **detach**；`RenderEngine::shutdown()` 在拆设备前 `area_->bindTarget(nullptr)` |
| 4 | 缺入口 | `RenderSurface(raw_ptr<RenderEngine>)` 允许 null，但 null 的区永久死且失败全静默（没有 `setEngine`） | **已修**：构造函数改收 `RenderEngine&`（非空由签名保证，与 `present(SceneView&)` 同款），调用点 31 处机械替换 |
| 5 | 文档漂移 | `SceneView.hpp` 仍按"视图自己的矩形"写（同文件另一处又写对了）、`RenderEngine::passOrder` 说"读者是区"、`PresentTarget.hpp` 引用了不存在的 `RenderEngine::render()`、`RenderEngine` 类注释块与 `class` 挤在一行 | **已修**：纯注释 |
| 6 | 命名 | appfw GUI 公开布尔 getter 违约定：`Control::enabled()/visible()`、`RibbonAction::enabled()/checkable()/checked()` | **已修（2026-10-19）**：改为 `isEnabled()/isVisible()/isCheckable()/isChecked()`（`RibbonButton` 同步），配套补 Doxygen，`VN_APPFW_PLUGIN_ABI_VERSION` 7u→8u；调用点与测试同步 |

## 看着可疑但确实合理（别在后续复核里反复提）

- `RenderPass` 的产出/输入各有四套拼写（name / `ImageRef` / `RenderTarget` / `PassDesc`）：§14 已裁决（细声明驱动运行时接线、
  名字是糖、粗声明兜底），不是残留。
- `RenderBackend` 一个对象同时是设备 + 执行器 + 目标：S10a 的**过渡聚合**，三张契约已各自成头。
- 呈递端两层（`PresentTarget` + `WindowPresentTarget`）：2026-10-12 裁决；离屏 = `executeFrame(frame, nullptr)` 是一等形态。
- 目标用借用指针而不是 `intrusive_ptr`：S10a 的理由成立（它的问题只在"没有撤销点"，即发现 3）。
- `Viewer` 住在 appfw::gui、老三条转发、`isActive()`/`takeArea()` 读区：§9.3 决议 + 不变量 1。
- `RenderControl` 构造只组装、第一拍兜底 attach、无后端直接 `Failed`：S0/§2.3 裁决。
- `SurfaceWindow` 单视图仍按按钮记捕获：左键拖拽与右键菜单共存需要；多格下线删的是"按点路由"。
- 后端半看着"重复"实则不然（2026-10-16 独立复核；**别当"重复"改**）：`RetirementQueue::retirePoint` 是转发（单一实现
  在 `FrameTimeline::retirePoint`）；`DataKey` / `InstanceSlot` 是 audit 表的**词汇条目**（用例 `CoreKeysTest` 钉住），
  不是误留；`Observe` 与 `RetirementQueue` 的保留数是**故意互校**（`Observe::agreesWith`，两条用例钉住）；`FrameRing` /
  `MaterialArena` 共用 `perFrameCopies` 规则而结构不同；`PixelProbe` / `DepthProbe` 是像素类型不同的孪生夹具；
  `FrameGraph` 取 `CollectedPass` 是 recorder → graph → compiler 的**相邻流水线段**，不是分层违规。

## 值得做但现在不做（带触发条件）

1. ~~**删/弃用 `SceneView::frame(double)`**~~ **已做（2026-10-14：公开面清理中删除，零调用者）**。原判据：触发 = 下一次公开面清理，或出现"宿主把视图当帧泵"的用法（它会绕过
   `RenderControl` 的 surface_ok / 暴露检查）。
2. **发现 3 的契约（已落地为口径）**：这不是"以后做"，而是 S10b 的**前置约束** —— 越早定越省。另一条同源前置：**共享设备下 `initialize` / `shutdown` 归谁**，见 `graphics-window-device-viewer.md` §9.7（2026-10-16 补登）。
3. **发现 6 的改名**：并进下一次 appfw 公开面小步（要动 ABI 版本号）。
4. **`SurfaceWindow` 输入路由骨架整理**：触发 = 输入路由/焦点重新进 SDK 的那天（`viewAt` / `focus` 已删，
   `routedView` 恒返同一视图）。
5. **T2 / S10b**：已登记、触发条件未命中；本次只加了第 2 条前置。
