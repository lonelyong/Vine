# ADR-2026-10-22：缓存不设上限，释放显式管理

- 状态：已采纳
- 日期：2026-10-22
- 相关：`../design/graphics-content-lifecycle.md`

## 背景

纹理缓存有 256 条上限 + FIFO/LRU 淘汰；材质 arena 有 FIFO。300 张同序循环的实测显示：
工作集 > 容量时每轮全重建，LRU 与 FIFO 表现相同；上限是替宿主做决定。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. LRU | 常见局部性下命中率高 | 300 张同序循环下与 FIFO 一样；仍要上限 |
| B. 字节上限 + 压力淘汰 | 内存可控 | 替宿主决定；压力信号不准 |
| C. 不设上限 + 显式释放 | 有多少用多少；宿主控制寿命 | 宿主必须调用释放 |

## 决定

选 **C**：`MaterialImages` 删上限与淘汰；`MaterialArena` 删 FIFO，槽满 `Refused`（不淘汰
活材质），录制前按活材质数扩 storage（无硬上限）；`releaseContent()` 释放 GPU 驻留。

## 后果

- 好：没有“旧贴图被莫名重建”；释放路径清晰；材质数量不再受硬上限。
- 代价：宿主负责内存；忘了释放就驻留。
- 重开条件：宿主报告内存压力且显式释放不够（目前没有）。

## 证据

- 300 张纹理超过旧上限后第二轮 0 缺失。
- 200 / 300 / 2000 个材质扩容后 `write/f=0`、`refus/f=0`。
- `ThreeHundredLiveMaterialsDrawTheirOwnColours`；`test_vsg` 498、`test_graphics` 441。
