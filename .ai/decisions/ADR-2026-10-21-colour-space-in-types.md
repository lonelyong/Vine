# ADR-2026-10-21：颜色空间与数值域放进类型

- 状态：已采纳
- 日期：2026-10-21
- 相关：`../design/graphics-rendering.md`

## 背景

颜色在 SDK 里同时以线性浮点、sRGB 字节、0–255、0–1 出现，靠注释区分；默认清屏灰
`#333333` 被当成线性值后又编码一次，亮了一个 gamma。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 只写注释 | 零改动 | 第二次同类缺陷必然再犯 |
| B. 两个类型 + 一对转换 | 编译期区分；转换有名字 | 调用点要改；多一个类型 |
| C. 通用 `UnitScalar` 包装 | 理论上更强 | 过度设计；没有第二个消费者 |

## 决定

选 **B**：`Colorf` = 线性光浮点；`Color` = sRGB 编码字节；`Color::toLinear()` /
`Colorf::toColor()` 是唯一转换；SDK 作者颜色只剩 `Colorf`。

## 后果

- 好：清屏 / pass 颜色 / 默认色都是线性值；后端不再 `/255`；文本与字节序有明确入口。
- 代价：调用点要选类型；`Color` 的文本是编码那一半。
- 重开条件：需要目标格式的 sRGB 变体（目前不做）。

## 证据

- `ColorTest` / `ColorfTest`（线性 0.5 → `#bc0000`；黑白 lerp 中点是 188 不是 128）。
- `VsgBackendTest.AClearColourIsAStoredLinearValueNotAnEncodedOne`（读回 55 而不是 128）。
