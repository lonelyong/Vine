# vsg 后端自定义顶点属性（任意 location）设计方案

> 状态：已实现（2026-09-08，复核批次 A 后同步）。权威来源 = `SceneBridge.cpp` / `VsgRenderer.cpp`；
> 本文件为契约与取舍说明。承接 `vsg-custom-shader.md` / `vsg-pipeline-sharing.md` /
> `docs/data-flow.md`。目标目录 `src/plugins/gfx_backend_vsg`。
> 本方案**不改动 SDK 公开 API**、**不破坏** Phong/FlatShaded、动态 opacity、pipeline sharing。

## 0. 实现核对（2026-09-08，批次 A 已落地）

- **索引流保真**：数据构建不再按三角形截断/拒绝索引；`DrawIndexed` count == 源索引数（仅越界索引拒绝）。
  法线推导仅 Triangles；Points/Lines 缺法线时用常量 `(0,0,1)`（topology 是数据身份输入）。
- **loc2 最终语义**：内建路径恒用后端内部白 DYNAMIC carrier（改写用户 loc2 alpha 会污染共享 Geometry 数据），
  authored loc2 忽略；自定义路径（程序自管 opacity）存在且有效的 loc2 绑定为 `vine_Color`（保留 authored alpha），
  否则白。携带 loc2 的 mesh 在 built-in/custom 间切换会重建数据节点。
- **L1a/L1b 已实现**：`compileProgramStages`（按 program+revision 一次）+ `assembleProgramShaderSet`
  （按 program+revision+layout 装配）；诊断计数 `SceneBridge::programStageCompileCount()` 供测试断言。

## 1. 问题与现状

- 数据模型（`Geometry`，`src/viz/graphics/sdk/vine/graphics/Geometry.hpp`）本已支持
  **任意 location 的 open attribute 表**（`std::map<uint32_t, AttributeChannel> attributes_`，
  `AttributeChannel{ components 1..4, data }`），文档明示"后端无关的自定义逐顶点通道"。
- 但后端消费端只用了 0/1：
  - `SceneBridge::buildGeometryData()` 只读 `buffer(0)`（位置）、`buffer(1)`（法线），其余 location 丢弃；
  - 顶点数据节点固定绑 3 个数组 `[vertices, normals, colors]`（`colors` 为内部白/opacity 载体）；
  - `buildProgramShaderSet()` 只为自定义程序声明 `vine_Vertex(0)/vine_Normal(1)/vine_Color(2)` 三个绑定，
    format 写死（0/1 恒 `R32G32B32`）；
  - `buildStateGroup()` 只按 `arrays[0..2]` 固定名 assign。
- 结果：`ShaderProgram` 只携带 GLSL stages（`ShaderProgram.hpp` 无 attribute 声明概念），
  后端也无从把 location≥3 的 buffer 喂给自定义着色器 → 特性端到端断链。

## 2. 设计目标与约束

1. **保持内建语义**：0=position、1=normal、2=color（`vine_Vertex` / `vine_Normal` / `vine_Color`），
   Phong/FlatShaded/动态 opacity 行为不变。
2. **任意 location 可透传**：对自定义 `ShaderProgram`，几何的每个 attribute buffer 按
   (location, components) 生成 vsg 顶点数组并声明对应绑定。
3. **绑定名稳定规则**：内建 0/1/2 名不变；额外 location L 用 `vine_Attribute{L}`。
4. **不改 SDK 公开 API**：规则收敛在后端 + 文档；`Geometry` 已能表达任意 location，
   `ShaderProgram` 维持"源码即契约"。
5. **pipeline sharing 语义正确**：顶点布局（绑定的属性集合）不同 ⇒ 不能共享同一变体模板。

## 3. 关键 vsg 机制（先厘清）

- vsg 的顶点输入布局由 `ShaderSet::attributeBindings` 决定（每个 binding 带 location+format+样例 Data）；
  `GraphicsPipelineConfigurator::assignArray(name, …)` 把实际 Data 数组按 **name** 挂上，
  `copyTo` 时按 ShaderSet 的 location 生成 vertex input。**ShaderSet 名只需与 assignArray 的 name 一致**，
  不校验 GLSL 输入变量名 —— 用户 GLSL 只须 `layout(location=N) in …` 与绑定 location 对上。
- Vulkan 允许 pipeline 声明**未被着色器消费**的顶点属性（多余绑定合法）；
  但**着色器读了、pipeline 却没提供**该 location 会在 `vkCreateGraphicsPipelines` 报 validation 错
  （开发者错误，可在管线编译期暴露）。
- 结论：绑定集合应从"几何实际提供的 location"推导；GLSL 读得更多属于作者错误，文档言明即可。

## 4. 绑定名与 format 规则（后端内部约定）

| location | 绑定名            | 来源                          |
|----------|-------------------|-------------------------------|
| 0        | `vine_Vertex`      | 几何 loc0（强制，3/4 分量取 xyz）|
| 1        | `vine_Normal`      | 几何 loc1（可选，缺失则推导）   |
| 2        | `vine_Color`       | 内建路径=内部白 opacity 载体；自定义路径=几何 loc2（若有）否则白 |
| L≥3      | `vine_Attribute{L}` | 几何 loc L（按 components 映射 format）|

components→Vulkan format：`1→R32_SFLOAT`、`2→R32G32_SFLOAT`、`3→R32G32B32_SFLOAT`、`4→R32G32B32A32_SFLOAT`。
（`#2` 已把 loc0/1 解包改为按 components 跳步 + vec4 取 xyz；本设计把同一规则推广到所有 location。）

## 5. 数据节点与状态包装的解耦（沿用现有架构）

- **数据节点（`buildGeometryData` 产物，program 无关）** 改为"几何提供的超集"：
  仍绑 `[vertices, normals, colors]`，其后按 **location 升序**追加每个额外 buffer 的 typed 数组
  （`floatArray`/`vec2Array`/`vec3Array`/`vec4Array`，按 components）。这样：
  - 一个几何在内建/自定义程序间切换（program swap）**只重建状态包装、不重建/重传数据**（现有 Item 契约）；
  - 内建 Phong 路径不读 extra，多余绑定 Vulkan 视为未用，行为不变（需回归确认 vsg 不报多余绑定）。
- 需要把"数组下标→location"显式记录（否则下标无法推出 location，location 可能不连续）。
  在 `Item` 增加一个小的 `std::vector<uint32_t> attribute_locations;`（第 i 个绑定数组对应的 location，
  前三个固定 0/1/2），`data_dirty` 重建时一并填写，`buildStateGroup` 据此把数组按名 assign。

## 6. 自定义程序 ShaderSet：按 (program, 布局) 建集并缓存

- 当前 L1 把程序 ShaderSet 按 program 缓存并跨几何共享 —— 与"每几何布局不同"冲突。
  改为 **两级**：
  - L1a（保留）：每 program 的 glslang 编译缓存（stages 缓存，防重复编译）；
  - L1b（新增）：**组装后的 ShaderSet** 按 `(program, layoutSignature)` 缓存。
    `layoutSignature` = 几何提供的位置排序列表 + 各自 components 的哈希；
    命中即复用同一个 `vsg::ShaderSet`（相同布局的几何仍共享一套绑定对象）。
  - 组装 = 以该 program 的 stages 为底，声明 0/1/2 内建绑定 + 每个额外 location L 的
    `vine_Attribute{L}`（format 由 components 映射），material 描述符与 `pc` push-constant 不变。
- **保持 glslang 编译次数**：stages 只编译一次（L1a），仅装配对象按布局多份 —— 编译开销不随几何涨。
- 若几何缺少着色器需要的某 location：该几何的 ShaderSet 不声明它 ⇒ 管线编译期报 validation 错。
  可选增强（后续）：从 GLSL 文本反射 `layout(location=N)` 输入，仅声明"几何提供 ∩ 着色器需要"，
  并对"着色器需要但缺失"打一条一次性诊断而非撞管线 —— 列为可选、不阻塞 v1。

## 7. 变体缓存（L2）必须纳入布局

- `variant_cache_` 的键当前是 (program, material, state) 哈希，VariantEntry 存可复用 bind 命令 + 原型数组状态 +
  `baseAttributeBinding`。**绑定数量/顺序不同 ⇒ 原型数组状态不同**，共享模板会错位。
- 改法：把 `layoutSignature` 并入 `hashStateVariant()` 的键，并在 `VariantEntry` 存一份签名做命中相等校验
  （与现有 program/material/state 三重校验同风格）。布局相同才走 L2 快速路径。

## 8. buildStateGroup 装配

- 依据 `Item::attribute_locations`：`arrays[0]→vine_Vertex`、`[1]→vine_Normal`、`[2]→vine_Color`，
  第 i≥3 个数组 → `vine_Attribute{locations[i]}` 逐个 `assignArray`（仅当 ShaderSet 声明了对应名，
  否则跳过 —— 内建路径就是"只 assign 0/1/2"现状）。
- `vine_Color` 语义区分：
  - 内建/opacity 路径：`colors` 仍是内部白 DYNAMIC 载体（行为不变）；
  - 自定义路径：几何自带 loc2 时用其数据（静态），否则维持现内部白（程序拥有 opacity 时不用 carrier）。

## 9. 边界与风险

- **内建路径**下数据节点多绑 extra 数组：需回归确认 vsg `copyTo`/`BindVertexBuffers` 对
  "pipeline 未消费的多余 binding" 无 validation 噪音；若有，则内建路径在数据节点裁剪 extra、
  仅在 program 切换需要时补建（回到"program swap 需重建数据"的小代价分支）。优先验证"多余绑定无害"。
- **deferred/MRT**：extra 只是增加 vertex binding，与多目标无关，不受影响。
- **自定义着色器命名**：文档要求作者用 `layout(location=N)`（名字任意），v1 不解析名字。
- **公开 API**：v1 零改动；若后续要"程序自描述布局/编译期校验"，再议 `ShaderProgram` 增加
  attribute 描述符（模型层变更，独立立项）。

## 10. 落地清单（后续执行）

1. `SceneBridge.hpp`：`Item` 增 `attribute_locations`；`buildGeometryData` 记录；私有 helper
   `formatForComponents()` / `makeTypedArray(components)`。
2. `SceneBridge.cpp`：
   - `buildGeometryData`：枚举 `geometry->bufferLocations()`，对 loc0/1 走现逻辑（components stride），
     其余按 components 建 typed 数组追加绑定；
   - `buildProgramShaderSet`/新 `getProgramShaderSet(program, signature)`：按布局声明 `vine_Attribute{L}`；
   - `buildStateGroup`：按 locations 逐个 `assignArray`；
   - `hashStateVariant` + `VariantEntry`：并入 layoutSignature。
3. 测试（device-free，仿 `GeometrySafetyTest`）：
   - loc3 components=1/2/3/4 → 生成对应 typed 数组且已绑定；
   - 同 program + 同布局 N 个几何 → 共享 ShaderSet/管线（variant 计数不变）；
   - 同 program、一个多 loc3 一个没有 → 两个布局 ⇒ 各自变体（variant 计数 +1、无串用）；
   - 内建路径带 loc3 数据 → 仍正常绘制（Phong 变体数不变、根节点在）。
