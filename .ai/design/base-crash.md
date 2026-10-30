# 崩溃报告（`vn::Crash` 与 `vnCrashHandler.exe`）

> **状态**：现行
> **最后核对**：2026-10-30
> **范围**：进程内崩溃捕获（Windows）、报告文本格式、`vnCrashHandler.exe` 的符号化与窗口。不含上报、聚合、跨平台 dump 格式。
> **权威来源**：`src/base/crash/sdk/vine/crash/CrashHandler.hpp`（契约）、`tests/test_crash/`（行为）。
> **待办**：`../backlog/base-crash.md`

## 1. 概要设计

### 1.1 问题与背景

`main.cpp` 曾自己写一份 Windows SEH 过滤器（约 160 行）：在主进程内用 DbgHelp 走栈、写一份未符号化的 `vn_crash.log`、然后退出。
它有三个问题：崩溃路径上碰 DbgHelp 与堆（本身危险）、没有 dump（拿不到现场给调试器）、用户看不到任何界面也不知道文件在哪。

### 1.2 目标

- 一次崩溃留下三样东西：**报告**（文本，可解析）、**dump**（给调试器）、**窗口**（告诉用户怎么办）。
- 崩溃路径不分配、不加锁、不抛异常，也不依赖可能正处在崩溃现场的库。
- 报告格式稳定：先写的字段永远是先写的字段，新字段只追加。
- 模块属于 `base` 层：无 Qt、无 appfw，任何进程（含工具）都能用。

### 1.3 非目标

- 不做上报/聚合/上传队列（没有服务端，也没确定的支持渠道）。
- 不自己造跨平台 dump 格式，不接符号服务器。
- 不在崩溃进程里显示任何界面（堆可能已坏，Qt 可能正是崩溃现场）。

### 1.4 一句话结论

`vn::Crash` 在进程健康时装好 last-chance 过滤器；崩溃时把静态缓冲里的报告写完、写 dump、拉起同目录的 `vnCrashHandler.exe`、然后退出。
handler 是一个独立的健康进程：读报告 → 用 DbgHelp 符号化 → 写人读的 `.txt` → 显示窗口。

## 2. 设计要点

### 2.1 位置：`src/base/crash`（G2、G12）

- 是什么：`vn::Crash` 库 + 同目录的 `vnCrashHandler` 可执行目标。
- 为什么：崩溃捕获是平台事实，不是框架能力；放 appfw 会让低层需求依赖高层，放 `src/fw` 会误导依赖方向。
- 边界：只依赖 `vn::Global`、`vn::Core`，Windows 上私有链接 `dbghelp`。handler 只链 `vn::Crash` + 系统库，**不链 Qt、不链 appfw**。

### 2.2 报告文件是进程间唯一的接口（G4、G5）

- 是什么：崩溃进程写文件、起进程、然后死掉；handler 只读这一个文件（外加 dump 路径）。
- 为什么：没有 IPC 协议、没有常驻进程、没有孤儿回收；格式就是契约，可单测。
- 边界：报告缺字段时 handler 降级显示，不拒绝整份文件。

### 2.3 崩溃路径不分配、不加锁、不抛（G10）

- 是什么：报告的镜像表/帧/上下文都在**静态**缓冲里组装（不占已溢出的栈），文本渲染进静态 `char[32768]`，写文件用 `WriteFile`。
- 为什么：崩溃时堆与 CRT 锁的状态未知；日志模块可能持锁，所以不用它。
- 边界：截断是允许的降级（见 3.2），但不是"丢几行继续写"——渲染在第一个装不下的行处停止。

### 2.4 符号化搬到 handler（G7、G11）

- 是什么：崩溃进程只记录 `模块+偏移`；名称与源码行由 handler 在健康进程里解析。
- 为什么：DbgHelp 是重活，放在崩溃进程里既慢又危险；而 handler 可以随便分配、可以查符号路径。
- 边界：符号缺失时帧退化为 `模块+偏移`，绝不丢帧。

### 2.5 符号必须按镜像的 preferred base 装载（G11）

- 是什么：handler 从 PE 头读 `ImageBase` 与 `SizeOfImage`，把镜像装到**它自己的首选基址**上，查询地址 = `首选基址 + 偏移`。
- 为什么：DbgHelp 解析符号的方式是 `base + RVA`，而这个 base 必须是 PDB 生成时的那个；崩溃进程实际映射到的地址每次开机都不同（ASLR）。
  只给 base 不给 size 时 DbgHelp 会把模块当成 0 字节，任何地址都落不进去，于是"符号从来没被请求过"——看起来像"找不到 PDB"。
- 边界：两个镜像可能首选同一基址（`KERNELBASE` 与 `ntdll` 都是 `0x180000000`），DbgHelp 以 base 为键，后装的会顶掉先装的；
  冲突时给后者加 `0x10000000000` 的整数倍。

### 2.6 窗口只展示与导出（G4、G8）

- 是什么：窗口显示报告/dump 路径，展开区是符号化后的报告；四个动作里三个是导出（复制、打开文件夹、发邮件草稿），第四个是关闭。
- 为什么：崩溃现场不可信，窗口不该尝试"修复"或重启应用；它能做的只有把文件交到人手里。
- 边界：`mailto:` 不能带附件，所以邮件动作的文案必须让人自己把 dump 拖进邮件。

### 2.7 模块不认识产品（G1）

- 是什么：`Config::app_name` + `Config::context` 是产品知识的全部入口；报告头那一行（如 `Vine 1.0.0 (Debug)`）由应用自己拼。
- 为什么：同一份 handler 要给 `bin/` 里所有应用用，写进产品名就绑死了。
- 边界：`app_name` 为空时退化为 exe 文件名，再退化为 `app`。

### 2.8 一个 `bin/` 一份 handler（G10）

- 是什么：handler 与 `Vine.exe` 装同一目录，崩溃时按**绝对路径**拉起，不查 `PATH`。
- 为什么：同一次构建 ⇒ 写报告与读报告没有版本偏移；全机共享一份才会有这个问题。
- 边界：handler 缺失时只写报告与 dump（降级，不影响退出）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
|---|---|---|---|
| F1 | 安装与卸载 | `install(Config)` 解析路径、快照配置、挂 last-chance 过滤器；`uninstall()` 还原 | 已落地 |
| F2 | 无符号报告 | 崩溃时写 `vn-crash-report 1` 文本（上下文、异常码/地址/线程、镜像表、帧） | 已落地 |
| F3 | dump | 同目录写 `MiniDumpNormal` dump，失败删除半成品 | 已落地 |
| F4 | 拉起 handler | `CreateProcess` 传 `--report/--dump/--app/--mailto`，不等它 | 已落地 |
| F5 | 报告渲染/解析 | `renderReport`（纯函数，写入给定缓冲）与 `parseReport`（未知键跳过） | 已落地 |
| F6 | 符号化 | handler 读 PE 头、装模块、解析函数名与源码行 | 已落地 |
| F7 | 人读文本 | handler 写 `<报告名>.txt`（与报告同目录） | 已落地 |
| F8 | 崩溃窗口 | 路径 + 展开区 + 复制报告 / 打开文件夹 / 发送邮件 / 关闭 | 已落地 |
| F9 | 符号诊断 | 缺 PDB/装不上符号时，在 `.txt` 末尾说明原因（含 DbgHelp 错误码） | 已落地 |

### 3.1.1 F1 安装与卸载

- **行为**：空路径各自取默认值（`<exe 目录>/<app>_crash.log`、`<app>_crash.dmp`、`vnCrashHandler.exe`）；`handler_exe` 只有在装的时候文件存在才会被拉起。
- **约束**（C1）：只挂 last-chance，绝不挂 first-chance——被 `catch` 或调试器接住的异常不是崩溃。
- **约束**（C2）：一个进程只有一套设施，重复 `install` 是替换而不是叠加。
- **要求**（R1）：`install` 之后进程行为不变；`uninstall` 之后过滤器不再被调用。
- **验收**（T1、T2）。
- **注意**（N1）：测试宿主、插件宿主这类"自己决定进程生死"的进程必须 `uninstall()`，否则它的报告与退出码会被替换。

### 3.2 F2/F5 报告格式

```
vn-crash-report 1
app Vine
time 2026-10-30T15:17:42
dump E:\...\Vine_crash.dmp
context Vine 1.0.0 (Debug)
exception 0xc0000005 at 0x00007ff7645f3bbc thread 53560
image 0x00007ff7645f0000 Vine.exe E:\...\Vine.exe
frame 0x00007ff7645f3bbc 0 0x3bbc
```

- **行为**：每行 `键 值...`；`image`/`frame` 一行一个，`frame` 的第二个字段是镜像下标（`-1` = 不在任何已加载镜像内），第三个是偏移。
- **约束**（C3）：`vn-crash-report <版本>` 是唯一硬要求；不认识的版本整份拒绝（半份帧表比没有更糟），不认识的键跳过。
- **约束**（C4）：渲染在第一个装不下的行处**停止**，因此输出永远是完整报告的**前缀**——不会出现"帧指向已被截断的镜像表"。
- **要求**（R2）：`parseReport(renderReport(r)) == r`。
- **验收**（T3、T4）。

### 3.3 F6/F7 handler 的符号化与文本

- **行为**：`SymInitializeW(符号路径)` → 每个镜像按 `ImageBase` + `SizeOfImage` 装 → 帧地址 = `base + 偏移` → 函数名 + `[文件:行]`。
- **约束**（C5）：符号搜索路径 = handler 所在目录 + 每个镜像所在目录 + `_NT_SYMBOL_PATH`（handler 继承崩溃进程的环境，所以不需要额外配置）。
- **要求**（R3）：没有 PDB 时帧仍输出 `模块+偏移`（偏移是与重建后二进制对照的依据）。
- **验收**（T5）。

### 3.4 F8 崩溃窗口

- **行为**：`TaskDialogIndirect`（comctl32 v6，靠 manifest pragma，无 `.rc`）；正文是两个路径，展开区是符号化文本。
- **复制报告**：把 `.txt` 全文 + 末尾 `dump: <路径>` 放进剪贴板（开发者要的是能直接粘进 issue 的那段）。
- **打开文件夹**：`explorer /select,<dump>`；dump 不在时打开报告所在目录。
- **发送邮件**：仅当 `--mailto` 给了才出现；打开默认邮件客户端并预填收件人/正文，**附件要用户自己加**。
- **注意**（N2）：`TaskDialogIndirect` 不可用时退回 `MessageBoxW`（只给路径），再不行就不弹——报告已经在盘上。

## 4. 公开接口

### 4.1 `vn::crash`（`sdk/vine/crash/CrashHandler.hpp`）

| 接口 | 契约 | 备注 |
|---|---|---|
| `Config` | 安装期配置；空路径 = exe 旁的默认名；`max_frames` 落在 `[1, 64]` | 只在 `install` 里读一次 |
| `Report` | 固定容量的崩溃载荷（24 镜像 / 64 帧 / 8 行上下文），可平凡复制 | 就是报告文件的内容，双向共用 |
| `install(Config)` | 挂 last-chance 过滤器并快照配置 | Windows 返回 true；暂无其他平台 |
| `uninstall()` | 还原过滤器、清空快照；未安装时安全 | |
| `renderReport(Report, span<char>)` | 纯函数、不分配；返回写入字节数，超容量时是前缀 | 崩溃路径用这个 |
| `renderReport(Report)` | 返回完整文本（handler/测试用） | |
| `parseReport(text, Report&)` | 未知键跳过；魔数/版本不符返回 false | |

### 4.2 `vnCrashHandler.exe`

```
vnCrashHandler.exe --report <报告文件> [--dump <dump>] [--app <名字>] [--mailto <地址>]
```

不认识的开关忽略（崩溃进程可能是更新的版本）。没有 `--report` 时退出码 1，否则窗口是唯一输出，退出码 0。

## 5. 约束、要求与不变量

### 5.1 约束（C#）

- **C1** 只挂 last-chance 过滤器。
- **C2** 一个进程一套设施；`install` 幂等替换。
- **C3** 报告魔数+版本不符时整份拒绝，未知键跳过。
- **C4** 渲染只输出完整行，且在第一个装不下的行处停止（前缀性质）。
- **C5** 符号路径 = handler 目录 + 镜像目录 + `_NT_SYMBOL_PATH`。

### 5.2 要求（R#）

- **R1** `install` 不改变进程行为；`uninstall` 之后过滤器不再被调用。
- **R2** `renderReport`/`parseReport` 往返相等。
- **R3** 符号缺失时帧仍给出 `模块+偏移`。

### 5.3 不变量（N#）

- **N1** 崩溃路径上没有 `new`/`malloc`、没有标准流、没有日志模块、没有 Qt。
- **N2** 报告先于 dump 落盘：dump 写不出来时报告仍在。
- **N3** 报告、dump、`.txt` 三者同目录同名（`.log` / `.dmp` / `.txt`）。

## 6. 注意事项与陷阱

### 6.1 `exception_code` 是 SEH 宏名

- 会怎么错：`<excpt.h>` 把 `exception_code` 定义成 `_exception_code`，`Report::exception_code` 会变成 `Report::_exception_code`（`C2039`）。
- 怎么判：成员访问报"不是 `Report` 的成员"，而头文件里明明写着。
- 怎么防：字段叫 `code`（见 `Report`）；同理，`windows.h` 的 `min`/`max` 宏要吃 `std::min`，编译前 `#define NOMINMAX`。

### 6.2 只给 base 不给 size ⇒ 模块"零字节"

- 会怎么错：`SymLoadModuleExW(..., base, 0, ...)` 之后所有查询都失败，`SymGetModuleInfo` 报 `size=0`、无 PDB——看起来像 PDB 找不到。
- 怎么判：`Vine.exe` 这种本地就有 PDB 的模块也没有函数名，而同时 `loadedImage` 为空。
- 怎么防：从 PE 头读 `SizeOfImage` 一起传（见 2.5）；`.txt` 的"符号"段会打印 DbgHelp 的错误码。

### 6.3 两个镜像首选同一基址

- 会怎么错：`KERNELBASE` 与 `ntdll` 都用 `0x180000000`，DbgHelp 以 base 为键，后装的顶掉先装的，两边的帧都错。
- 怎么判：`SymGetModuleInfo` 对两个不同下标返回同一个 `LoadedImageName`。
- 怎么防：给冲突基址加一个固定的巨大整数倍（见 2.5），查询时用加过的基址。

### 6.4 崩溃路径重入

- 会怎么错：写报告时再次异常（堆坏、栈溢出升级），过滤器重入，两份报告互相截断。
- 怎么判：报告只有半行、时间戳跳变。
- 怎么防：`InterlockedExchange` 单调门闩，重入直接 `EXCEPTION_CONTINUE_SEARCH`。

### 6.5 `wmain` 会带出控制台

- 会怎么错：handler 若用 `wmain` 且为 console 子系统，崩溃时会先闪一个黑框——看起来像第二次失败。
- 怎么判：崩溃时出现控制台窗口。
- 怎么防：`WIN32_EXECUTABLE TRUE` + `wWinMain` + `CommandLineToArgvW`。

## 7. 已否决选项

| # | 选项 | 理由 |
|---|---|---|
| D1 | 放 `appfw` | 低层需求依赖高层（违反 G2）；无 Qt 的工具用不了；崩溃时碰 Qt/日志更危险。 |
| D2 | 并进 `vn::system` | 一个目标两件事，且让每个 system 使用方都链 `dbghelp`（G5）。 |
| D3 | 接 first-chance 异常 | 抢在调试器与 `catch` 之前，把正常控制流报成崩溃（G4）。 |
| D4 | 崩溃时写日志模块 | logging 状态未知（可能正持锁），会二次崩（G10）。 |
| D5 | 默认给所有进程安装 | 测试会吞掉 gtest 的报告与退出码（G8）。 |
| D6 | 在崩溃处理里弹 Qt 对话框 | 堆可能已坏、Qt 可能正处在崩溃现场（G10/G11）。 |
| D7 | 预启动常驻 handler（Crashpad 形状） | 现在只需要"崩溃时起一个进程 + 文件为接口"；常驻只在"堆坏也要起 UI"成为真痛点时才值（G8）。 |
| D8 | 现在引 Crashpad/Breakpad | 只用到它们最便宜的本地符号化，却要接整套构建与部署（G8）。触发条件见 `../backlog/base-crash.md`。 |
| D9 | handler 解析 minidump 走栈 | 要在 dump 上重实现 StackWalk；报告里的 `模块+偏移` 已经足够，dump 留给真正的调试器（G7）。 |
| D10 | `Config::symbol_path` | handler 继承崩溃进程的环境，`_NT_SYMBOL_PATH` 已经够用；多一个字段就是多一个没人设的旋钮（G8）。 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
|---|---|---|
| T1 | `install` 在 Windows 返回 true；`uninstall` 后过滤器不再被调用 | `test_crash`：`CrashInstallTest.*` |
| T2 | 未安装时 `uninstall()` 安全；重复 `install` 是替换 | `test_crash`：`CrashInstallTest.UninstallIsSafe*`、`InstallingTwice*` |
| T3 | 渲染四种形态：无帧 / 有镜像 / 镜像外地址 / 有上下文 | `test_crash`：`CrashReportTest.*` |
| T4 | 渲染与解析往返相等；非报告文本被拒绝；未知键被跳过 | `test_crash`：`RoundTripsThroughParse`、`RejectsTextThatIsNotAReport`、`IgnoresKeys*` |
| T5 | 缓冲区不足时输出是完整报告的前缀 | `test_crash`：`DropsWholeLinesWhenTheBufferIsTooSmall` |
| T6 | 真实崩溃：报告 + dump + 拉起 handler + 退出码 | 手工：临时在 `main` 里空指针写，跑一次，检查三个文件与 `.txt` 的符号行 |

## 9. 状态与演进

### 9.1 当前状态

- 库、handler、窗口、符号化全部落地；`Vine.exe` 在 `createGuiApplication()` 之前 `install`，报告头带 `Vine <版本> (Debug|Release)`。
- `test_crash` 12 个用例；真实崩溃已按 T6 跑通（`main+0x6ac [main.cpp:140]` 能对上源码行）。

### 9.2 已知缺口

- Linux 无实现（`install` 返回 false）；报告模型与渲染是平台无关的，缺的只是 `sigaction` 那半边。
- `std::set_terminate`、栈耗尽保证（`SetThreadStackGuarantee`）、只读安装目录回退到用户数据目录、上次崩溃提示：见 `../backlog/base-crash.md`。
- dump 里没有线程上下文以外的内存；需要"堆已被破坏也能出界面"时再评估常驻 handler。

### 9.3 兼容性 / 迁移

- `main.cpp` 删掉了手写的 `vineCrashFilter`/`installCrashLogger`；外部若在找 `vn_crash.log`，现在是 `<app>_crash.log`（报告）+ `.dmp` + 窗口生成的 `.txt`。
- 报告格式只追加字段；handler 对不认识的版本整份拒绝并原样显示原文。

## 10. 参考

### 10.1 代码

- `src/base/crash/sdk/vine/crash/CrashHandler.hpp` —— 契约。
- `src/base/crash/src/` —— `CrashWindows.cpp`（过滤器/dump/拉起）、`ReportRender.cpp`、`ReportParse.cpp`、`CrashHandler.cpp`。
- `src/base/crash/handler/` —— `main.cpp`、`Symbolize.cpp`、`CrashWindow.cpp`、`Text.cpp`。
- `src/apps/vine/src/main.cpp` —— 唯一调用点（装在最前面）；`tests/test_crash/` —— 用例。

### 10.2 文档与 ADR

- `appfw-app-info.md` —— 报告头那一行身份信息的来源（`AppInfo`）。
- `../backlog/base-crash.md` —— P2/P3 与跨平台触发条件。

### 10.3 外部资料

- Win32 `SetUnhandledExceptionFilter`、`MiniDumpWriteDump`、`RtlCaptureStackBackTrace`；DbgHelp `SymLoadModuleEx`/`SymFromAddr`/`SymGetLineFromAddr64`；`TaskDialogIndirect`。
- PE 格式：`IMAGE_OPTIONAL_HEADER.ImageBase` / `SizeOfImage`。
