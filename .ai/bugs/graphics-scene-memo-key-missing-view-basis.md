# 场景收集 memo 的视图键漏掉 view 基与 up，可能返回另一视图的 billboard

- **状态**：待修复；常规 `Camera` 用法下不易命中（投影与视图同源），`CollectView` 直接构造时可达。
- **发现日期**：2026-11-02
- **模块**：`src/viz/graphics`
- **涉及**：`Scene::collectRenderCommands(const CollectView&, std::uint32_t)` 的 memo 判定（`src/viz/graphics/src/Scene.cpp:700-710`）、
  `CollectView`（`sdk/vine/graphics/CollectView.hpp`）、`AutoTransform::localToWorldForView`（`src/viz/graphics/src/AutoTransform.cpp:278-321`）

## 现象与影响

收集结果的 memo 键由 `projection * view`、`eye`、`layer_mask`（必要时加 viewport）组成，但视图相关节点读的是
**分解后的** `view` 与 `up`：

- `RotateToScreen` 用 `screenRotation(view.view * parent_world)`（`AutoTransform.cpp:292`）；
- `RotateToCamera` 用 `view.up` 求局部 up（`AutoTransform.cpp:300`）。

两者都不是 `projection * view` 的函数：存在 `P1*V1 == P2*V2` 而 `V1 != V2` 的组合（同一合成矩阵的不同分解），
也存在 `view` 相同、`up` 不同的 `CollectView`。此时 memo 命中，返回的是上一视图算出来的 billboard 变换：
billboard 朝向错误、屏幕空间缩放的缩放因子错误，且不会自我纠正。

## 根因

memo 的键只覆盖了“收集用到的输入”的一个子集，而且是按“通常由 `Camera` 生成”这个假设挑的；`CollectView` 是公开结构，
它允许调用者分别提供 `projection` / `view` / `eye` / `up`，契约里没有任何一条要求它们必须来自同一个相机对象。

## 修改建议

1. 键覆盖**实际读取的输入**：至少把独立的 `view` 与 `up` 纳入比较；其余视图相关项（viewport 已处理）逐一核对
   `Node::localToWorldForView` 的实现，凡是读了的都进键。
2. 更稳妥的做法是把"视图相关项"做成节点自己声明的小集合（现在已有 `ViewDependence` 档位），memo 按其中出现过的
   最高档位决定比较哪几项——这样新增一个视图相关节点时不至于再漏一次。
3. 头文件里写明：`CollectView` 的 `projection` / `view` / `eye` / `up` 必须彼此一致，否则 memo 的复用前提不成立；
   若无法保证，就退化为“含视图相关节点的场景不跨视图复用”。

## 验证建议

- 单测：构造两次 `CollectView`，使 `projection * view` 完全相同而 `view` 不同（如把 `projection` 与 `view`
  相应项移位补偿），场景含一个 `RotateToScreen` billboard；两次收集都命中 memo 时，断言第二次的 billboard 世界
  矩阵与单独走查的结果一致（当前会失败）。
- 同一 `view`、不同 `up` 的 `RotateToCamera` 版本同理。

## 设计约束

该问题涉及 **G4**（“memo 键 = 结果的全部输入”是隐含契约，却没写清也没实现全）、**G11**（错在画面里而不是错误里，
定位只能靠读 memo 代码）、**G12**（视图相关性的声明与它的缓存键不在同一处表达）。
