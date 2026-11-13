# ADR-2026-11-13：一个引擎驱动一个表面 —— 否决"一引擎多 Surface"（graphics-next F8）

> **状态**：已采纳
> **日期**：2026-11-13
> **影响范围**：`.ai/design/graphics-frame-pipeline.md`（F8 / §3.8 / R21 / R27 / R37 / T29 / K10 / §7 / §9.3）、
> `.ai/design/graphics-architecture.md`（F19 / D2 / §2.8 名词纪律）、`.ai/backlog/graphics.md`。**无代码改动**。

## 背景

- graphics-next 的 F8 / `R21` / `R27` / `R37` / `T29` / `K10` 要求"一个 Session 驱动多个 Surface"：
  `Target = Surface{SurfaceId}`、帧请求带 `SurfaceId`，判据是"一个 Session 两个 Surface 都正确 present"，
  且明确禁止用第二条引擎线程实现（`K10`）。
- 同一仓库的 `graphics-architecture.md` §7 **D2 早已否决**"一帧跨多窗口"；而 §3.5 F19 却写着"一个引擎可服务
  多个窗口"——两句话互相矛盾。
- 代码站在 D2 这边：`RenderEngine::createSurface()` 拒绝第二个区（`area_refused_`，每次 episode 报一条），
  `announceAreaSize()` 只认自己那个区，一帧只有一个 `frame.target`，`RenderSurface` 的规则是"一区一幅画"，
  且 `RenderControl` / `SurfaceWindow` 每个窗口造一个引擎。
- 用户文档 `docs/guides/render-views.md` §7 已按"每窗口一个引擎、一个设备可服务多个引擎"写，并把
  "一帧跨多窗口"标为"登记不做"。
- 2026-11-13 做了逐文件的代价评估：实现 F8 约 **900–1400 行、14–20 个文件**（SDK 形状 ~300–450、引擎内部
  ~150–250、vsg 后端 ~250–400、appfw ~150–300、测试 ~150–250），并要重写 `FrameResult` / `PresentRecord` 的
  单值语义——后者刚在 F12（同日）落地。

## 决定

1. **一个 `RenderEngine` 驱动一个 `RenderSurface`**（现状，不变）。
2. **多窗口 = 一个 `RenderDevice` 服务多个引擎**：每窗口一个引擎、一个区、一个 `Viewer`（现状）。
3. 从 graphics-next 删除 F8、`R37`、`T29`、`K10` 与 §9.3 的"步 4"；`R21` / `R27` 按"一引擎一表面"改写；
   `SurfaceId` 从 §2.5 的 id 表移除。
4. 修正 `graphics-architecture.md` F19，并把那里的 D2 记为本 ADR 的前身。
5. `FrameResult.present` / `PresentRecord` 保持**单值**：一帧一个表面。

## 理由

- `G8`：仓库里没有"同一计划要在两个窗口以不同视口呈现"的真实宿主。两个窗口今天就是两个引擎，工作正常；
  没有消费者就预建 surface 集合、`SurfaceId` 与 per-surface 结果，是把复杂性先造出来。
- `G5`：surface 集合是新机制，必须有家（`detail::`），否则 `RenderEngine`（1484 行）继续变胖，直接违背
  `graphics-architecture.md` §2.8 的组合根声明。
- `G4` / `G10`：一帧多表面会把 `FrameResult.present`、`PresentRecord`、失败语义（一个表面 `Lost` 不等于整帧
  `Lost`）与 transient 默认尺寸（跟随哪个表面）全部改成 per-surface；F12 同日刚落成单值，再做一次是返工。
- `G2`：一帧跨表面 = 一次 `executeFrame` 内 N 次 acquire/present，而后端执行器只有一个 window（`d->driving`）；
  这是执行层的新语义，不是搬运。

## 否决的选项

| 选项 | 否决理由 |
|---|---|
| 按 F8 实现"一引擎多 Surface" | 无消费者（`G8`）；公开结果形状刚改过再改一次（`G10`）；appfw 所有权（窗口拥有引擎）要重写；失败语义全新。 |
| 只做引擎/SDK 半边（F8a）、appfw 不动 | `T29` 要求"一个 Session 两个 Surface 都正确 present"，没有 appfw 形态就没有真实调用点，等于预建能力（`G8`）。 |
| 保留 F19 的说法、不改代码 | 文档与代码相反、也与 D2 相反——读者会按不存在的形状写宿主。 |
| 用两个 Session 共享设备实现多窗口 | `I6`：只有一条引擎线程碰设备；两个 Session 各自有引擎线程就是两条（审计 X4 已指出）。 |

## 后果

- **放弃**多窗口共享 pass 表 / 计划 / 编译的收益：每窗口一份引擎状态（plan、slots、发布表）。
  **仍共享**设备级资源：两个引擎共用一个 `RenderDevice`，管线缓存与显存按设备算。
- 文档回到一致：F19、D2、`docs/guides/render-views.md` §7 与 `createSurface()` 的契约说的是同一件事。
- `FrameResult` / `PresentRecord` 的单值语义有了书面依据（一帧一表面）。
- 代价写在明处：**每窗口一份编译与驻留账**（`docs/guides/render-views.md` §7 已记录同一事实）。

## 触发条件（何时重开）

命中任一条，按**新 ADR** 重开，不改本 ADR：

1. 真实宿主需要"同一计划（同一组 pass / 同一条链）在两个窗口以不同视口呈现"，且实测多引擎的重复编译 / 显存
   开销成为瓶颈；
2. 第二个后端落地，使"设备 = Session"成为必要（与 backlog `T15` / `T22` 同触发）。

## 参考

- `.ai/design/graphics-frame-pipeline.md` F8 / §3.8 / `R21` / `R27` / `R37` / `T29` / `K10`（本 ADR 删除或改写）
- `.ai/design/graphics-architecture.md` §3.5 F19、§7 D2
- `docs/guides/render-views.md` §7（现役多窗口立场）
- `src/viz/graphics/src/RenderEngine.cpp`：`createSurface()`、`announceAreaSize()`、`buildContentHalf()` 的
  `frame.target`
- `tests/test_graphics/RenderSurfaceTest.cpp`（第二区被拒）、
  `tests/test_graphics/HeadlessBackendTest.cpp`（`OneDeviceServesSeveralAreasWithoutCrossingThem`）
