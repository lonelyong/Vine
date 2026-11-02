# 命令参数 待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

参数**携带机制**已落地（见 `../design/appfw-command-arguments.md`）：一行文本切成 token、随执行帧送到
`CommandExecutionContext::arguments()`、历史与事件记录可回放的规范形。下面是还没有做的部分。

## 1. 现在可做（无触发条件）

（无）

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | 声明式参数 | `Command::parameters()` 返回 `CommandParameter{name, short_name, kind, position, required, default, description}`；注册时校验唯一性并在派发前按声明解析、填默认、报未知/重复/缺参。类型来自声明，不来自 token | 出现 3 条以上命令各自手写同样的 `-x value` 解析，或需要框架生成用法/补全 | `../design/appfw-command-arguments.md` §9 |
| T2 | 帮助与补全 | `help <command>` 与 `--help` 由声明生成参数表；控制台第二段起补参数名 | T1 落地后（没有声明就无可补全的东西） | 同上 |
| T3 | 缺参提问 | 框架在必选参数缺失时按 `description` 提问；编程入口直接 `Failed` + 用法。今天由命令自己用 `UserIO` 问（`test_createbox` 已是“给全就直跑、缺的才问”） | T1 落地后 | `../design/appfw-command-arguments.md` F5 |
| T4 | 对象参数 | 声明 `object_kind` 的参数以稳定 id 传递，派发时解析、命令按需再解析；`document`/`view` 折叠进 `CommandTarget` | 出现“按名字/编号指定文档、视图、插件”的真实命令 | `.ai/decisions/ADR-2026-10-24-command-target.md` |
| T5 | 参数长度上界 | 单条 `args_record` 与单次 token 总量的上界，防止超长输入占住历史 | 历史条目数上限之外需要守内存时（N6 从“条数有界”扩到“字节有界”） | `../design/appfw-command-manager.md` F5 |
