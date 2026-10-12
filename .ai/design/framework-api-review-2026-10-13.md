# 框架级 API 复核：骨架的三问（2026-10-13）

> 一次**只读复核**（结论落地在本文件；唯一顺手修掉的是发现 1，判据见下）。承接
> `graphics-api-review-2026-10-12.md` —— 那份把"区 / 引擎 / 目标的所有权与基数"挖干净了，并明确留了白：
> `CommandManager` / `UserIO` / `ConfigManager` 内部、**iobase / robotics 未深挖**。本片补的正是那一半，
> 外加三问：**是否符合框架级代码 / API 协调性 / 概念与抽象是否优秀**。
>
> 覆盖：`base/**`（core / async / math / geometry / iobase / window / runtime / logging / progress / system /
> crypto / imaging）、`fw/di`、`fw/appfw`、`viz/graphics`、`robotics/**`、`loaders/**` 的**公开 SDK 头**
> （`src/**/sdk/**/*.hpp`，**332** 个：graphics 76 / appfw 61 / robotics-core 36 / core 32 / async 28 / math 21 /
> geometry 18 / iobase 10 / meshio 7 / window 6 / runtime 6 / di 5 / logging 5 / progress 4 / system 4 / crypto 4 /
> robotics-io 4 / imaging 3 / brepio 3 / imageio 2 / global 1），外加 24 个 CTest 套件的全量一轮与五个 check 脚本。
> 未深挖：各模块实现内部（只有找发现 1 的根因时读了 Qt 部署那一段）。

## 量法（数字怎么来的，可复现）

一次性脚本扫公开头，规则刻意收窄到"能人工复核"：

- 只认**类作用域**的公开声明（跟踪花括号深度，跳过函数体里的语句）；
- 向上找文档块时跳过属性行（`[[nodiscard]]` 这类）——不跳会把有文档的函数判成没文档（第一版就是错的）；
- 参数表按**配平括号**取第一个括号组，并在函数体 `{` 处截断——否则 `requires(FP<T>)` 与函数体会被当成参数
  （第一版把 `Vector2<T>::normalize()` 判成"有 1 个参数"）；
- **成组注释算已注释**：同一段无空行的连续声明共用块首那段（本仓访问器成组注释是常见写法），
  所以指标是"**成组无 `@brief`**"（块首那句不存在），不是"每个函数都得有自己的注释"；
- 每个桶都抽查过样本、修掉假阳性后剩下的清单逐条看过：对不上号的不写。

## 结论（三问）

**① 是否符合框架级代码：是。** 依赖方向实测单向无环，机制 / 组合的边界守得住，导出与模板实例化纪律正确（见"正例"）。
**② API 协调性：大体协调，三处不齐** —— 公开面文档纪律没有门禁（覆盖按模块年龄分层）、命名两套口径、
以及**仓库自己文档里写的门禁命令修前是红的**。**③ 概念与抽象：多数优秀，短清单是概念债** ——
遗留指针族仍在公开面、自建 std 无成文理由、所有权三分法无成文口径、配置有两条读取路径。

## 正例（这些别再当问题上报）

- **依赖方向**（实测各模块 SDK 头 include 的 `vine/<模块>` 前缀）：`fw/appfw → {appfw, async, graphics, math,
  progress, di}`；`viz/graphics → {graphics, math, window, geometry, imaging}`；`base/iobase → io`；
  `loaders/meshio → {geometry, crypto, io, runtime}`。**没有任何一处向上 include**，也没有环。
- **导出与实例化纪律**：非模板类写 `class VN_X_API Name`；**模板在 .cpp 里显式实例化导出**
  （`Matrix4x4.cpp` 末四行 `template class VN_MATH_API Matrix4x4<...>;`），所以 `base/math` 21 个头里只有
  `math_global.hpp` 带宏 —— 那是对的，不是漏；header-only 模块（`base/async` 28 头 / 0 个 cpp）不带宏也是对的。
- **graphics 的公开面最干净**：76 个头里"成组无 `@brief`"只有 **2** 处（`FrameRing.hpp:46`、`MaterialArena.hpp:49`），
  0 处 `get*` 访问器，布尔 getter 一律 `is/has`。
- **内部类型统一住 `detail`**：`base/async` 里 112 处 `detail::`，协程状态类型（`CvState` / `AsyncQueueState` /
  `SharedTaskState`…）一致地被关进 detail —— 公开签名里出现它们的**类型名**是 C++20 协程的必然，不是设计失误。
- **`Object` 不掺所有权**：`Object.hpp:21` 只有 RTTI（`getType` / `isKindOf` / 虚析构），要计数才用
  `RefCounted<T>` + `intrusive_ptr`。这比"所有东西都继承一个带引用计数的基类"干净。

## 发现与处置

| # | 判定 | 证据（实测） | 处置 |
| --- | --- | --- | --- |
| 1 | **仓库自己文档里的门禁命令是红的** | `docs/guides/graphics.md` §4.4 把 `ctest --test-dir build` 列进门禁。修前：`ctest -R test_gui` = **Exit code 0xc0000409（3.34 s）**，全量 24 套件 23 绿 + 这一条红。根因：**Release 树 `platforms/` 下只有 `qwindows.dll`**（Qt 直接 abort：`Could not find the Qt platform plugin "offscreen"`），而 `cmake/VineDeployQt.cmake` 里那段"复制 offscreen 插件给 ctest"的代码**从来没被跑过**（`DeployQt` 无 preset、无依赖、不是 `ALL`）。同一段旧代码在 Windows 上用 `WIN32 OR` 跳过存在性检查，正是因为 `if(EXISTS)` 吃不了 `$<CONFIG:Debug>` 生成器表达式 | **已修**：①插件发现提成 `vn_find_qt_offscreen_plugin_dir()`（两种拼写**都查磁盘**），`tests/test_gui/CMakeLists.txt` 给测试目标挂 POST_BUILD 复制 —— 不依赖"有人记得跑 DeployQt"；②`UserIOTest.PickingFilesFillsTheLineWithoutSubmitting` 在 `offscreen`/`minimal` 下 `GTEST_SKIP()`（无头平台没有可脚本化的模态文件对话框，等它的人只会空转）；③§4.4 的过期句改掉。**修后实测**：`ctest -R test_gui` **Passed 15.33 s**；全量 `ctest` **24/24 通过**；`build/bin/Release/platforms/qoffscreen.dll` 到位 |
| 2 | **遗留指针族仍在公开 SDK 面** | `sdk/vine/Ptr.hpp` / `sdk/vine/RefObject.hpp` 是转发头，真身在 `sdk/vine/deprecated/{Ptr,RefObject}.hpp`（`[[deprecated]]` 已标）；全仓**零活用户**（`src` + `tests` + `tools` 里唯一的提及是 `src/fw/di/sdk/vine/di/Registration.hpp:55` 的一句注释） | 待裁决：移出 `sdk/`（`vn_add_library` glob 的是 `sdk/**`，移走即不再进安装面）或直接删。代价：4 个公开头 + 一个框架里两套所有权概念 |
| 3 | 公开面文档纪律**没有门禁** | 规则在 `.github/copilot-instructions.md`（每个公开函数要 `@brief` / `@param` / `@return`）。实测"成组无 `@brief`"：graphics **2**、appfw **71**、base **224**、robotics-core **11**；"有 `@brief` 缺 `@return`（非 void、非 setter）"：graphics **103**、appfw **58**、base **33**；缺 `@param`：graphics **28**、appfw **32**、base **23**；协程 plumbing 无注释 **24**（这类本来就不该要求） | **已落地（2026-10-13，不缩规则）**：新增 `scripts/check_public_docs.py` + `scripts/public_docs_baseline.txt`（**1164 条义务 / 127 个头**：`@brief` 454、`@param` 288、`@return` 422；按模块 appfw 529 / core 194 / graphics 148 / math 110 / robotics-core 50 / async 39 / geometry 26 / logging 24）。语义：基线是**地板** —— 新增无文档的公开声明**或删掉已有文档**都红；补文档只报 `stale`（绿），`--update` 是降基线那一步。已并进 `scripts/vsg_rewrite_gate.sh` 第 5 阶段。判据与 4 条变异见下 |
| 4 | 命名两套口径 | `get*` 共 35 处，真访问器 **16** 处：`ConfigManager` 8（**类注释里自带 @note 理由**：`int/bool/double` 是关键字、get/set 共用 key）、`Object::getType` + `Inherit::getType`（覆盖）、`Quaternion::getRotate`、`robotics/core` 6（`getQ` / `getEnd` / `getEnds` / `getNumEnds`）。其余是动作（`getFileAsync` = 问用户要文件、`getOrAddGroup` = 取或建），合规 | robotics 与 MoveIt / ROS 上游同源 ⇒ 建议**按模块声明口径**；其余并入下一次公开面变更（与已推迟的 GUI `enabled()` / `visible()` 同批，要动 ABI 号） |
| 5 | 所有权三分法无成文口径 | 三种拼写并存且各有理由：`intrusive_ptr`（`RefCounted` 派生对象）、`std::shared_ptr`（协程状态；EventBus 的 `Payload` 持 `shared_ptr<const Object>`，依据在 `appfw-eventbus.md`；`Signal` 用 `std::atomic<std::shared_ptr<Table>>` 是标准库 idiom）、借用的裸指针 / `raw_ptr`。`base/async` **0** 处 `intrusive_ptr` / **40** 处 `std::shared_ptr`，且出现在公开签名（`AsyncQueue.hpp:106`、`AsyncConditionVariable.hpp:52`） | 写一句口径（框架对象 = `intrusive_ptr`；事件 / 协程状态 = `shared_ptr`；借用 = 裸指针 / `raw_ptr`）；awaiters 那几个构造函数可改 **private + friend**，把内部状态类型从公开面拿掉（可选） |
| 6 | `base/core` 自建 std 无成文理由 | `String.hpp`（70 KB）、`SmallVector.hpp`、`Buffer.hpp`、`Signal.hpp`、`ThreadPool.hpp`、`MemoryStream.hpp`、`stl++.hpp` —— 一套替代品；`.ai/` 里 grep 不到一条说明"何时用自建、何时用 std"。`vn::String` 在 SDK 边界统一这件事有记录（appfw 342 处 `String` vs 1 处 `std::string`） | 补一条 memory（"为什么自建"），成本极低 |
| 7 | 配置两条读取路径 | `ConfigManager`（扁平字符串键 + 每类型访问器 + JSON）与 `ConfigGroup` / `ConfigCategory` / `ConfigItem`（类型化树，给选项 UI 用）并存 | 写清分工（扁平 = 运行时读取器 / 树 = 描述与 UI），或把扁平面标成 convenience |
| 8 | 门禁清单与套件的关系不清 | §4.4 只列 4 个套件 + 3 个脚本，实际 **24 个 CTest 条目**（含 `test_di` / `test_asyncqt` / `test_proximity` / `test_robotics_*`），且那段点名的 `scripts/gfx_lavapipe_check.sh` / `scripts/vsg_selftest_evidence.sh` **树里根本不存在** <!-- drift-ok --> | **已落地（2026-10-13）**：§4.4 改成事实（一条命令 `scripts/vsg_rewrite_gate.sh` + 8 个卫生脚本 + `ctest` 24 套件口径），并写明那两个名字已不存在 |
| 9 | CMake 复制粘贴残留 | `src/robotics/core/CMakeLists.txt:2` 用 `APPFW_TARGET_NAME` 装 `RoboticsCore` 目标（同目录作用域内当前无害，但同名变量在两处指不同目标） | 改一个变量名 |
| 10 | 骨架里的死代码 | `Object.hpp` 公开头里两段被注释掉的 `cast<T>()` | 删或补理由 |
| 11 | **既有 flake（与本片无关，未修）** | `test_async` 间歇性挂死：一次 CTest 全量里它跑了 **7+ 分钟、CPU 只剩 0.2 s、20 个线程**（判据：进程还活着但不再前进）；同期 `-j 4` 的另一轮它是过的。`.ai/memory/async.md` 里没有这条记录 | 待查（不在本片范围，要堆栈）；先把"已知 flake"记进 memory，免得下次又被当成新问题 |
| 12 | 文档漂移（随发现 1 一起改） | §4.4 的"三个套件既有失败（`test_cppstd` / `test_runtime` / `test_system`）"**已过期**：实测 5/5、28/28、12 + 1 skip 全绿 | 已改 |
| 13 | "0 warning" 只在增量构建下成立 | `tests/test_gui/test_gui.cpp` 一重新编译就报 **C4033**（`ThrowingCommand::execute` 必须返回值，行 2858）——把本片的改动 stash 掉、编原版照样报（实测），是**既有**的协程误报：协程体末尾是 `throw`，MSVC 仍按普通函数分析 | 不修（unreachable 的 `co_return` 只是噪声）；要么加一行局部 `#pragma`，要么把"重新编译过的 TU 允许这条"写进口径。触发 = 下次动 `ThrowingCommand` 时 |

## 看着可疑但确实一致（别在后续复核里反复提）

- **模板头不带导出宏**：模板导出走 .cpp 的显式实例化，头里本来就不该有宏（见正例）。
- **公开签名里出现 `detail::` 类型**：协程 awaiter 的类型必须完整可见，状态类型住 `detail` 是全模块统一的写法。
- **EventBus 用 `shared_ptr`**：`Object` **不**带引用计数（`Object.hpp:21` 只有 RTTI），总线要跨线程、跨排队投递保活，
  `shared_ptr` 是设计选择，依据写在 `appfw-eventbus.md`；缺的只是一句"口径"（发现 5）。
- **`ConfigManager` 的 `getXxx` / `setXxx`**：类注释里写了理由（关键字不能做方法名 + get/set 共用 key 参数）。
- **`get*` 里的动作动词**：`getFileAsync` / `getOrAddGroup` 是"去拿 / 取或建"，不是属性访问器。
- **`vn::String` 统治 SDK 面**：`std::string` 只在边界转换处出现（base 20 / appfw 1 / graphics 3 / robotics 1 处）。
- **`Math.hpp` 里 `PI` / `E` / `EPSF` 不带 `k` 前缀**（13 处，全在这一份头里）：数学常数的行业拼写，
  与"文件级常量用 `k` 前缀"的冲突值得一句豁免说明，不值得改名。
- **`fw/di` 的 DI 容器**：它有真消费者 —— appfw 的 `ServiceManager`（不是只为测试存在）。
- **`test_gui` 那条对话框用例在 Windows 默认平台仍会红**（原生对话框脚本不了）：既有事实，
  本模块判据一直用 `--gtest_filter=-UserIOTest.PickingFilesFillsTheLineWithoutSubmitting` 排除；
  想让它在 Windows 也绿，要在测试里置 `Qt::AA_DontUseNativeDialogs`（触发：下次动这条用例时一起做）。

## 值得做但现在不做（带触发条件）

1. **发现 2/3/4/5/6/7 的落地**：都要裁决（用户已选"先落记录 + 修发现 1"）。
2. **把导出注解门禁扩到其它 DLL 模块**：今天 `check_export_annotations.py` 只盖 vsg 后端；
   graphics / appfw / base 靠人看。触发 = 下次新增公开类或模块时一起加。
3. **把 doc 规则变成"基线门禁"**（发现 3 的选 (a)）：触发 = 下一次公开面清理。
4. **`test_async` 的 flake 定位**（发现 11）：触发 = 下次被它挡住门禁时。

## 落地记录（2026-10-13，随本文件一起进树）

**发现 3 + 发现 8**（同一天补的第二片：文档纪律基线门禁 + 门禁清单对账）：

- `scripts/check_public_docs.py` + `scripts/public_docs_baseline.txt`：扫 `src/**/sdk/**/*.hpp` 的公开声明，
  按 `.github/copilot-instructions.md` 的三条义务（`@brief` / 每个参数的 `@param` / 非 void 的 `@return`）比对基线。
  **基线 1164 条义务 / 127 个头**（`@brief` 454、`@param` 288、`@return` 422；appfw 529 / core 194 / graphics 148 /
  math 110 / robotics-core 50 / async 39 / geometry 26 / logging 24）。语义是**地板**：新增无文档的公开声明**或删掉文档**
  都红；补文档只报 `stale`（保持绿）；`--update` 是唯一降基线的动作（在提交里可评审）。豁免成文在脚本头：协程 plumbing
  （`await_ready` / `await_suspend` / `promise_type` / `return_void`…）、`detail` 作用域、`= delete` / `= default`、`operator`，
  以及"成组注释算已注释"（本仓访问器成组注释的写法）。
- 已并进 `scripts/vsg_rewrite_gate.sh` 第 5 阶段（`run_hygiene "public docs"`），并在 `docs/guides/graphics.md` §4.4
  列出门禁口径（顺手把那段里两个**已不存在**的脚本名对账掉）。
- **判据（实测）**：门禁在基线上 **0 new / 0 stale**；**4 条变异各自单独红** —— ①在 `RenderEngine.hpp` 插一条无文档的
  `int mutantProbe() const;` ⇒ 报 `brief|RenderEngine::mutantProbe` + `return|RenderEngine::mutantProbe`、exit 1；
  ②删掉 `RenderEngine::setDevice` 的 `@param device` ⇒ 报 `param|RenderEngine::setDevice/1`、exit 1；
  ③把 `DHTransformConverter.hpp` 里补上的 `@brief` 撤回（欠债变多）⇒ 报 `brief|mdhToTransform`、exit 1；
  ④反例：给基线里的 `mdhToTransform` 补 `@brief` ⇒ 只报 **1 stale**、**exit 0**（补文档永远不会变红）。
- **门禁自己的坑（写脚本时撞到的两个）**：①`skip = name == "detail" or (scopes and scopes[-1][3])` 在 scopes 为空时返回
  **空列表**（不是 `False`），这个列表一旦被存进作用域栈就会被共享改写 —— 症状是"每个类的每个成员都被当成跳过"，
  一条发现都报不出来（写成 `bool(...)` 之后才有输出）；②"三条义务"的顺序检查是错的：先只记 `brief`、documented 才查
  `@param`/`@return`，于是**补上一个 `@brief` 会让 `@return` 义务"冒出来"变成新条目** ⇒ 补文档变红。改成三条各自独立判定
  （一次声明可欠多条，基线也就从 786 涨到 1164）。

## 与既有文档的关系

- 上一轮的框架级复核（区 / 引擎 / 目标的所有权与基数）：`graphics-api-review-2026-10-12.md`。
- 对象模型与行业口径（含"拾取是机制、选择在业务"这条边界）：`graphics-object-model-vs-3d-frameworks.md`。
- 落地记录：`.ai/memory/graphics.md` 与 `.ai/memory/appfw.md` 顶部同日条。
