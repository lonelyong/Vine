# vsg 后端：STEP 级元素选择 / 高亮设计（Phase 1 草案）

> 模块：`src/plugins/gfx_backend_vsg` + 平台层 `vn::graphics`
> 日期：2026-09-08
> 关联：`.ai/design/vsg-pipeline-sharing.md`（管线共享 / 数据·状态解耦 / D22 增量）、
> `.ai/design/vsg-user-mutation-strategy.md`（可变/可配情形策略）、
> `src/plugins/gfx_backend_vsg/docs/data-flow.md`。
>
> 一句话：**可选中元素数量不能成为场景图/渲染成本** —— 每个 STEP 部件 = 少数几个
> 大 drawable（base + highlight 子集），元素身份活在 CPU 元素表里，hover/选中只改一个
> “紧凑动态子集索引”，走 DYNAMIC+dirty 次帧回传；拾取用 BVH 命中 → triangleIndex → 元素 id。

## 1. 目标与规模假设

- STEP 部件：上万个**面**与**边**，每个都要求可单独 hover / 选中 / 高亮，稳态 60fps。
- 约束（为什么不能“每元素一个 drawable/节点”）：
  - 每帧 `collectRenderCommands` 全树遍历 → 元素级节点不可行（O(1e4+) 叶）；
  - draw call 与管线只应随**部件数与状态变体数**走（已由管线共享保证）。

## 2. 可复用基础（已就位，勿重复造）

| 已有能力 | 作用 |
|---|---|
| `SceneBridge` 数据/状态解耦（`data_node` 稳定、可复用） | highlight 可共享同一份顶点数据 |
| 颜色数组/材质 UBO 的 `DYNAMIC_DATA + dirty()` | “小数据热改次帧回传、未变零拷贝” 的模板 |
| L1/L2 缓存（状态变体级共享） | highlight 风格只是**新增状态变体** |
| D22 增量编译 | 首次建 highlight 只编新增子树 |
| `RayIntersection` 已带 `triangleIndex` | 拾取命中 → 元素表映射的现成入口 |
| `Topology::Lines` + per-geometry 状态 | 边高亮可直接走 Lines 变体 |

## 3. 数据表示：部件级大网格 + 元素表（平台层）

- **每个部件一个 `Geometry`**（复用现有 `Geometry::addBuffer/setIndices`）：
  - base 三角网：位置+法线+索引（一次上传、一条 draw）；
  - 边线：同一份位置 + 一份 `Lines` 线段索引（需要“边”为独立元素时）。
- **元素表（CPU，平台层持有，不进场景图）**：
  - `faceOfTriangle[tri]`：每三角形 → 面 id（面可含 1..k 三角形）；
  - `edgeOfSegment[seg]`：每线段 → 边 id；
  - 反查 `elementRanges[element] = [lo, hi)`（三角形/线段区间），用于把“元素集合”展开成紧凑索引列表。
- 好处：**元素数只活在 CPU 表**，与 GPU 上传、draw、管线解耦。

## 4. 渲染路径：base + highlight 子集 overlay

### 4.1 目标结构（后端 Phase 1 扩展点）
在现有 Item 之外/之内新增一个“子集 drawable”：

```
部件 subtree
├─ base        : 既有 data_node（顶点/索引） + state(默认变体)
└─ highlight   : 同一份 顶点/normal/color arrays
                 + 独立紧凑 uintArray（= 被高亮元素展开出的三角形/线段索引，DYNAMIC）
                 + 独立 state(高亮变体: fill+polygon offset / Lines)
```

后端要补的**最小原语**：让第二个 drawable 能
“引用 `data_node` 的 arrays，但绑定**自己的索引**” —— 即把 `buildGeometryData`
里“绑定哪份索引”参数化/抽出（顶点绑定复用，索引与 `DrawIndexed(count)` 独立）。

### 4.2 更新路径（热路径，仿透明度）
- hover（1 元素）/ 选中集变化 → 平台用元素表把 **变化的元素** 展开成紧凑索引列表；
- 就地覆写 highlight 的 `uintArray` 内容 + `dirty()`（标 `DYNAMIC_DATA`）；
- **不重建节点、不重编译**；未变化帧零拷贝；框选 1 万面时一次性写几十 KB，可接受。
- 稳态（hover 未跨元素）→ 零 CPU。

### 4.3 高亮风格 = 状态变体
- fill：半透明染色/发光 —— 深度测试开 + **polygon offset** 防 z-fight（需补一个后端保留状态或高亮专用 `ResolvedRenderState` 扩展）；
- edge：`Topology::Lines` 线框叠加（hover/选中轮廓）。
- 两者走现有 L1/L2 → 数量 = O(风格)，与元素数无关。

## 5. 状态与通道 / API（平台层草拟）

元素状态通道决定“颜色/风格”，与渲染结构解耦：

```
hover    = 1 元素（临时）
selected = 任意子集（持久）
grouped / preview / illegal（可选，各给一种颜色/风格）
```

草拟接口（Qt 风格命名，camelCase）：

```cpp
MeshPart::addFaceElements(ranges);   // 建 faceOfTriangle / 反查表
MeshPart::addEdgeElements(ranges);   // 建 edgeOfSegment / 线段表
Selector::setHover(part, element_id);
Selector::clearHover(part);
Selector::select(part, element_id);          // toggle / add
Selector::selectBox(...);  Selector::clearSelection();
```

底层保证：**任何一次状态变化的最坏成本 = 重算受影响元素的紧凑索引 + 一次小 IB 写**；
不触碰 base 网格、不重建、不编译、不重传顶点。

## 6. 拾取

- `RayIntersection` 命中已带 `triangleIndex` → 经 `faceOfTriangle[]` 得面 id；
- 规模化前提：**每部件一个 BVH（预构建，引用共享位置）**，避免每事件重新物化整份网格
  （现状 `meshOfGeometry` 每次调用重新物化 O(mesh)）；
- **hover 节流**：仅在“命中的元素与上次不同”时重算/更新；
- 细边精度：屏幕空间近邻容差二次测试（光线条 → 按投影距离取最近）；
- **可选方法 B（密集/细特征）**：id-buffer 点选 —— 元素 id 编码颜色渲染进小离屏 RT，点击读回即得 id（O(1)）；仅在点击时用，规避 readback 停顿。

## 7. 与 SceneBridge / 后端的接线清单

| # | 扩展 | 位置 |
|---|---|---|
| 1 | 抽出“共享 arrays + 独立索引”的子集 drawable 原语 | `SceneBridge`（data 构造旁） |
| 2 | highlight 索引数组 `DYNAMIC` + 就地写 + `dirty()` | 沿用透明度模板 |
| 3 | polygon-offset / 高亮保留状态（fill 不 z-fight） | `RenderStateMapper` / `ResolvedRenderState` 或后端保留项 |
| 4 | 元素表 + Selector（CPU） | 平台层（`vn::graphics` 或上层 CAD 模块） |
| 5 | BVH + 拾取返回 element id | 平台层拾取（复用 `RayIntersection`） |
| 6 | D22 增量不变；首建 highlight 只增一子集节点 | `VsgRenderer` |

## 8. 数量级与成本（1 部件 1 万面 + 1 万边）

| 项 | 值 |
|---|---|
| 场景节点 | O(部件数) |
| 每部件 draw | base 1 + fill 1 + edge(可选) 1 ≈ 2~3 |
| VkPipeline | O(风格)（共享后个位数） |
| hover 单元素 CPU | BVH 命中 + 几条~几十条索引写 |
| 框选 1 万面 | 一次性几十 KB 索引写 |
| GPU 顶点缓冲 | 每部件一份（highlight 复用，不复制） |

## 9. 分阶段落地

1. **Phase 1a（后端原语 + 最小端到端）**：实现“共享 arrays + 子集索引”drawable + DYNAMIC 更新；
   用一个“整部件单面 hover tint”demo 验证（不依赖完整元素表也可先验渲染正确）。
2. **Phase 1b（元素层）**：部件大网格 + 元素表 + BVH 拾取（`triangleIndex → face id`）+ hover 节流。
3. **Phase 1c（交互完备）**：selected 集、多选/框选、edge 拾取与轮廓高亮、多通道颜色/风格。
4. **远期（可选）**：per-element flag 通道 + 专用 selection program 一次 draw 全覆盖（依赖自定义属性通道 loc≥2 接线与 D1 修复）。

## 10. 风险 / 决策点

- **z-fight**：fill overlay 必须 polygon offset（补状态），否则闪烁。
- **透明/顺序**：fill 半透明需并入既有 transparent painter 序或作为 on-top 槽。
- **元素 id 稳定性**：部件网格重三角化（revision）会使 `faceOfTriangle` 失效 → 元素表随
  `Geometry` revision 版本化/重建。
- **子集索引上限**：框选极大集合时一次写较多索引——用脏写 + 分批上传兜底即可，暂不需 Indirect。
- **GPU 行为需真机复验**（headless 单测覆盖不到渲染）；沿用 env 逃生口/演示开关策略。
