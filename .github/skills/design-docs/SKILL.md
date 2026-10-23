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

```md
# <主题>

> **一句话**：<结论>
>
> **权威来源**：<头文件 / 测试 / 脚本>
> **最后核对**：YYYY-MM-DD
> **待办**：`../backlog/<area>.md`

## 1. 设计要点

1.1 ...
1.2 ...

## 2. 功能点

### 2.1 <分组>

| # | 功能 | 说明 |
|---|---|---|
| F1 | ... | ... |

## 3. 公开接口

### 3.1 <类 / 模块>

| 接口 | 作用 |
|---|---|
| ... | ... |

## 4. 注意项 / 不变式

| # | 注意项 |
|---|---|
| N1 | ... |

## 5. 否决项

| # | 否决 | 理由 | ADR |
|---|---|---|---|
| D1 | ... | ... | `../decisions/ADR-...md` |

## 6. 判据

| # | 判据 |
|---|---|
| T1 | ... |

## 7. 图

Mermaid 图放在对应小节旁边，不要堆到文末。类图 / 时序图 / 状态图按问题选一种。

## 8. 相关文档
```

规则：
- 编号 ID 稳定，供别的文档引用（"见 N4"）。
- 功能点按主题分组；一张表一眼能看出有哪些能力。
- 公开接口按类分组；完整签名看头文件。
- 注意项写“会怎么错 / 怎么判”，不写泛泛而谈。
- 否决项只留一行；完整论证链接 ADR。
- 图必须与当前代码一致；改代码时更新“最后核对”。

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
python3 scripts/check_doc_symbols.py
python3 scripts/check_public_docs.py
```

- `check_ai_docs.py`：`.ai/` 下每份 `.md` 必须在 `.ai/README.md` 被点名；现行段落的引用必须存在。
- `check_doc_symbols.py`：`docs/guides/` 的 living 文档点名的单元必须存在。
- `check_public_docs.py`：公开面文档义务基线不能变坏。
- 纯文档改动不跑编译 / 测试；改到源码注释时按仓库规矩决定是否 build。

## 7. 自查清单

- [ ] 文件名不带日期（design / backlog）。
- [ ] 一份文件只讲一个主题；≤300 行。
- [ ] 结论在最上面；历史不在 design 里。
- [ ] 功能点有编号、分组；接口按类分组。
- [ ] 注意项写清失败形态 / 判据。
- [ ] 否决项一行 + ADR 链接。
- [ ] 图与代码一致；“最后核对”已更新。
- [ ] `.ai/README.md` 已点名新文件。
- [ ] 三个文档门禁通过。
