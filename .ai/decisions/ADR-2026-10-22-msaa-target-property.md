# ADR-2026-10-22：MSAA 是目标属性，颜色与深度各解析一张图

- 状态：已采纳
- 日期：2026-10-22
- 相关：`../design/graphics-rendering.md`

## 背景

样本数决定三件互相绑定的事：图像的样本数、render pass 附件描述的样本数、管线的
`rasterizationSamples`。Vulkan 要求三者相等，而图像形状归目标所有。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. 管线开关 | 调用简单 | 图像/通道/管线三者可能不一致；状态泄漏 |
| B. 目标属性 `setSamples` | 三者由目标形状统一驱动 | 目标重建时整批资源换 |
| C. 后处理 AA | 不改样本数 | 不是真 MSAA；边缘与深度不解决 |

## 决定

选 **B**：`RenderTarget::setSamples(n)` 是唯一入口；采样数进 `TargetShape` ⇒
`RenderPassCompatibility` ⇒ 管线与图像；多重采样目标自建 MS 图 + **颜色与深度各一张
单采样 resolve 图**（深度 `sample0`，不平均）。

## 后果

- 好：读回/采样/借出/提升都走 `picture()` / `depthPicture()`，样本数不分叉。
- 代价：多重采样目标不能**借入**深度；默认窗口仍单采样。
- 重开条件：需要窗口级 MSAA / 平台交换链样本数（目前不做）。

## 证据

- 1 样本边缘 0 个混合像素；4 样本 > 0；内部纯色。
- 4 样本深度无中间值；`MsaaTargetTest` 设备用例。
