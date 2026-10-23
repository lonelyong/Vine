---
name: design-docs
description: |
  **DOCUMENTATION WORKFLOW** — Create, update, split, merge or move repository design docs,
  ADRs, backlog entries, memory notes and bug postmortems.
  Use when: writing or editing `.ai/design/*.md`, `.ai/decisions/*.md`, `.ai/backlog/*.md`,
  `.ai/memory/*.md` or `.ai/bugs/*.md`; the user asks "设计文档怎么写"、"整理设计文档"、
  "把待办移出去"、"补个 ADR"、"这份文档放哪"、"合并设计文档"、"更新 copilot instructions".
  Enforces: current design vs history vs backlog separation; one fact one home; numbered
  design points / features / interfaces / caveats / rejected options / tests; no dates in
  design or backlog filenames; ADR for why and rejected alternatives; git is the archive.
---

# Design Docs: 目录、模板与迁移

## 0. 一句话

**design 写“现在是什么”，decisions 写“为什么、否决了什么”，backlog 写“还没做什么”，
memory 写“现役要点与坑”，bugs 写“事故复盘”，git 写“全部历史”。**

## 1. 先选目录

| 内容 | 目录 | 命名 | 带日期 |
|---|---|---|---|
| 现行设计、契约、架构、类图、接口、否决项摘要 | `.ai/design/` | `<area>-<topic>.md` | **否** |
| 一个决策的完整论证（选项、代价、后果） | `.ai/decisions/` | `ADR-YYYY-MM-DD-<slug>.md` | **是** |
| 开放的、触发条件型工作 | `.ai/backlog/` | `<area>.md` | **否** |
| 现役要点、坑、命令 | `.ai/memory/` | `<area>.md` | 否 |
| Bug 复盘 | `.ai/bugs/` | `<area>-<slug>.md` | 可带 |
| 调研、提案、实验 | `.ai/research/` | `<area>-YYYY-MM-DD-<slug>.md` | **是** |
| 使用者文档 | `docs/guides/` | 按指南命名 | 否 |

**不要**把待办、日期、已关闭项、历史叙述放进 `.ai/design/`。
**不要**给 design / backlog 文件名加日期后缀。
**不要**在 design 里复制头文件契约或测试细节；给指针。

## 2. Design 文档模板

每个设计文档用同一套编号章节；章节里有子项就继续用 `1.1` / `1.1.1`。
编号一旦被别的文档引用就稳定，不重排。

```md
# <主题>

> **状态**：现行 / 草案 / 已取代（→ 后继文档）
> **最后核对**：YYYY-MM-DD
> **范围**：<一句话：管什么、不管什么>
> **权威来源**：<头文件 / 测试 / 脚本>
> **待办**：`../backlog/<area>.md`

## 1. 概要设计

### 1.1 问题与背景
### 1.2 目标
### 1.3 非目标
### 1.4 一句话结论

## 2. 设计要点

### 2.1 <要点>
- 是什么：
- 为什么：
- 边界：

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | ... | ... | 已落地 / 部分 / 未做 |

### 3.1.1 F1 <功能名>

- **行为**：
- **约束**（C1）：
- **要求**（R1）：
- **验收**（T1）：
- **注意**（N1）：

## 4. 公开接口

### 4.1 <类 / 模块>

| 接口 | 契约 | 备注 |
|---|---|---|
| ... | ... | 完整签名见头文件 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）
### 5.2 要求（R#）
### 5.3 不变量（N#）

## 6. 注意事项与陷阱

### 6.1 <主题>
- 会怎么错：
- 怎么判：
- 怎么防：

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
|---|---|---|---|
| D1 | ... | ... | `../decisions/ADR-...md` |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | ... | ... |

## 9. 状态与演进

### 9.1 当前状态
### 9.2 已知缺口 / 待办
### 9.3 兼容性 / 迁移

## 10. 参考

### 10.1 代码
### 10.2 文档与 ADR
### 10.3 外部资料
```

规则：
- 编号 ID 稳定，供别的文档引用（"见 N4"）。
- 功能点按主题分组；先给索引表，再给每个功能点的行为/约束/要求/验收/注意。
- 共享的约束、要求、不变量放 §5，用 `C#` / `R#` / `N#`；功能点引用它们，不重复。
- 公开接口按类分组；完整签名看头文件。
- 注意事项每条写“会怎么错 / 怎么判 / 怎么防”，不写泛泛而谈。
- 否决项只留一行；完整论证链接 ADR。
- 小文档可把 1.1–1.3 合成一段，但 §1/§2/§3/§5/§7/§8/§9/§10 的标题保留。
- 图必须与当前代码一致；改代码时更新“最后核对”。
- 状态是文档的一部分：`现行` / `草案` / `已取代`，取代时链接后继文档。

## 3. ADR 模板

```md
# ADR-YYYY-MM-DD：<决策>

- 状态：已采纳 / 已取代（被 ADR-... 取代）
- 日期：YYYY-MM-DD
- 相关：`../design/<doc>.md`、测试 / 测量

## 背景

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A | ... | ... |
| B | ... | ... |

## 决定

## 后果

- 好的：
- 坏的 / 代价：
- 触发条件（什么时候重开）：

## 证据

- 测试 / 门禁 / 测量数字 / 代码位置
```

只给**可能被重新讨论**的决策写 ADR。不要把逐片实施记录写成 ADR。

## 4. Backlog 模板

```md
# <模块> 待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

## 1. 现在可做（无触发条件）

空 / 具体项。

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | ... | ... | ... | `../design/<doc>.md` §... |
```

规则：
- 一条一个触发条件，写成**可观察的事实**（某个数字、某个调用点、某台机器）。
- 关闭、明确不做、已被取代的条目**删除**，不写“已关闭”。
- 文件不带日期；日期只在条目正文里出现（如某次测量）。

## 5. 迁移与整理

### 从 design 移出去

- 待办 → `backlog/<area>.md`。
- 历史论证 / 为什么否决 → `decisions/ADR-...md`。
- 调研 / 提案 → `research/`（采纳后把结果写进 design，原文可删）。
- 纯坑 / 命令 → `memory/`。

### 合并

- 同一主题的多份 design → 合并成一份 `<area>-<topic>.md`。
- 合并后删除旧文件；所有引用改到新文件。
- 历史在 git，不保留“历史登记”副本。

### 删除

- 已关闭 / 已明确不做 / 已被取代：直接删除。
- 需要保留“为什么”的：只写 ADR。
- 删除后更新 `.ai/README.md` 与 `scripts/check_ai_docs.py` 能发现的引用。

## 6. 必须跑的检查

```bash
python3 scripts/check_ai_docs.py --strict
python3 scripts/check_design_docs.py
python3 scripts/check_doc_symbols.py
python3 scripts/check_public_docs.py
```

- `check_ai_docs.py`：`.ai/` 下每份 `.md` 必须在 `.ai/README.md` 被点名；现行段落的引用必须存在。
- `check_design_docs.py`：每份 design 文档的顶部块、10 章编号、子序号、行数与
  `F/C/R/T/N/D` ID；`--strict` 把行数/ID 警告也变成失败。
- `check_doc_symbols.py`：`docs/guides/` 的 living 文档点名的单元必须存在。
- `check_public_docs.py`：公开面文档义务基线不能变坏。
- 纯文档改动不跑编译 / 测试；改到源码注释时按仓库规矩决定是否 build。

## 7. 自查清单

- [ ] 文件名不带日期（design / backlog）。
- [ ] 一份文件只讲一个主题；≤300 行。
- [ ] 顶部有状态 / 最后核对 / 范围 / 权威来源 / 待办。
- [ ] §1–§10 的标题齐全（小文档可合并 1.1–1.3，但不删章节）。
- [ ] §3 有功能索引表；每个 `F#` 有行为 / 约束 / 要求 / 验收 / 注意，或引用 §5 的共享 C#/R#/N#。
- [ ] 接口按类分组；完整签名看头文件。
- [ ] 注意项写清失败形态 / 判据 / 预防。
- [ ] 否决项一行 + ADR 链接。
- [ ] §9 写当前状态、已知缺口、兼容/迁移。
- [ ] 图与代码一致；“最后核对”已更新。
- [ ] `.ai/README.md` 已点名新文件。
- [ ] 三个文档门禁通过。
