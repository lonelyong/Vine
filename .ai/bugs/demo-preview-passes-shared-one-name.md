# demo 的四个 G-buffer 预览共用一个 pass 名：只剩最后一个，且布局重排从不生效

- **状态**：已修复（2026-11-14，`demo_plugin`）
- **发现日期**：2026-11-14
- **模块**：`src/plugins/demo_plugin`（宿主侧接线；引擎的 `(owner, name)` 计划语义本身正确）
- **涉及**：`src/plugins/demo_plugin/src/DemoScene.cpp`（`addDemoPipeline`）、
  `src/viz/graphics/src/EngineCompile.cpp`（`applyPlanRows`：同 owner 同名 = 一行，原地 `setDesc`）

## 现象与影响

deferred demo 顶部的 G-buffer 预览条本应是四块（albedo / shading normal / occlusion / view position），
实际**只有第 4 块**出现。修复后帧表是四条独立行，四块 tile 的均值各不相同：

| tile | 帧表名字 | 均值（RGB） |
|---|---|---|
| 0 | `gbuffer_preview_albedo` | (65.2, 65.5, 66.5) |
| 1 | `gbuffer_preview_normal` | (3.0, 18.5, 13.7) |
| 2 | `gbuffer_preview_emissive`（occlusion 走 alpha） | (30.1, 30.1, 30.1) |
| 3 | `gbuffer_preview_view_position` | (12.3, 3.1, 0.0) |

## 根因

两件事叠在一起，都源于“计划按 `(owner, name)` 认行”：

1. 四个预览都叫 `gbuffer_preview`、owner 都是 render control ⇒ `applyPlanRows` 把它当成**同一行**的
   四次声明，最后一次（attachment 3）的 desc 原地覆盖前三次（`setDesc`）。于是帧里只有一个预览 pass，
   采样的是 attachment 3。
2. 布局步里的“按源图纵横比重排预览矩形”只改了 demo 自己持有的 `ScreenPass` 对象：
   `previews[i]->setViewport(...)`。引擎的节点是**从声明建出来**的，改对象不回灌；重排因此从未生效
   （帧表里的矩形一直是最初的 160×90 槽位）。

## 修复

- 每个预览一个名字（按它显示的通道：albedo / normal / emissive / view position）——同一 owner 下名字就是
  行身份。
- 布局步在 `setViewport` 之后**重新声明**该 pass（`addDemoPass` 再应用一次，同 owner 同名原地更新），
  矩形才会到帧里。

## 验证

- `VN_FRAME_PLAN=1`：帧表从 1 条 `gbuffer_preview` 变为 4 条（order 120–123，矩形各不相同）。
- 窗口拉宽到 1032×421 抓帧：四块 tile 的均值如上（互不相同，且都不是背景色）。
- vk 与 vsg 同场景渲染区**逐字节相同**（0/82004 像素差）；`test_vk` 72/72、`BackendParityTest` 12/12。

## 设计约束

- **G4**（接口语义）：`(owner, name)` 是计划的行身份，名字不是标签；同一个贡献者要画四块就必须声明四行。
- **G9**（可扩展）：预览条按 attachment 循环，加一个通道 = 表里多一项，不需要动布局步的形状。
- **G11**（可调试性）：帧表一眼就能回答“这条流水线到底有几行、矩形是什么”；症状能被表直接读出来，
  不需要猜。
