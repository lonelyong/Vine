# ADR-2026-10-22：pass 调度只在引擎

- 状态：已采纳
- 日期：2026-10-22
- 相关：`../design/graphics-architecture.md`

## 背景

一轮里有两套排序：引擎按 stage/registration 排，后端 `FrameGraph` 再按输入边排。
两套帧词汇、两套“编译”、两个可能互相矛盾的顺序。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保留后端 FrameGraph | 后端自足 | 两套调度；引擎与后端可能不一致 |
| B. 调度上移引擎 | 一个实现；后端只物化 | 后端不再能独立排序 |
| C. 只允许显式 order + 依赖边 | 最简单 | 需要宿主手写顺序；失去稳定排序 |

## 决定

选 **B**：`detail::FrameScheduler`（迭代 Tarjan + 堆就绪队列，稳态零分配）在引擎
`EngineCompile.cpp::scheduleRows()` 里建边、排序、按序发布；`PassStage` 只做同优先级
tie-break；环组件整块跳过并由引擎报一次。

## 后果

- 好：一套顺序、一套帧词汇；后端按 `FrameDescription.passes` 物化，不重排、不报环。
- 代价：后端不能自行调度；任何调度语义变化都改引擎。
- 重开条件：第二个后端需要不同的调度语义（目前没有）。

## 证据

- `backend::FrameGraph`、`FrameCompiler::graph()`、`FrameGraphTest` 已删除。
- `FrameScheduleTest` 覆盖调度；`vsg_rewrite_gate.sh` 两跑 0 VUID / 0 SYNC-HAZARD。
