# ADR-2026-10-24：灯块改为「带类型的灯数组」，两条路径共用 UBO

- 状态：已采纳
- 日期：2026-10-24
- 相关：`../design/graphics-shader-params.md` · `../design/graphics-rendering.md` · `../backlog/graphics.md`

## 背景

旧的 `VineLightsBlock` 是「1 盏环境光 + 3 盏方向光」（112 B）。内容侧的 `LightType` 却有四种，而
`Light` / `LightRef` 都没有位置；打包把类型拍平（`Ambient` 进环境槽、`Directional` 进 0..2、其余丢弃），
全屏路径把同一组数字压进 128 B push（`LightPushBlock`，其中 16 B 的 `projparms` 恒为 0 且无人读）。
后果有三个：

- 接口承诺了表达不出的东西（点光/聚光有名字、没有数据）；
- 同一组灯数字有两种形状（UBO 与 push），超 3 盏时 content 路径报掉落、全屏路径不报；
- 「位置」这个概念从未进入 ABI，加灯类型不是加字段而是换布局。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 保持方向光，点光以后再说 | 不动 ABI | ABI 要破两次；枚举继续承诺不存在的效果 |
| B. 带类型的灯数组，两条路径共用 UBO（选定） | 一次改对；删掉 `LightPushBlock` 与死字段；一份打包 | 块 112→528 B；全屏路径多一个灯块 binding |
| C. 全屏路径继续 push，只给 content 加类型 | 全屏改动小 | 两种灯模型；点光在全屏路径仍不可用 |
| D. 同时做逐灯阴影（点光 cube / 聚光透视） | 完整 | 独立能力，会把这一批撑爆；阴影 ABI 与光相机都要重做 |

## 决定

1. **块形状**：`VineLightsBlock` = `vec4 ambient` + 五个 `vec4[8]` 平行数组
   （`position_kind` / `direction_range` / `color_intensity` / `cone`），528 B。不用 struct 数组：ABI 扫描器的
   成员表只认内建类型，`VineLight lights[8]` 会被判成无法测量的声明。
2. **类型 tag**：`position_kind.w` = `LightType` 的数值（Directional=1 / Point=2 / Spot=3），空槽 = -1。
   `LightType` 显式赋值（0/1/2/3）把 ABI 钉住。
3. **视图空间**：位置与方向都在打包时 world→view，shader 因此不需要额外的矩阵（与今天的方向规则一致）。
4. **一个 binding 服务两条路径**：灯块在 set 0 **binding 5**（content 的 shadow 对留在 3/4，2 因此空出；
   全屏的 source 颜色在 0..N-1、深度在 4、灯在 5、shadow 对后移到 6/7）。片段里没有可参数化的 binding，
   ABI 扫描器也不做宏求值——同一个数字是唯一不引入第二份声明的办法。
5. **两条路径都用 UBO**：`LightPushBlock`、`packLightPushBlock` 与 `projparms` 删除；全屏 pipeline 不再声明
   push range，`ScreenDraw::push` 留给宿主程序。
6. **选灯仍是「按声明顺序」**，掉落的灯在两条路径都按 episode 报（全屏路径此前静默）。
7. **shading 落地**：点光/聚光的求值（方向、距离衰减、锥体）在 `vine_params.glsl` 的
   `vnEvaluateLight` 一处，forward 与 deferred 各自只保留自己的材质路径。
8. **逐灯阴影不做**：点光/聚光即使请求 `setCastShadow` 也不会有 caster（阴影仍是方向光的正交相机），
   该请求被报出来，不静默。

## 后果

- 好：一个灯模型、一份打包、一条掉落规则；块里首次有位置；`projparms` 的死预算消失；全屏路径也能报掉落。
- 代价：`VineLightsBlock` 大小 112→528（`BlockStorage` 灯区、`ProgramAbi` 断言、`LightBlockTest` 跟着改）；
  全屏 pipeline layout 多一个 dynamic uniform binding；8 槽是硬上限（超出可见地报）。
- 重开条件：逐灯阴影、逐对象选灯（灯表/簇）、区域光。三者各自需要新的设计，不共用这一批。

## 证据

- `Light.hpp`：`LightType{Ambient, Directional, Point(reserved), Spot(reserved)}`，`Light` 只有 `direction_`；
- `FrameRecorder.hpp` 的 `LightRef` 只有 `has_direction` / `direction`；
- `LightBlock.cpp` 的 `packLightBlock` 对 `Point`/`Spot` 走 `default: break`；
  `LightBlockTest.DisabledAndUnusableLightsAreNotPacked` 钉住「a kind the block has no slot for」；
- `ContentPass.cpp` 全屏路径的注释明说掉落「deliberately NOT reported」；
- `LightBlock.hpp`：`projparms` 是 RESERVED 且恒 0，没有 shipped program 读它；
- `ProgramAbi.cpp` 的 `memberTypeLayout` 对 struct 成员返回 false（因此块形状避开 struct 数组）。
