# ADR-2026-10-30：Linux 的崩溃窗口用 Qt Widgets

- 状态：**已取代**（同一天被 `ADR-2026-10-30-crash-handler-x11-on-linux.md` 取代：窗口只用 Xlib + Xft 就能画出来，
  `base` 层不必为了一个对话框引进 UI 工具包）
- 日期：2026-10-30
- 相关：`../design/base-crash.md`（§2.1 边界、§2.9 捕获、§3.4 窗口、§9 状态） · `../backlog/base-crash.md`

## 背景

`vn::Crash` 的 handler 是一条写下来的边界：**只链 `vn::Crash` + 系统库，不链 Qt、不链 appfw**
（`base-crash.md` §2.1，理由写在 `src/base/crash/CMakeLists.txt` 的注释里：一个学会了 Qt 或 appfw 的 handler，
恰好在那些东西刚死掉的时候就起不来了）。

Linux 侧要补上捕获与窗口之后，这条边界撞上了一个事实：**窗口是 handler 的全部输出**。
Windows 上这个窗口是系统给的（`TaskDialogIndirect`，comctl32 自带），Linux 上没有等价物 ——
要么引入一个窗口工具包，要么放弃窗口。这不是"多写一个文件"的选择题，而是边界本身的取舍。

## 选项

| 选项 | 好处 | 代价 |
|---|---|---|
| A. Qt Widgets（选定） | 仓库已经在用 Qt6（`Vine` 本身就是 Qt 应用），没有新的工具链；`QDialog` + `QPlainTextEdit` 就是所需全部；与 `src/apps` 的构建方式一致 | **违反 G2**：`base` 层的可执行目标出现 Qt 依赖；handler 运行时需要 Qt 库与平台插件；`check_export_annotations` 之类的库级约束管不到它，靠这条 ADR 记着 |
| B. GTK3 | 不碰 Qt，桌面发行版基本都带 | 给仓库引进一个目前没有任何使用者的依赖（G8）；样式与其他界面差得远；两台机器（开发机 / 用户机）都可能缺 `gtk3` |
| C. 终端界面（报告打到 stdout，`xdg-open` 打开目录） | 零依赖、ssh 下也能用、完全符合现有边界 | 双击启动时没有终端，用户什么都看不到 —— 这正是窗口存在的理由（G4） |
| D. 让 appfw 提供这个窗口 | 复用现有 UI 能力 | handler 从此依赖 appfw，即"崩溃时最不该依赖的东西"（G2/G10，也是这条边界最初的理由） |

## 决定

1. **只有 `CrashHandler` 这一个可执行目标链 `Qt6::Widgets`，且只在 Linux 上**；`vn::Crash` 库、崩溃进程、
   Windows handler 的依赖都不变（`N1`：崩溃路径上没有 Qt）。
2. Windows 继续用系统 TaskDialog（`ADR` 不改动它）：能不给用户多装一棵依赖树就不装。
3. 没有 display 时（`DISPLAY`/`WAYLAND_DISPLAY`/`QT_QPA_PLATFORM` 都没设）不建窗口，把文本打到 stdout；
   `.txt` 在开窗之前就已经写好，所以任何一条路径都不会丢报告。
4. 这条边界违反**只写在这一处**：设计文档 §2.1 指向本 ADR，CMake 注释指向本 ADR，两处都不复述理由。

## 后果

- 好的：Linux 有和 Windows 等价的崩溃窗口，且不新增工具链；handler 仍然是一个独立、健康、只读报告的小进程。
- 坏的 / 代价：`base` 层第一次有了 Qt 依赖（尽管只在一个可执行目标上）。找依赖的人会看到 `src/base/crash`
  链了 Qt，需要读到本 ADR 才知道范围：**它不传染给库**。
- 坏的 / 代价：部署 handler 时要带上 Qt 的 Widgets / platform 插件（发行版仓库自带即可；
  自建静态环境是下一段的触发条件）。
- 触发条件（什么时候重开）：① 出现"目标机器上没有 Qt"的分发形态（静态链接 / 精简镜像）；② 仓库里出现别的
  GTK 使用者，使 GTK 变成"已经付过钱的依赖"；③ handler 需要长期常驻（Crashpad 形状）而 Qt 的启动成本不可接受。
