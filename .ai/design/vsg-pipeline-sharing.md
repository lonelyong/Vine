# 管线共享 / 变体缓存（历史登记）

> **历史登记（2026-09-25 收口）**：旧后端的"维度归位"（什么算管线身份、什么不是）与 L1/L2 变体模板缓存设计
（2026-09-08 落地）。旧实现已删除，**本文不再更新**；全文在 git 历史：
`git show 26861de:.ai/design/vsg-pipeline-sharing.md`。

新世界的同一问题：

| 旧主题 | 新家 |
| --- | --- |
| 什么进管线身份（管线键） | `include/vine/vsg/core/Keys.hpp`（决定与理由写在头文件里）+ 设计文档 §5.2 |
| 变体缓存（编译 / 复用 / 淘汰） | `VariantPool`（容量 65、FIFO；计数 `created()/reused()/evictions()`）+ `ContentPipeline` —— `docs/data-flow.md` §3 |
| "哪些算兼容必须由键回答完整，视图救不了键" | 设计文档 §5.2（坑） |
