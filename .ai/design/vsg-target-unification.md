# vsg 后端重构立项：Target 统一 + 去 Scene 绑定（v6）

状态：**主体完成（2026-09-04，C6.4/统一/C6.5）**。`Impl` 三桶
（window_layers/offscreen/screen_slots）已彻底收敛为**统一 `Target`**：
```text
targets[RenderTarget*]        // nullptr 键 = 窗口
  Target
  ├── kind : Window | Offscreen          // 只决定附件来源 + 是否 present/resize
  ├── attachments（offscreen 独有）       // color/depth image+view / render_pass / framebuffer
  ├── graph : RenderGraph                // offscreen 自持；window == impl->render_graph（共享窗口图）
  └── content_slots[(camera, order)]     // order = pass 显式顺序；每槽 = 保留 View(root+SceneBridge+light)
  └── screen_slots[source RT]            // PiP 采样视图（画进本 target 图）
```
窗口与离屏**共用同一套内容槽机制与单一代码路径**（`renderContentSlot`/`setupContentSlot`）；
`renderOffscreenTarget` 拆为 `buildOffscreenTarget`（只建附件+空图，View 交给槽）；每 target 每
(camera, order) 自持 SceneBridge（vsg 按 viewID 编译）。target 内叠画顺序**严格按用户显式设定的
pass order**（`addPass(order)`；引擎每次在 pass 执行前经 `RenderBackend::setPassOrder(order)` 通知
后端 → `ContentSlot.order`；`setupContentSlot` 插入子 View 时保持按 order 升序，PiP 视图视为最高阶）
——**不用 main/HUD 语义限制顺序**。深度风格（depth-on/off、光照）由 `clear()` 标记判定，与顺序解耦。
按显式 order 排序天然吸收引擎 warm-up 的越序创建（warm-up 先建高 order 的 on-top 槽无妨：低 order
槽稍后按 order 插到其前；实测窗口 gizmo 曾因 warm-up 先建而排在 main 之前、被主场景盖住，归位后
children = [o0 MAIN, o10 HUD]）。早期 `primary_camera`/`vsg_camera`/`vsg_scene` 主槽别名与
`vsgCamera()/vsgScene()` 访问器已删除（无外部调用）；`resize()` 改为遍历窗口 target 的全部 main 槽
刷新视口。

历程：**C6.1** 三桶并单表 `targets[]`；**C6.2** `create()` 无参、去 Scene/Camera 绑定、主/顶由
clear() 判定；**C6.3a** `RenderPass::setProgramOverride`（引擎）；**C6.3b** 窗口层键 → `(camera, content slot)`（显式 order 化后此 contentSlot 轴已删：`setContentSlot`/
`contentSlot`/`pending_content_slot` 全移除，内容槽键改为 `(camera, order)`，释放经
`releaseWindowLayer(camera, order)`）；
**C6.4** 离屏升级为同套多槽（同 RT 可烘多遍 shader/内容，`VINE_VSG_OFFSCREEN_MULTISLOT` demo 设备验证：
同 640x360 RT 下 2 个内容槽 + PiP）；**统一** 窗口/离屏同构为 `Target{graph, content_slots[]}` +
单一路径。回归：构建绿、test_graphics 114 / test_vsg 10 绿、三类冒烟（离屏单槽 / 窗口槽叠画 /
离屏多槽）无 Vulkan 校验错。遗留待办：逐槽 depth 策略的引擎侧显式表达、运行期叠加截图目检、
文档(§7/11/12 等旧章节)全量改写。

前置：Overlay 删除 / RenderEngine 单列表（见 `graphics-overlay.md`）。

## 动机（一句话）

后端应是一个“**按输出目标 + 内容槽驱动**的设备”：引擎是 pipeline 的唯一所有者
（scene/camera/order/内容都经 pass 显式送入命令流），后端不再绑定任何 Vine `Scene*`；
**“窗口”只是众多输出目标之一**，与离屏 target 同构。由此顺带解决：
同 target + 同 camera + 同 scene + 不同 shader 的“目标内多槽不覆盖”需求（窗口与离屏同套代码）。

## 1. 现状盘点（已核实代码）

### 1.1 后端绑定：`VsgRenderer(Scene*, Camera*)`
- `Impl{ scene, camera, ... }`，构造时写入（`VsgRenderer.cpp` 构造 + `initialize()` 判空）。
- `impl->scene` 用途：initialize 预编译主内容（`collectSceneCommandsNoCull(impl->scene)` →
  `primary.bridge.syncRenderCommands`）；`setupWindowLayer` 种子主层默认光
  （`impl->scene->lights()` 非空才建灯）。
- `impl->camera` 用途：主层身份 `camera == impl->camera` → `on_top=false`；
  `render()` 空相机回退；`releaseWindowLayer` 摘主层时清别名；便捷 `frame()`
  （`render({}, impl->camera)`）；公开访问器 `vsgCamera()/vsgScene()` 指向主层。
- `vsgCamera()/vsgScene()`：**无任何外部调用方**（仅声明/自用，含 vsg_probe 均未用）。

### 1.2 三桶结构
- `window_layers`（键 `Camera*`）：主窗口 RenderGraph 内多个 View（主层 + 顶部层）。
- `offscreen`（键 `RenderTarget*`）：每 RT 一个独立 RenderGraph + 单 View/bridge。
- `screen_slots`（键 `source RenderTarget*`）：PiP View（挂在主窗口 RenderGraph）。
- “顶部层风格（depth-off + ambient）”由 `camera != impl->camera` 推断，非 pass 携带。

### 1.3 公共 API / 接线
- `RenderBackendFactory::create(raw_ptr<Scene>, raw_ptr<Camera>)`（纯虚，graphics sdk）。
- `RenderBackendRegistry::create(name, scene, camera)`（sdk）+ doc。
- `VsgRenderBackendFactory::create` override（vsg 插件）。
- `RenderControl::wireEvents()`：`entries.front().factory->create(engine->scene(), engine->masterCamera())`（appfw）。
- `tests/test_vsg` `CreateBackendByName`：`registry.create(u8"vsg", scene.get(), camera.get())`。
- `tests/test_graphics` `MockBackendFactory::create(scene, camera)` override。
- 全仓 RenderBackendFactory 实现仅 vsg + 测试 Mock（无其它真后端）。

## 2. 目标架构

```text
targets[ TargetKey ]                 // TargetKey：窗口=专用键；离屏=RenderTarget*
  Target
  ├── kind : Window | Offscreen      // 只决定 附件创建 + 是否 present/resize
  ├── vsg::RenderGraph ×1            // 每个 target 一个（render pass 绑一组附件，硬约束）
  └── content_slots[]                // 目标内有序 View
        slot = {
          内容 Scene(经命令流) + 每槽 SceneBridge/root  ← 每槽自持保留内容（不互相覆盖）
          program/state override（此槽与其它槽的差异：shader/polygon/cull/depth/blend）
          camera/子视口、clear 策略、order、enabled
        }
```

- `render()` 只做：按“当前 target（setRenderTarget，null=窗口）+ 槽身份”find-or-create → sync。
- 窗口 = `kind=Window` 的最终 target：附件来自 swapchain、负责 present/表面重建；内容侧与离屏零差异。
- 跨 target 的 producer→consumer 顺序仍显式维护（离屏先录、消费后录），提交仍是 command_graph 一次 `recordAndSubmit`。
- 后端**不再拥有 Vine Scene/Camera**；“主呈现槽”由引擎显式给出（见 4.2），顶部层风格由 pass/槽携带，
  不再靠 `camera == impl->camera` 猜。

## 3. 公共 API 破坏清单（需同步改）

| 破坏点 | 现签名 | 目标 |
|---|---|---|
| `RenderBackendFactory::create` | `create(raw_ptr<Scene>, raw_ptr<Camera>)` | `create()`（无 scene/camera） |
| `RenderBackendRegistry::create` | `create(name, scene, camera)` | `create(name)` |
| `VsgRenderBackendFactory::create` | 带参 override | 无参 override |
| `VsgRenderer` ctor | `(Scene*, Camera*)` | 无参（或仅保留 kind 引导，无 Vine 对象） |
| `RenderControl::wireEvents()` | `factory->create(scene, masterCamera)` | `factory->create()` |
| `vsgCamera()/vsgScene()` | 主层别名访问器 | 保留（指向主呈现槽）或删除（无外部调用） |

> 引擎 `setBackend`/`addPass` 等 graphics SDK 不受影响（scene/camera 仍是引擎级状态）。

## 4. 关键语义迁移（决定“去绑定”能成立的三件事）

1. **谁清屏 + depth-on（主呈现槽）**：不再由“后端绑的主相机”决定，而是由**引擎注册的呈现 pass**
   携带（该 pass camera==master、target=窗口、clear 开）。后端在窗口 target 下把“清屏/depth-on”的首槽
   建为主呈现槽，后续窗口槽为叠层（clear-off + 各自 depth/blend）。`RenderPass` 已表达 clear；
   depth 策略（scene vs on_top shaderSet）需要由 pass/槽显式给后端（新增最小语义，见 5 的 C6.2）。
2. **初始预编译**：主内容“init 预编译避免帧中首编不可靠”改由**引擎 `initialize()` warm-up 承接**
   （对所有 enabled 且有内容的 pass 预热执行，先于首帧 present）；后端 `initialize()` 只建窗口 target。
   相应把 `collectSceneCommandsNoCull`/`primary.bridge` 预编译路径迁走或保留为窗口 target 惰性首帧编译。
3. **顶部层风格**：`on_top`（depth-off + ambient）不再由 `camera != impl->camera` 推断，
   改为 pass/槽携带（如 `RenderPass` 加 depth-policy，或引擎按内容/顺序告知后端建槽风格）。

## 5. 分阶段迁移（每步保持编译/测试绿）

- **C6.1 内部并表（行为等价）**：把 `window_layers + offscreen + screen_slots` 合并成一张
  `targets[TargetKey]`（窗口=专用键），保留“每 target 一个 RenderGraph + 单内容槽”的现状行为；
  顶层路由/释放统一为 `releaseTargetSlot` 语义。不动公共 API、不动绑定。回归=现状全绿。
- **C6.2 去 Scene/Camera 绑定（公共 API 破坏一次完成）**：`create()` 无参链（sdk/registry/factory/
  RenderControl/test_vsg/Mock）；移除 `Impl.scene/camera` 及其用途；init 预编译迁引擎 warm-up；
  “主呈现槽 + 顶部风格”改为引擎显式（见 4）；`vsgCamera()/vsgScene()` 决定保留指向主槽 or 删除。
- **C6.3 目标内多内容槽（新能力）**：render()/pass 带“槽身份 + program/state override”；
  同 (target, 槽) 各自保留 bridge/root，同 target 同 camera 同 scene 不同 shader → 并存叠层不覆盖；
  移除按槽释放。先以“实体 + 线框叠加（同主相机）”验证。
- **C6.4 离屏复用同一套槽**：`offscreen[RT]` 从“单 View/bridge”升级为“RenderGraph + 多槽”，
  与窗口同构（同 RT 烘多遍 shader）。
- **C6.5 清理与回归**：删旧三桶/旧启发式残留；docs（本文件→正式设计并入 `graphics-overlay.md`
  或模块 md）；全量构建 + test_graphics/test_vsg + App 目检。

## 6. 逐文件影响评估

| 文件 | 影响 |
|---|---|
| `src/viz/graphics/sdk/vine/graphics/RenderBackendFactory.hpp` | create 去参（纯虚 + doc） |
| `src/viz/graphics/sdk/vine/graphics/RenderBackendRegistry.hpp/.cpp` | create(name) 去参 |
| `src/plugins/gfx_backend_vsg/include/vine/vsg/VsgRenderer.hpp` | ctor 去参；Impl 去 scene/camera；槽/风格 API（**注：该头一度于 §44 被删除、类定义并入内部头，又于 §45 以“整个类定义”的身份回到该路径；其状态于 §47 搬进 `VsgRendererState.hpp`，原 `Impl` 更名**） |
| `src/plugins/gfx_backend_vsg/src/VsgRenderer.cpp` | 最大改动：Impl、initialize、render 路由、offscreen/screen、setupWindowLayer、release、frame()、访问器 |
| `src/plugins/gfx_backend_vsg/include/vine/vsg/VsgRenderBackendFactory.hpp` + `.cpp` | create 无参 |
| `src/fw/appfw/src/gui/RenderControl.cpp`（wireEvents） | factory->create() 无参 |
| `src/fw/appfw/sdk/vine/appfw/gui/RenderControl.hpp` | 注释（如有 scene/camera 说明） |
| `tests/test_vsg/GfxBackendVsgPluginTest.cpp` | create(name) 无 scene/camera |
| `tests/test_graphics/GraphicsTest.cpp` | MockBackendFactory::create() 无参 |
| `tests/test_vsg/*`（子组件单测） | C6.3 起新增“多槽/多 shader”语义用例 |
| `src/plugins/app_shell/src/AppShellUi.cpp` | 若用 addOffscreenToScreen 等，C6.4 复核 |
| `RenderPipelineBuilder` | 无引擎侧改动预期；复核离屏多槽 API 兼容 |
| docs（`gfx_backend_vsg.md`/`vine-to-vsg-data-flow.md`/`.ai/*`） | C6.5 同步 |

## 7. 风险与边界

- 附件差异是 kind 而非图：窗口(swapchain/present) 与离屏(自建可采样) 仍是各自 RenderGraph（硬约束），
  只是同表管理。
- 目标内多槽的 clear/load 语义：首槽 clear、后续 clear-off + 各自 depth/blend；全同槽=双倍填充无意义。
- 跨 target producer→consumer 录制顺序必须显式，不能由“一张表”自动保证。
- vsg 按 viewID 编管线：每槽必须自持 SceneBridge（不可共享），内存随槽数线性。
- 全图 compile 非增量现状不变；帧中首编不可靠 → 引擎 warm-up 覆盖所有 enabled pass。
- 公共 API 破坏需一次合并到位（sdk create 链 + RenderControl + tests），避免半途编译裂开。

## 8. 关联
- `.ai/design/graphics-overlay.md`（前置：pass 单列表/窗口层）
- `.ai/design/graphics-render-pipeline.md`（多 pass / 槽 / 命名产出）
- `gfx_backend_vsg.md` §7/11/12（现状窗口层/离屏/PiP、生命周期）
- `vine-to-vsg-data-flow.md`（数据映射、SceneBridge 契约）
