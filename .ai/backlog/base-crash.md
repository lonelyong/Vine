# base/crash 待办

> 只列开放项；没有触发条件就不做。关闭项直接删除。

## 1. 现在可做（无触发条件）

空。当前形态（报告 + dump + 窗口 + 符号化）已经能回答"崩在哪"。

## 2. 触发条件型

| # | 项 | 内容 | 触发条件 | 出处 |
|---|---|---|---|---|
| T1 | 上次崩溃提示 | 启动后检查未看过的报告，主窗口非模态提示"上次崩溃了，要不要打开报告" | handler 经常起不来（`CreateProcess` 失败/被杀），或用户反复说"崩了就没了" | `../design/base-crash.md` §9.2 |
| T2 | `std::set_terminate` | 未捕获的 C++ 异常、`std::thread` 里抛出的异常也走同一条报告路径 | 出现"进程直接消失、没有报告"的 bug 且栈里能看到 `terminate` | 同上 |
| T3 | 栈耗尽保证 | `SetThreadStackGuarantee` + 溢出时另开线程写报告 | 出现 `0xC00000FD` 崩溃且报告缺失或被截断 | 同上 |
| T4 | 只读安装目录回退 | 装到 `%ProgramFiles%` 时报告改写用户数据目录 | 有安装包把产品装进只读目录，且用户机器上找不到报告文件 | 同上 |
| T5 | Linux `sigaction` | `sa_sigaction` + `backtrace`/`backtrace_symbols_fd` 填同一个 `Report` | 第一次正式在 Linux 上发布 | 同上 |
| T6 | 改用 Crashpad / Breakpad | 换掉自研链路，接全套 handler + dump 工具链 | ① 需要跨平台同格式 minidump；② 需要从用户机器上报/聚合（含接 Sentry 之类）；③ 需要符号服务器或批量符号化 | `../design/base-crash.md` §7 D8 |
| T7 | 邮件/打包上传 | 把 `.txt` + dump 打包并发到一个确定的支持渠道 | 支持渠道与收件人定下来，且"用户手动拖附件"实测失败率高 | `../design/base-crash.md` §2.6 |
| T8 | dump 解析 | handler 自己读 minidump 走栈（不依赖崩溃进程记录的帧） | 报告里的帧不足（例如栈缓冲被截断）成为定位瓶颈 | `../design/base-crash.md` §7 D9 |
