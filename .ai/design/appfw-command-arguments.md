# appfw 命令参数设计

> **状态**：现行
> **最后核对**：2026-11-03
> **范围**：一行文本的切词、参数载体、传递路径、历史与事件的回放形态、空行语义；命令如何解释参数不在本文。
> **权威来源**：`CommandArguments.hpp`、`CommandManager.cpp`、`tests/test_appfw/CommandArgumentsTest.cpp`。
> **待办**：`.ai/backlog/appfw-command-arguments.md`（声明式参数、帮助/补全、缺参提问、对象参数、长度上界）。

## 1. 概要设计

### 1.1 问题与背景

名字入口（控制台、ribbon、快捷键、宏）本身不携带任何调用者数据，实例入口却靠字段携带；于是每个需要输入的
命令都自己向用户提问，或者让宿主自己拆词——引号规则、错误格式、历史记录各长一套。历史只记名字，
`open a` 与 `open b` 是两条无法区分的记录。

### 1.2 目标

- 一行文本只按一份规则切词（引号、反斜杠），控制台、启动参数、宏回放、测试共用。
- 参数随执行帧流动、执行期不可变、空参数是空容器。
- 命令自己解释参数；框架不声明、不校验、不设置。
- 历史与事件记录可回放的规范形。

### 1.3 非目标

- 不做声明式参数（类型/默认值/必选）、帮助、补全与**生成的**缺参提问——见待办；命令自己用 `UserIO` 提问是允许的（F5）。
- 不解析 `--`、`-l10`、`--name=value` 的语义：分词器只切词，含义归命令。
- 不引入对象引用（id/名字）、单位与量纲。

### 1.4 一句话结论

**框架切一次词、随执行送一次 token，命令自己解释；历史记的是能被重新切回同一组 token 的规范文本。**

## 2. 设计要点

2.1 **分词归框架，语义归命令**：引号与转义是全局约定，每个命令各写一遍必然分叉；而"`-l 10` 是什么意思"
是命令的语法，框架不猜。分词器不解释 `--`，它只是一个普通 token。

2.2 **载体只装 token**：`CommandArguments` 是值语义、不可变，空参数是空容器而不是 nullptr；它不携带名字、
指针、类型信息，也不保存原始行（`toString()` 已能还原）。

2.3 **传递只在派发时发生一次**：文本入口切词后把首个 token 交给名字解析，其余 token 复制进执行帧；三个名字
入口都接参数，实例入口不接（字段就是它的参数）。

2.4 **记录即可回放**：`toString()` 与 `tokenizeCommandLine()` 互为逆运算，所以一条历史或一次事件就能重建调用。

2.5 **命令行优先，缺的才提问**：命令先把命令行给的 token 当值用，缺失的项才经 `UserIO` 询问（`test_createbox` 就是样板）：
`test_createbox 10 20 30` 不打断用户，`test_createbox` 逐项问，写了半截只问剩下的。
给了但写错的 token 是**调用者错误**（`Failed` + 用法），绝不退化成提问——那会把输入错误藏起来。
这是**命令**的形状而不是框架行为：声明式参数落地后，提问可由派发按声明统一生成（见待办 T3）。

2.6 **变体是显式判别符，不是参数个数**：同一结果的不同输入表达（长宽高 / 两个对角点）由一个模式 token 选择：
`size` / `corners`，另保留唯一无歧义简写（三个纯数字 = size）。判别符是 token，因此进历史与回放；两种模式同时出现直接拒绝。
不同结果对象、不同算法不是变体，是两个命令（G1）。

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 文本切词 | `tokenizeCommandLine()`：空白分隔、双引号成组、反斜杠按 Windows C 运行库规则 | 已落地 |
| F2 | 参数传递 | 三个名字入口接 `CommandArguments`；`context.arguments()` 只读 | 已落地 |
| F3 | 历史与事件 | `args_record` 与 `executing`/`executed` 携带同一份规范化参数 | 已落地 |
| F4 | 文本入口与空行 | `executeCommandLineAsync()` 是唯一分词点；空行派发前拒绝 | 已落地 |
| F5 | 交互回退 | 命令行给全 ⇒ 直接执行；缺项 ⇒ `UserIO` 提问；写错 ⇒ `Failed`（`test_createbox`） | 已落地（命令侧） |
| F6 | 参数变体 | 显式模式 token 选 `size`/`corners`；互斥、缺项提问、坐标与 `getPoint3dAsync` 同语法 | 已落地（命令侧） |

### 3.1.1 F1 文本切词

- **行为**：`tokenizeCommandLine(line)` 返回 token 列表；`""` 是一个空 token；未闭合引号取到行尾。
- **约束**（C1）：只切词，不解释选项语义；反斜杠只有紧邻引号时才是转义，`C:\dir\file` 原样保留。
- **要求**（R1）：与进程 `argv` 的引号规则一致（Windows C 运行库）；所有文本来源共用一个实现。
- **验收**（T1）：`CommandArgumentsTest.*`。
- **注意**（N1）：方言是规范的一部分，改它等于改所有宏与脚本的语义。

### 3.1.2 F2 参数传递

- **行为**：`executeCommandAsync/AndWait/Detached(name, args, target)` 把 token 送进执行帧，命令经
  `CommandExecutionContext::arguments()` 读取。
- **约束**（C2）：值语义、执行期不可变；参数不写进命令对象；实例入参不接 args。
- **要求**（R2）：空参数不是 nullptr；命令可在恢复到的任意线程读取。
- **验收**（T2）：`CommandArgumentsManagerTest.ArgumentsReachTheCommandThroughTheNameEntry`、
  `InstanceEntryCarriesNoArguments`。
- **注意**（N2）：命令要字段化就在 `execute()` 开头自己绑成局部结构，框架不代劳。

### 3.1.3 F3 历史与事件

- **行为**：`CommandHistoryEntry::args_record` 与两个事件参数携带 `args.toString()`。
- **约束**（C3）：记录的是规范化文本，不是用户原始输入；执行、事件、历史三处一致。
- **要求**（R3）：`tokenizeCommandLine(args_record)` 还原出命令看到的 token。
- **验收**（T3）：`HistoryRecordsTheReplayableInvocation`、`EventsCarryTheSameArgumentsAsTheExecution`。
- **注意**（N3）：参数长度目前只受历史条数约束，见待办 T5。

### 3.1.4 F4 文本入口与空行

- **行为**：`executeCommandLineAsync(line)` 切词，首个 token 作为名字；无 token 时返回 `Failed`。
- **约束**（C4）：这是唯一调用分词器的地方；空行不创建实例、不进历史、不发事件。
- **要求**（R4）：控制台在调用前忽略空行；其他宿主拿到的是明确的 `Failed`。
- **验收**（T4）：`EmptyLineIsRefusedBeforeAnythingRuns`。
- **注意**（N4）：控制台补全只覆盖命令名，出现分隔符即不弹候选。

### 3.1.5 F5 交互回退

- **行为**：有位置参数的命令先消费 `arguments()`，只对缺失项 `co_await io->getDoubleAsync(...)`；全部给齐时一次提问都没有。
- **约束**（C5）：token 存在但不可转换 ⇒ `Failed` + 用法，不重新提问；多余的 token 同样拒绝。
- **要求**（R5）：提示语与顺序由命令定义（今天）；语义校验（如长宽高必须为正）仍在命令里。
- **验收**（T5）：控制台手工验收：`test_createbox 10 20 30` 无提问、`test_createbox` 逐项问、`test_createbox 10` 只问两项、`test_createbox 10 abc 30` ⇒ `Failed`。
- **注意**（N5）：这是为了让带参命令在控制台与宏里都好用；生成式提问要等声明式参数（待办 T3），不要在框架里先长第二套参数语法。

### 3.1.6 F6 参数变体

- **行为**：`test_createbox size <长> <宽> <高>` 与 `test_createbox corners <x,y,z> <x,y,z>`；简写 `test_createbox <长> <宽> <高>` = size。两者最终得到同一个盒子。
- **约束**（C6）：只允许一个无歧义简写；判别符占第一个 token；第二个模式关键字出现 ⇒ `Failed`；每个模式固定 token 数量与类型。
- **要求**（R6）：坐标语法与 `UserIO::getPoint3dAsync` 一致（恰好 `x,y,z` 三个有限数）；缺项才提问，一个 token 都没有时先问模式；两模式收敛到同一结果。
- **验收**（T6）：控制台手工：`test_createbox 10 20 30`、`size 10 20 30`、`corners 0,0,0 10,20,30`、`test_createbox`（先问模式）；错误：`corner ...`（未知模式）、`size 1 2`（缺参→提问）、`corners 0,0,0`（缺第二个点→提问）、`corners 0,0,0 0,10,20`（轴重合 ⇒ `Failed`）、`size 1 2 corners 0,0,0 1,1,1`（两种模式 ⇒ `Failed`）。
- **注意**（N6）：判别符进 `args_record`，所以回放不会漂到另一个模式；简写回放仍然确定。声明落地后升级为参数集（待办 T6）。

## 4. 公开接口

### 4.1 `CommandArguments` / 分词

| 接口 | 契约 |
| --- | --- |
| `tokenizeCommandLine(const String&)` | 纯函数，切词；一份方言 |
| `CommandArguments::tokens()/size()/empty()/operator[]` | 只读 token 视图 |
| `CommandArguments::toString()` | 规范文本；与分词器互为逆运算 |

### 4.2 入口

| 接口 | 说明 |
| --- | --- |
| `executeCommandLineAsync(line, target)` | 文本入口，唯一分词点 |
| `executeCommandAsync(name, args, target)` / `executeCommandAndWait(...)` / `executeDetached(...)` | 名字入口携带参数 |
| `executeCommandAsync(Command*, target)` | 实例入口，不接参数 |
| `CommandExecutionContext::arguments()` | 本次执行的 token，只读 |

## 5. 约束、要求与不变量

| # | 约束 |
| --- | --- |
| C1 | 分词只做词法，不解释选项；反斜杠仅紧邻引号时转义。 |
| C2 | 参数值语义、执行期不可变，不写进命令对象。 |
| C3 | 历史/事件记录规范化文本。 |
| C4 | 空行在派发前拒绝。 |
| C5 | 存在但不可转换的 token ⇒ `Failed`，不重新提问；多余 token 拒绝。 |
| C6 | 只允许一个无歧义简写；判别符占第一个 token；两种模式同时出现拒绝。 |

| # | 要求 |
| --- | --- |
| R1 | 所有文本来源共用分词器与方言。 |
| R2 | 空参数是空容器；可从任意线程读取。 |
| R3 | `toString()` 往返恒等。 |
| R4 | 控制台忽略空行，其他宿主拿到 `Failed`。 |
| R5 | 提示语与顺序、语义校验由命令定义；框架不代生成提问。 |
| R6 | 模式决定 token 数量与类型；坐标与 `getPoint3dAsync` 同语法；两种模式产生同一结果。 |

| # | 不变量 |
| --- | --- |
| N1 | 一次执行一份参数，执行结束随帧销毁。 |
| N2 | 名字与 token 分开传递，名字不占 token 位。 |
| N3 | 变体的判别符必须出现在调用记录里，或被唯一简写确定。 |

## 6. 注意事项与陷阱

### 6.1 把参数当成命令状态

- 会怎么错：写进命令成员，同一实例再次执行读到上次的值，并发执行数据竞争。
- 怎么判：命令由工厂默认构造、执行完即销毁；实例入口允许复用同一指针。
- 怎么防：只从 `context.arguments()` 读；需要字段就在 `execute()` 开头绑。

### 6.2 每个命令自带引号规则

- 会怎么错：`"C:\Program Files\x"` 在一个命令里是一个 token，在另一个里是三个。
- 怎么判：出现第二份切词代码就是信号。
- 怎么防：分词只允许调用 `tokenizeCommandLine()`。

### 6.3 保存原始行而不是规范形

- 会怎么错：回放依赖用户当时怎么写，别名、引号风格一变就重现不了。
- 怎么判：历史条目里的字符串与 `toString()` 不一致。
- 怎么防：只记 `toString()`；需要展示原文时另存一份，不作为回放依据。

## 7. 已否决选项

| # | 选项 | 理由 |
| --- | --- | --- |
| D1 | 由管理器把值设置到命令成员 | 需要反射或字符串分发；引入"实例一次性""半初始化对象不得运行"两条新不变量。 |
| D2 | 每类参数各造一个值类型（`any`/variant 全集） | 错误推迟到运行期，且与 `ConfigManager` 值模型打架；今天只需要 token。 |
| D3 | 分词器解释 `--` 与短选项 | 没有声明就没有"短名属于谁"的答案；先切词，声明落地后再上语义。 |
| D4 | 按参数个数推断变体（`0,0,0 10,10,10` vs `10 20 30`） | 缺参时报不清是哪种形式，回放无从判断，加一个可选参数就歧义。 |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 切词：引号、转义、路径、未闭合引号 | `CommandArgumentsTest` |
| T2 | 三个入口看到同一份参数；实例入口为空 | `CommandArgumentsManagerTest` |
| T3 | 历史/事件可回放 | `HistoryRecordsTheReplayableInvocation`、`EventsCarryTheSameArgumentsAsTheExecution` |
| T4 | 空行不执行 | `EmptyLineIsRefusedBeforeAnythingRuns` |
| T5 | 交互回退（命令侧） | 控制台：`test_createbox 10 20 30` / `test_createbox` / `test_createbox 10` / `test_createbox 10 abc 30` |
| T6 | 参数变体（命令侧） | 控制台：`size …` / `corners …` / 简写 / 未知模式 / 缺项 / 两种模式同时给 |

## 9. 状态与演进

机制已落地；声明式参数、帮助/补全、缺参提问、对象参数与长度上界按 `.ai/backlog/appfw-command-arguments.md`
的触发条件推进。`CommandArguments` 是最终载体的子集：声明落地后增加类型化访问与派发前校验，`tokens()` 保留。
交互回退（F5）与参数变体（F6）目前是**命令侧**实现（`test_createbox`）；第二条命令需要多写法时升级为声明里的参数集（待办 T6）。

## 10. 参考

- `src/fw/appfw/sdk/vine/appfw/CommandArguments.hpp`、`src/fw/appfw/src/CommandArguments.cpp`
- `src/fw/appfw/sdk/vine/appfw/Command.hpp`（`arguments()`）、`src/fw/appfw/src/CommandManager.cpp`
- `tests/test_appfw/CommandArgumentsTest.cpp`
- `appfw-command-manager.md`（链、历史、事件）
