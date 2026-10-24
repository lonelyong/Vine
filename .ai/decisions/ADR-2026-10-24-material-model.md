# ADR-2026-10-24：材质改为一个 metallic-roughness PBR 模型 + 稀疏实例

- 状态：已采纳
- 日期：2026-10-24
- 相关：`../design/graphics-material.md`、`../backlog/graphics.md`（T16）

## 背景

引擎今天的材质是“平铺 Phong + 一张贴图”：`ambient / diffuse / specular / shininess`，材质 alpha
只运输不被读；导入的 PBR 资源在 `ModelScene.cpp::materialFrom` 里被近似成 Phong（metallic 折进 diffuse、
roughness 折进 shininess + specular 强度）。没有实例，逐对象高亮 / tint 只能克隆整个材质
（`ModelRenderView::selectAt`）。

backlog T16 的两个触发条件都已经出现：

1. 真实 PBR 需求：导入资源带 base color / metallic / roughness / emissive，近似在金属与粗糙表面上肉眼可见；
2. 逐对象参数出现第二个调用点：model_viewer 的选择高亮、test_plugin 的逐帧动画色。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保留 Phong，只加 `MaterialInstance` | 改动最小，G-buffer 与两条路径不动 | 真实 PBR 仍无法表达；近似逻辑永久留在导入器；两套材质语义 |
| B. 双模型（材质可选 Phong / PBR） | 旧资产零迁移 | 两套 block、两套 G-buffer 解释、两条路径 × 两个模型；`G1` `G6` 直接受损 |
| C. 单一 metallic-roughness PBR + `MaterialInstance` + alpha 模式 | 一个语义、两条路径一套 BRDF；实例消除克隆 | 公开 API 大改；透明规则要重写；G-buffer 要重排 |
| D. 单类 `Material` 带可选父节点（不做实例类型） | 少一个类型 | “覆盖了哪几项”与“改资产影响谁”成为运行期数据；`G1` `G4` |
| E. 逐纹理槽变体位 | 无纹理槽零成本 | 2^5 组合 × 路径 × 阴影，管线缓存与门禁矩阵爆炸；`G10` |

## 决定

采用 **C**，并做三个配套决定：

1. **一个着色模型**：metallic-roughness PBR 是引擎唯一的材质语义。`vn::geometry::PhongMaterial` 在导入
   边界按固定表转换，引擎内部不再有 Phong 分支。
2. **资产 / 实例两级**：`Material`（资产）与 `MaterialInstance`（稀疏覆盖）共享 `MaterialBase`；
   `RenderCommand` 持有 `MaterialBasePtr`。实例解析结果按（父 revision，自身 revision）缓存。
3. **纹理集 + 二元变体**：五个语义槽（base_color / metallic_roughness / normal / occlusion / emissive），
   每槽带 `Sampler`；`VINE_TEXTURED` 一个布尔位决定是否声明五个采样器，缺槽绑类型化常量回退。
4. **透明度**：`AlphaMode::Opaque | Mask | Blend` 在材质上；幅度仍是 drawable opacity；有效 alpha 是两者与
   base 贴图 alpha 的乘积；`isTransparent()` 同时读两个输入。取代旧规则“材质不贡献 alpha”。
5. **G-buffer**：四张附件重排为 albedo / 八面体 normal + roughness + metallic / emissive + occlusion /
   view position + mask；A2 升到 RGBA16F。

## 后果

- 好的：
  - 一个材质语义贯穿导入、收集、两条着色路径与测试（`G1` `G6`）。
  - 逐对象高亮 / tint 不再克隆材质，纹理只存一份（`G10`）。
  - 变体数量不随纹理槽增长（`G10`），无纹理内容保留零采样器快路径。
  - 排序与着色读同一对透明度输入，消除了潜在的“半透明不排序”（`G4`）。
- 坏的 / 代价：
  - `setDiffuse / setAmbient / setSpecular / setShininess` 删除，demo / 测试 / model_viewer 需要迁移。
  - deferred G-buffer 布局变化，`builtin_gbuffer.frag`、`builtin_deferred_lighting.frag` 与
    `ProgramAbi` 的绑定计数都要同步。
  - 有纹理的材质固定占五个采样器（缺槽绑回退），比今天“只有 base 槽才声明采样器”多四个绑定。
  - 法线贴图依赖屏幕空间导数切线基，镜像 UV 下是已知近似。
- 触发条件（什么时候重开）：
  - 有纹理材质的描述符压力/绑定量被实测证明是瓶颈 → 再评估逐槽变体或 bindless；
  - 出现 IBL / 环境探针需求 → occlusion/emissive 的 G-buffer 通道可能需要再加；
  - 出现 KHR_materials_unlit 资产的真实消费者 → 再讨论“着色模型是否是材质属性”。

## 证据

- 今天的近似：`src/viz/graphics/src/ModelScene.cpp` 的 `MaterialType::Pbr` 分支（metallic → diffuse 折减，
  roughness → shininess/specular 强度）。
- 逐对象高亮的克隆点：`src/plugins/model_viewer/src/ModelRenderView.hpp` 的 `selectAt`。
- 单贴图与 64 B 块：`src/viz/graphics/sdk/vine/graphics/Material.hpp`、`ShaderAbi.hpp` 的
  `VineMaterialBlock`、`shaders/vine_params.glsl`。
- 变体机制与门禁：`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/ProgramVariant.hpp`、
  `scripts/vine_shader_check.sh`。
- 采样器按值缓存已经存在：`src/plugins/gfx_backend_vsg/include/vine/vsg/internal/MaterialImages.hpp`
  （`acquire(texture, sampler, why)`）。
