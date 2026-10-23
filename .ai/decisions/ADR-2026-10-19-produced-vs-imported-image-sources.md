# ADR-2026-10-19：produced 与 imported 图源不合并

- 状态：已采纳
- 日期：2026-10-19
- 相关：`../design/graphics-rendering.md`

## 背景

pass 的输入图有两种来源：帧内某条 pass 产出的 `RenderTarget` 附件（`ImageRef`），
以及宿主持有的 `Texture`。两者的生命周期、producer 规则、失效方式都不同。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 两个平行列表 | 类型清晰 | 混排顺序丢失；消费方遍历两次 |
| B. `ImageRef` 增加 `bind(Texture)` | 一个列表 | 联合体；大量 `if (texture)`；producer 规则不成立 |
| C. `ImageInput` 声明 + `ImageSource` 解析值 | 统一“输入图”的形状，保留两种生命周期 | 多一个值类型 |

## 决定

选 **C**：`ImageInput` 持有 `ImageRef` 或 `Texture`（+ 可选 `Sampler`）；`ImageSource`
是解析后的 produced / imported 值；produced 参与一帧一个 producer 的 wiring，imported
走 `MaterialImages` 上传缓存。

## 后果

- 好：content 与 full-screen 两条绑定路径共享同一套输入声明；sampler 随输入携带。
- 代价：全屏 ABI 的 “binding N = attachment N” 作为显式兼容路径保留。
- 重开条件：需要数组 / 3D 纹理输入（目前不做）。

## 证据

- `FrameCompilerTest.AnImportedTextureInputIsAFirstClassInputWithNoProducerEdge`。
- `VsgBackendTest` imported texture / cube / sampler state 用例。
- `ScreenDrawTest` 全屏绑定顺序不回归。
