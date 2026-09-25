# 自定义顶点属性（历史登记）

> **历史登记（2026-09-25 收口）**：旧机制 —— 任意 `location` 的自定义通道（绑定名 / 格式规则、与状态包装解耦、
按 (program, 布局) 建 ShaderSet 并缓存）。旧实现已删除，**本文不再更新**；全文在 git 历史：
`git show 0e99623:.ai/design/vsg-custom-attributes.md`。

新世界的同一问题：**几何自己的通道顺序（`location` 升序）= 绑定顺序**（`ContentFacts`；
设计与坑在 `.ai/design/vsg-reimplementation.md` §5.5）；"按名字声明才算数、按 L1 类型名认领块"
见 `docs/data-flow.md` §6；变体进键见 §5.2。
