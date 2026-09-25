# graphics / vsg 后端审查（2026-09-15；历史登记）

> **历史登记（2026-09-25 收口）**：一轮代码走查 + 修复，对象是旧实现（已删除）。**本文不再更新**；全文在 git 历史：
`git show 0b2f58c:.ai/design/graphics-vsg-audit.md`。

其中三条**与代码无关、仍然成立**的工程事实已并入 `.ai/design/vsg-reimplementation.md` §5.9：
证据门禁前必须整包 build（陈旧二进制会给出假证据）；本机 `http(s)_proxy` 出口坏（git 推拉要绕过）；
`vine_shader_check.sh` 需要 Linux `glslangValidator`（本机没有 ⇒ 变体编译由 `test_vsg` 覆盖）。
