# 仓库文档（`docs/guides/`）

面向使用者的文档统一放在这里（2026-10-11 从各模块自带的 `docs/` 目录搬来）。
`docs/` 同时是 GitHub Pages 站点根（`index.html` + `CNAME`），所以这些文档将来也能随站点发布；
设计记录与现役要点不在这里 —— 在 `.ai/design/`（设计）与 `.ai/memory/`（要点）。

| 文档 | 覆盖 | 先读它的人 |
| --- | --- | --- |
| [`graphics.md`](graphics.md) | graphics 模块：架构与使用 —— 分层、场景模型、每帧数据流、最小例子、手搭管线与 builder、自定义 shader、颜色空间、构建与跑法 | 写渲染代码的宿主 |
| [`render-views.md`](render-views.md) | 渲染视图模块说明：`Viewer` / `RenderEngine` / `SceneView` / `RenderArea` —— 架构图、类关系图、两格分屏 demo、一个窗口 N 格与多窗口立场 | 用 `RenderControl` 的宿主 |
| [`graphics-backends.md`](graphics-backends.md) | 图形后端：两个后端的共同契约、有意差异、支持矩阵、接缝与构建/证据 | 选后端 / 改后端的人 |
| [`eigen.md`](eigen.md) | math 模块的 Eigen 使用约定 | 写数学代码的人 |
| [`crypto.md`](crypto.md) | crypto 模块使用说明：要做什么该用哪个、流式对象契约、密钥与文件格式、线程与所有权、失败长什么样、不要做的事 | 写加解密 / 签名 / 密钥代码的宿主 |

约定：

- 文档之间用**同目录相对链接**互相引用（`[`graphics.md`](graphics.md)`）；
- 指向代码的链接用仓库相对路径（`../../src/...`），改代码位置时脚本 `scripts/check_doc_symbols.py`
  会盯住 `graphics-backends.md` 的单元名与 vsg 插件目录的一致性（两个方向）；
- 文档里出现"已经不存在的东西"，那一行加 `<!-- drift-ok -->`（规则同 `.ai/README.md`）。
