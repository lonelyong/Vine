# appfw 文档模型设计（2026-09-27）

本文回答：**"中间的渲染区、文档到底归谁；插件能否创建文档；文档是不是 app 创建的"**。核心结论一句话：

**框架只定义"文档"（身份 + 生命周期）与"文档类型"的注册；"有几个区、怎么摆、什么展示"是具体 app 的自由。**

状态：**core 的 `Document` + `DocumentManager`、打开/保存（载荷注册表 + 文档来源）、类型元数据与管理器面板均已落地**，
见 §4–§6 与 `tests/test_appfw/DocumentTest.cpp`（17 条语义钉子）、`tests/test_gui/test_gui.cpp` 的
`DocumentManagerDialogTest`。**视图层与共享会话（§7、§8）未做**。

## 1. 今天有什么：会话与视图，没有文档

- `RenderControl`（`sdk/vine/appfw/gui/RenderControl.hpp`）：一个控件 = 一个原生 surface = 一个 `RenderEngine` =
  **恰好一个** `SceneView`（camera + content scene + orbit 操纵器）；`MainWindow::setPrimaryRenderControl()` 把
  "唯一的那一个"发布出去。
- 但 `SceneView`（`sdk/vine/graphics/SceneView.hpp`）自己写着 **"Several views may share one engine"** —— 视图本来
  就可以多份，只是今天没人这么用；同一处还写着两个默认 window pass 会互相擦掉、"The engine never manages viewport
  layout"。
- **文档：全仓 0 处**（`robotics/io` 里的 `XMLDocument` 是 tinyxml2 的，无关）；宿主 `src/app/src/main.cpp` 也不接
  文件参数。

所以今天"中央渲染区"的语义是：**外壳拥有控件与会话，某个内容插件往里填一个 scene**（`demo_plugin` 正是那个插件）。
没有身份、没有生命周期、没有"关闭/保存/最近打开" ⇒ 那是**视图内容**，不是文档。

## 2. 三条不同的轴（别焊在一起）

| 轴 | 今天归谁 | 文档模型里归谁 |
|---|---|---|
| surface + 设备 + attach 时机（**会话**） | `app_shell`（`RenderControl`） | **不变：外壳**（一个窗口一个会话，管"画在哪"） |
| camera + content scene + 操纵器（**视图状态**） | `RenderControl` 私有那个 `SceneView` | **文档类型提供的视图**（见 §5） |
| 身份 + 生命周期 + 数据（**文档**） | 不存在 | **框架持有生命周期，插件提供类型** |

## 3. 框架的面积：只给两件，别的一概不定

| 层 | 内容 | 位置 |
|---|---|---|
| **core SDK**（无 Qt） | `Document`（`typeId/title/isDirty/canClose`）+ `DocumentManager`（类型注册、`create`、`documents/current`、`close`、三个事件） | `sdk/vine/appfw/` |
| **gui SDK**（第 2 步） | `DocumentView`（= `Control` + "我在看哪个文档"）+ 视图注册表（typeId → 工厂） | `sdk/vine/appfw/gui/` |
| **gui SDK 默认派生**（第 2 步） | `SingleAreaDocumentHost`：**只有一个区**的现成实现 | `sdk/vine/appfw/gui/` |
| **具体 app（外壳插件）** | 决定单/多文档、用不用默认派生、区域怎么摆 | `src/plugins/<app>_shell/` |
| **插件（文档类型）** | 注册 typeId + 视图工厂，决定展示形态（3D / 表格 / 图片…） | 各插件 |

框架**不定义**（也永远不定义）：区域数量、标签、布局、单文档还是多文档、展示类型的集合、"当前可见的是哪个"。
一条判据：一个**只开表格文档**的 app 不该为 3D 付钱（不建 device）；一个**多文档** app 不该需要 N 个 device；
一个**单文档** app 不该写标签条代码。

## 4. 文档语义（第 1 步已落地）

`Document` 是**纯数据 + 生命周期**，没有窗口/控件/场景/路径/对话框；`DocumentManager` 是进程一份（
`Application::documentManager()`，创建于 `Application::initialize()`，早于插件加载，所以插件能在自己的 `load()`
里注册类型）。五条语义：

1. **类型注册**：`registerType(type_id, display_name, factory)`；一个 id 一个类型，重名**拒绝**（不悄悄换工厂）；
   空 id / 空工厂拒绝。
2. **创建**：`create(type_id)` —— 未注册、工厂返回空、或工厂返回的文档 `typeId()` 与注册不符，一律返回 nullptr
   并记一条 warning。**创建不动 `current()`**：创建与选中是两件事。
3. **当前切换**：`setCurrent(doc)` 只接受本管理器拥有的文档（`nullptr` = "没有"）；值没变就**不发事件**。
4. **关闭**：`close(doc)` 先问 `canClose()`；**被拒 ⇒ 返回 false，集合与 `current` 都不动**（宿主可以存了再关）。
   基类 `canClose()` 默认是 `!isDirty()`；"脏也能关"的类型覆写它 —— *要不要问用户是宿主的事*，框架只回答能不能关。
5. **次序与所有权**：被关的文档**先出集合再发事件**（处理函数走 `documents()` 看到的是当下的集合），**事件期间对象
   仍然活着**（处理函数可以读它 —— 这是宿主从文档里取走所需信息的时刻），`close()` 返回时才销毁（除非别处还持有）。
   关掉的若是当前文档：`current` 变空，且 **`currentChanged(null)` 先于 `closed`** —— 宿主在 `closed` 里挑下一个当前
   文档，不会被随后那次通知覆盖。

事件：`opened` / `closed` / `currentChanged`（`Event<DocumentManager, DocumentEventArgs>`）。处理函数抛异常只记日志
（同 `CommandManager` 的 `fireEvent`）。**线程**：管理器与文档都是**应用线程**对象（文档类型可以把读盘放池上，但要
把结果搬回应用线程再建/改文档）；管理器**不带锁**，因为它的操作是"一个宿主的动作"，而事件会把管理器本身交给用户代码
（"锁内不跑用户代码"）。

**有意不做（第 1 步）**：`open(path)` 与保存路径（要 IO 的账，见 §7）、`dirtyChanged` 事件（文档类型自己发变化
事件、宿主订阅即可）、`closeAll()`（宿主三行循环，且"部分失败"的策略归宿主）、按插件记 owner（等有真实需求再加）。

## 5. 打开与保存：载荷注册表 + 文档来源

**位置不是 `path`，是 `Vfs + 虚拟路径`。** `src/base/iobase`（`vn::io`）已经有这件事的抽象：`Vfs` 是一棵以 `/` 分隔的
虚拟文件树，操作走 `Result`/`IoError`、带 `isReadOnly()`；后端有 `DirectoryVfs`（真实目录）、`ZipArchive`（包 / 内存树，
可从真实文件或**字节**打开，有改动 overlay + `commit`/`saveAs`/`toBytes`）、`MountVfs`（把多个后端挂成一棵树）。
“不是本地磁盘上的东西”的答案是**换一个后端**：网络/数据库/私有格式 = 自己实现一个 `Vfs`（或“先拿字节 → `ZipArchive`”）。
仓库里已有这个形状的现成例子：`robotics::io` 的 `WorkcellIO` —— `loadXml/loadPkg(real_path)` 只是便利包装，核心是
`loadVfs(vn::io::Vfs&, path)` / `savePkg(cell, Vfs&, options)`，它自己的注释写着 "DirectoryVfs for folders,
ZipArchive for packages"。

### 5.1 打开：按**载荷类型**注册的打开器

```cpp
manager.registerOpener<MeshPayload>({
    .type_id       = u8"mesh",
    .open          = [](const MeshPayload& payload) -> Document* { /* ... */ },
    .source_scheme = u8"file",
});
vn::appfw::Document* document = manager.open(&payload);     // 借用；键 = payload.getType()
```

- **登记项是个 struct**（`DocumentOpenerRegistration<Payload>`），和 `DocumentTypeRegistration` 同一个理由：除 id 与回调外都可选，
  而且**具名**字段不会互串（两个签名不同的 `std::function` 位置参数太容易换错）。载荷类型是**模板参数**不是字段 —— 它既是框架的键，
  也是“回调是强类型的”这件事的来源。指定初始化器得按声明顺序写（C++20）。

- **载荷是调用方与文档类型之间的私有协议**：`Vfs + path`、剪贴板里的一张图、一个设备连接、一个数据库键，甚至
  “多份输入”自定义 struct。框架**从不看内容**，也**不定义任何一种载荷类型**。
- **载荷派生 `Object`，键就是它的 `Type`**（理由与代价见 §5.7）：`registerOpener<Payload>` 用 `Payload::desc()` 登记，
  `open()` 用 `payload->getType()` 匹配，**只此一个入口**（没有模板重载：模板参数只是把入参的静态类型重说一遍，键其实来自
  对象的运行时类型，而 `close(Document*)`/`setCurrent(Document*)` 也都是指针）。被借用这件事由 `raw_ptr` 写在签名上。
  这和 `EventBus::subscribe<TEvent>` 是**同一个擦除形状**
  （`std::function<…(raw_ptr<const Object>)>` + `obj_cast<Payload>` 把类型还回来），于是 appfw 里只有**一套**运行时
  类型系统（`vn::Type`），而不是 `std::type_index` 与它并行。**“这个类型吃什么载荷”也不用另给名字**：
  面板直接显示 `Payload::desc()->fullName()`，不可能和键各说一套。
- **无人受理是明确的答案**：没有登记过这种载荷 ⇒ 返回 nullptr 并记一条带载荷类型全名的 warning。这正是“为什么这个
  文件打不开”的答案：现在没有任何类型吃这种载荷（插件没加载/被禁用）。
- **登记一种类型 = 接受它以及它的派生**：载荷自己的类型只要是登记类型的“子类”就算匹配（`Type::isKindOf`，和 EventBus
  投递事件同一把尺子，所以载荷谱系不需要框架知道任何事），而打开器拿到的**永远是它登记的那个类型**。这一条正好把 wrapper
  里的 `obj_cast` 变成**真在干活**的一步（精确匹配时它只是个永远不失败的检查）。
- **多个候选的挑选顺序是契约**：**具体者优先**（登记类型离载荷自己的类型最近的那个先试——“这类载荷归我”是结构性的说法）
  → `refine` 分数（>0 才接受，越高越先） → `priority`（越高越先） → 登记顺序；第一个返回文档的胜出。所以**给基类登记就是兜底**：
  具体的拒了/返回空才轮到它，而分数再高也抢不走具体的那个（“具体性”用登记类型在 `parent()` 链上的深度算，登记时算一次存起来）。
  这也是“同一棵树里的 `.urdf` 与 `.stl` 各找各的”那种事，而框架不知道任何扩展名。
- **一次 `open()` 只考虑它开始时就登记好的打开器**（同 `EventBus::publish()` 的“计划快照”）：`refine`/`open` 都是
  用户代码，可以在里面登记新打开器（`registerOpener` 会让打开器表扩容）⇒ 候选只记**下标**、调用前**拷一份** callable，
  免得正在跑的那个 `std::function` 被搬走。
- **打开器为未知 id 登记一律拒绝**（载荷没人能造就比不登记更糟：看着能用，直到真去开一个）。
- **`opened` 处理函数可以当场把文档关掉**（“打开之后发现不对”）⇒ `create()`/`open()` 在发完事件后要回头确认
  它还在集合里，否则**返回空**，而不是一个已经销毁的指针。
- **载荷得是个左值**（`open` 只收指针）：临时量要先起个局部再取地址。代价换来的是“借用”写在签名上、`nullptr` 表达
  “没有东西可打开”、以及和 `close`/`setCurrent` 同一套指针风格。

### 5.2 打开载荷 ≠ 文档来源

| | 打开载荷（open payload） | 文档来源（DocumentSource） |
|---|---|---|
| 是什么 | “**怎么读进来**” | “**我是谁 / 写回哪里**” |
| 形态 | 调用方与类型之间的私有协议（可借用、可含引用、可多份输入） | 值类型：`{ scheme, address }` 两个字符串 |
| 生存期 | 只在 `open()` 调用期间有效 | 跟着文档走（标题、`save()`、最近打开都要它） |
| 例子 | `VfsSource{vfs, path}`、`ClipboardSource{image}`、`DeviceSource{dev}` | `{"file", "<locator>#<path>"}`、`{"device", "robot-1"}`、**空** |

**类型在打开时自己决定来源是什么**：从 `VfsSource` 开的 ⇒ 来源是“哪个 Vfs 的 locator + 哪条路径”；从剪贴板来的 ⇒
**没有来源**（只能另存为）；从设备来的 ⇒ 来源是那个设备，`save()` 的语义是“下发”。只有打开器知道 locator（宿主给的），
所以那一半是打开器的责任 —— 框架不插手。

### 5.3 保存：core 不认识 `Vfs`

- `Document::save()` **没有参数**：写回“我自己的来源”，怎么解释是类型的事。基类默认**拒绝**（“哪里都不写”的类型是合法的，
  框架不能假装它存过了），没来源时类型自己也该拒。
- `Document::source()` 是基类持有的**值**（`setSource()` 是 protected，打开器/类型设它，框架只读）。
  **core 因此对 IO 零依赖**（appfw 不需要链 `vn::IOBase`，`Vfs` 只出现在“载荷”里，那是宿主/插件侧的词汇）。
- “另存为”归**类型自己的接口**（`RobotDocument::saveAs(VfsSource)` 之类），由类型的命令/宿主调用 —— 框架只留 `save()`
  一个统一动作（对应“关前问脏”）。
- 失败不用 `bool` 丢信息：类型自己的接口可以用 `io::Result`/`IoError`；框架这一层只问“存成了没有”。

### 5.4 `DocumentSource` 是值类型，**不派生**

“位置种类”这条轴已经被 `Vfs` 占掉了（`DirectoryVfs`/`ZipArchive`/`MountVfs`/自实现），再给 `Source` 派生 = 两套并行的
种类层次、且把“辨识来源”的负担推给每一个文档类型（`dynamic_cast`/访问者），还失去值语义（拷贝、比较、存盘）。
变化（variety）该放的三处是 **Vfs（怎么读）/ 打开器（怎么拿到 Vfs 或字节）/ 文档类型（怎么解释内容）**。
“未命名的新文档”**不是一种来源，是没有来源**（`valid() == false`）；需要子资源定位（“包里哪个场景”）就加一个可选字段
（仍然不是派生）；需要存盘的“最近打开”就由 **Vfs** 给出可持久化 locator。

### 5.5 三条必须写清的契约

1. **`Vfs` 是借用的**，不是引用计数的（`class Vfs { virtual ~Vfs(); }`）⇒ **它必须比从它打开的文档活得久**；
   关文档不关 `Vfs`，“关掉整个包”由宿主按“先关文档、再销毁 Vfs”的顺序做。
2. **`Vfs` 无内部锁**（头里写着 shared storage 由调用方同步）⇒ 文档可以在池上读它，但**同一个 `Vfs` 实例不能被两个
   线程同时碰**；并行读一棵树由宿主串起来。
3. **载荷是借用的，而且有两个生命周期**（`open` 收 `raw_ptr<const Object>` —— 指针本身就说明“非拥有”，框架不拷贝也不持有）：
   - **载荷对象**（那个小描述符本身）只在 `open()` 期间有效——管理器不持有，文档也不得存指向它的指针；
   - **载荷指向的东西**（`Vfs`、设备、图像缓冲）**要活到文档不再需要它**（至少和文档一样久），因为文档在
     `open()` 返回之后还在用它（`save()` 要靠源里的 `Vfs`）。打开器要把需要的指针/句柄**拷进文档或它的来源**，
     而不是存载荷的地址。

### 5.6 载荷不用智能指针（一个已经做过的决定）

- **载荷对象本身**：不要。`std::shared_ptr<Payload>` 保活的是**描述符**，不是描述符指向的 `Vfs`/缓冲 —— 它解决的是错的
  那一半，还会给人“已经安全了”的错觉；而值载荷（两三个指针的小 struct）上堆 + 引用计数是白付，`open(VfsSource(vfs, path))`
  这种自然写法还要变成 `make_shared`。框架也**不知道该持有多久**（只有类型知道文档要用它多久）—— 和 `setData` 同一个
  反对理由。
- **要转移的大块数据/所有权**：放进**载荷的字段**（`std::shared_ptr<const std::vector<unsigned char>>`、
  `intrusive_ptr<Image>`），打开器**拷那个字段**（const 载荷也能拷 `shared_ptr`）⇒ 所有权规则写在**载荷的定义**上，
  一眼看懂，而且不用改打开器签名。
- **唯一需要“框架持有载荷”的场景是异步**：那时用**新名字**表达（`openAsync` 按值收载荷，或明确要求调用方保活），
  而不是把同步 API 的参数偷偷变成共享所有权。
- **运行期才知道类型的路由**（IPC/拖放/命令参数转来的载荷）：要保活就**由它自己**持有（自己的消息对象里放
  `shared_ptr<void>`），再把借用指针传进 `open()`。**所有权归需要它的那一层，不归读它的那个 API。**

### 5.7 载荷派生 `Object`（第 2 步落地时的决定，含代价）

**结论**：`registerOpener<Payload>` 要求 `Payload` 满足 `DocumentPayload`（`ObjectBased` + `TypeDescribed` + **自己的**
`desc()`），键用 `Payload::desc()`；`open` 收 `raw_ptr<const Object>`（**只有这一个入口**，没有模板重载）；
面板的载荷列表由 `desc()->fullName()` 填。**这是一个取舍，不是免费的**：

| 换来的 | 要付的 |
| --- | --- |
| 一套 RTTI（`vn::Type`，与 `EventBus` 同形），不再让 `std::type_index` 与它并行 | 每个载荷一个 .cpp（`VN_OBJECT_META_IMPL`）—— **载荷不能 header-only** |
| `registerOpener` 少一个 `payload_name` 参数，也没有“名字与键漂移”这回事 | 载荷**不再是聚合体**（有虚函数）⇒ 调用点的指定初始化器 `Payload{ .field = … }` 没了，要逐字段赋值或给它构造函数 |
| `void*`、空指针检查、“拿不到名字就回退到 mangled 类型名”全消失 | `int`/`String`/`path` 这种标量不能再当载荷（要包一层 struct）；每个载荷多 vptr + 虚析构 |
| `obj_cast` 只在登记的 wrapper 里把类型还回来，**不可能错**（键就是那个类型），派生载荷也自然以基类引用进来 | —— |

**为什么不是“只要求 `Object`、但保留模板”**：模板里已经知道类型了，把键从 `Payload::desc()` 换回 `typeid(Payload)`
一行代码也不少，却要多背 .cpp 那份代价 ⇒ 那样是纯亏。要么整条走 `Object`（现在这样），要么整条走 `std::type_index`
（改之前的样子）。选前者，因为它把 appfw 的类型身份统一成**一套**。

**违规是响的，不是静的**：载荷忘了 `VN_OBJECT_META_DECL` ⇒ `DocumentPayload` 里的 `&T::desc != &Object::desc`
让 `registerOpener` 那行**编译失败**（探针实测：诊断里点名 `DocumentPayload`）；`desc()` 定义了两处 ⇒ 元数据自己抛
（`Type` 拒绝同型的第二个实例）。附带一条：**一个没带自己元数据的载荷类，对框架而言就是它最近的带元数据的祖先**（`getType()`
和匹配都这么说）—— 它不会报错，只是那个类自己的身份不存在，想让它算一种独立载荷就必须给它元数据。

### 5.8 “打开后文档到底拿着什么” 与刷新（reload）

**规则（一条）**：**框架不处理拷贝逻辑**。载荷从调用方对象到打开器的引用，一路上**一次拷贝都没有**；“要留住”只有两个
合法去处：**派生类型自己拷**（把需要的字段拷进文档 / 来源）与**业务提前拷**（调用方在 `open()` 之前把要留住的东西留住）。
框架既不持有载荷、也不帮任何人保活，更不会偷偷留一份 —— `open()` 路上有任何拷贝都是契约破了。用例
`TheFrameworkDoesNotCopyAPayload`（带拷贝计数的载荷，断言计数恒为 0）钉住前半句，`AnOpenerBuildsADocumentFromAPayload…`
里“载荷事后被改掉，文档手里还是自己拷的那份”钉住后半句。

§5.5 的第 3 条容易被误读成“文档不能留住载荷的东西”。准确的说法是：

- **框架不持有载荷对象**，也**不担保它活着**（它可以是个临时量）；所以文档**不得存指向它的指针**。
- **文档可以（也应该）把需要的字段拷进自己**：`Vfs*` + path、设备句柄、字节/`intrusive_ptr<Image>`—— 拷什么由**类型自己**决定，
  因为只有它知道“内容”是什么。例如 `VfsSource{vfs, path}` 这个载荷：`path` 进 `DocumentSource`，而**要重读就还得把 `Vfs*`
  存进文档**（`DocumentSource` 只有两个字符串，装不下引用 —— 这正是它“不派生、只描述”的代价）。`Vfs` 本身必须比文档活得久（§5.5 第 1 条）。

**刷新不做成“重放 `open(payload)`”**：那要求有人留着那个载荷对象（而且载荷指向的东西得还活着）⇒ 等于让框架持有载荷
（§5.6 已经否掉：描述符拷下来不等于它指向的资源还在）。刷新的正解是**类型的动作**：

- `Document` 上留一个动作位（`virtual bool reload()`，默认 `false` = “我重读不了”），由类型用它自己存下的东西（`Vfs*` + path、
  设备句柄、来源字符串）重读；**框架不参与怎么读**，只提供动作。
- **脏了就不刷新**：和 `canClose()` 同一条规矩 —— 会丢掉未保存改动时类型必须拒（宿主先去问用户存不存）。
- **需要“以后再开一次”的调用方，自己保管那份描述**（可能是它自己的消息对象），然后再调一次 `open(&payload)`：
  框架不需要为此多一行代码。

⇒ **现在不动代码**：`reload()` 是视图/宿主那一步（第 3 步）的事，等真的有一个宿主需要它再加（默认实现让所有已有类型不用改）。

## 6. 类型元数据与管理器面板

与仓库既有的三件套同构（`PluginInfo`/`PluginManagerDialog`、`CommandInfo`/`CommandManagerDialog`、`ConfigWindow`），
文档这一件是 `DocumentTypeInfo` + `gui::DocumentManagerDialog` + `show_document_types` 命令。

`DocumentTypeInfo` 带的是 **类型级**（静态）事实：`type_id`、`display_name`、`description`、`icon`（内联 SVG，同
`PluginInfo::icon`）、`owner`（注册它的插件名，同 `CommandInfo::owner`）、`can_create`（有没有“新建”工厂 —— 没有就是
“只能打开”的类型）、`payload_types`（受理的载荷名）与 `source_schemes`（打开器产出的来源方案）。

- **`owner` 来自宿主的一个标签**：`Application::registrationOwner()` —— 插件加载器在插件注册前后设它
  （`CommandManager::setRegistrationOwner()` 现在也写这一份，`PluginManager` 不用改），注册表读同一个事实即可，
  不必各自再留一份（否则每个注册表都要在加载器里多接一根线）。
- **面板两栏**：已注册的类型（上表）+ 已打开的文档（标题、类型、来源、脏 —— `source()` 与 `isDirty()` 恰好是标签页显示不了的东西）。
  它回答的是“为什么这个文件打不开”（看有没有类型受理那种载荷）与“上次开的是哪个”（看来源）。
- **面板只读，类型没有“启用/禁用”开关**：谁能用由注册它的插件决定（禁用或跳过插件 —— 既有且语义完备的开关），
  再给类型一个启用位会多出一个语义晦涩的轴（“禁用一个类型后，已经打开的同类型文档怎么办？”没有干净答案）。

## 7. 视图层（2026-09-27 落地）

三件东西，都在 `sdk/vine/appfw/gui/`：

| 类 | 职责 | 关键决定 |
| --- | --- | --- |
| `DocumentView` | `Control` 的薄包装 + `document()` + `content()` + `activate()`/`deactivate()` | 视图**按文档实例**存在，不是按类型 |
| `DocumentViewRegistry` | `type_id → 工厂`（`DocumentView* (*)(Document&)`） | 一类型**一个**工厂：重复登记 / 空 id / 空工厂一律拒 |
| `CentralDocumentHost` | 听 `DocumentManager`、造/销毁视图、把 `content()` 放进中央区（§8） | 六条契约见 §7.2 |

- **`content()` 是"放进中央区的东西"**：2D 视图（表格、图片、文本）**返回自己** —— `Control(QWidget*, bool)` 已能包任意
  QWidget；3D 视图**同样返回自己**（它自带渲染控件，见 §9）。宿主只认 `content()` 这一个虚函数，**一实例一页**、
  规则只有一条：页是视图自己的控件（`owns=false` 交接），文档关闭时宿主把它连同视图一起销毁。
- **`activate()` / `deactivate()`**：页面被换上来 / 换下去时的可选钩子（默认空）。适合"停掉计时器""收掉临时附着物"这类
  收尾 —— **换页不销毁视图**，所以别在这里释放重东西。
- **视图是"实例的"，不是"类型的"**：同一类型的两份文档**各有一个视图实例**，各自的相机 / 选择 / 滚动位置互不影响；
  切回来复用**同一个**实例（状态还在），文档关闭才销毁。这与"按类型共享某个 renderer"并不冲突：视图持有的是**实例状态**，
  引擎与渲染控件是共享的（§9）。
- **工厂每次调用必须返回新实例**（注册表只存一个可调用对象，不缓存实例）；`create()` 拿不回类型对应的文档时返回空，
  宿主显示"视图工厂拒绝了这份文档"，**不猜**。
- **视图层只在 GUI 宿主里**：视图是 `UIElement`，无头 `Application` 连控件都没有。所以注册表与宿主由 `GuiApplication`
  在**建主窗时**建好（`viewRegistry()` / `centralDocumentHost()`，`tests/test_gui/DocumentViewTest.cpp` 有一条钉子钉这件事），
  插件在 `load()` 里往注册表登记即可 —— 拿不到 `GuiApplication` 就跳过（无头进程里插件照样能加载，只是不登记视图）。

### 7.1 三档状态：谁该记住什么

| 归属 | 例子 | 生命周期 |
| --- | --- | --- |
| **文档**（core） | 数据、`source()`、`isDirty()` | 打开 → 关闭 |
| **视图**（gui，**按实例**） | 相机、选择、滚动位置、局部编辑状态 | 视图第一次被显示时懒建 → 文档关闭 |
| **宿主 / 面板**（按**事件**或按**类型**） | 当前是哪个文档、有没有打开过某种类型、命令能不能用 | 随事件走；同类型之间切换**不重建结构** |

这张表是"面板跟 `currentChanged`、视图按实例、命令自己判类型"三句话的出处：**面板**显示的是"当前文档的事实"（谁都能重算），
**视图**显示的是"这一份的状态"（换一份就不能复用），而**命令**问的是"当下有没有可作用的对象"—— 这是命令自己的检查
（§12.2 第 2 条），框架不带这份元数据。

### 7.2 `CentralDocumentHost` 的六条契约

写进 `CentralDocumentHost.hpp` 的类注释里，是后面接真 3D 视图时不能忘的东西：

1. **容器一次装好**：宿主第一次真的要显示文档时才 `setCentralWidget()`；在那之前**不碰**外壳放在中央区的东西；
2. **懒建**：没被显示过的文档没有视图（"打开十个文本文件"不该造十个视图）；
3. **按实例缓存**：同一份文档切回来复用同一个视图实例，`activate()`/`deactivate()` 是换页通知；
4. **文档关闭 ⇒ 视图销毁**（不留着，也不复活）；
5. **同类型之间切换只换页**：不重建、不重新登记；
6. **所有权是拆开的**（踩过坑，见下）：宿主拥有**视图对象**，容器拥有**页控件**。交给宿主的视图用 `owns=false` 包控件 ——
   否则"控件随容器析构"会顺着 `UIElement` 的自毁路径把宿主还拿着的**视图对象**一起删掉（实测是 double free：容器析构
   → 页控件 → `UIElement::~UIElement` 里的 `delete this` → 宿主 `Impl` 里的 `unique_ptr<DocumentView>` 再删一次）。

⇒ 第 6 条是视图层这一批里**唯一一个真缺陷**，形态值得记住：**两种所有权管着同一个对象** —— 宿主的 `unique_ptr` 管
视图对象，Qt 的父子关系管页控件，而 `UIElement` 在"控件没了"时会 `delete this`。两条析构路径单看都对，合起来就是 double
free；`gdb` 里表现为同一个 `UIElement::~UIElement` 连续出现两帧（都停在 `delete d` 那一行）。`build-asan` 下一跑就能抓
（视图用例 9/9 干净）。

## 8. 中央客户区（2026-09-27 落地）

`DockPanelManager::setCentralWidget()` 的实测语义（`third_party/DockingPanes`）：

- 它落到 `DockingPaneClient::setWidget()`：**删掉旧 layout + `layout()->addWidget(w)` ⇒ 重父化**。中央区是一个
  **单槽**（没有 id/标题/标签/关闭），**可以重复调，但每次都会重父化**；旧控件留在 pane 里无人管理（layout 被删、
  widget 没被 delete）。
- Qt 重父化会**销毁并重建原生窗口** ⇒ `RenderControl` 走一遍 `Pending -> Presenting`（它文档里那条 "a dock drag, a
  screen change, a **reparent**" 路径）。所以**不要每个文档各占一次中央控件**。
- `DockAreas` 只有 Left/Right/Bottom（**没有 Center**）⇒ 文档视图**不要**放进可拖拽的 dock pane（拖一下就重父化，
  3D 会话被重建一次）；`DockPanel::onClosing()` 那条否决只对**面板**有效，**不覆盖中央区** —— 文档的关闭询问由宿主接。

**落地形状**（`CentralDocumentHost`）：中央区放一个**文档页容器**（`QStackedWidget` 包的 `Control`，宿主拥有），
**一次装好**，之后只在容器内部切页：

- 切文档 = 容器内部换页（`QStackedWidget` **只切可见性、不重父化**）⇒ 视图的原生窗口与会话都不动；
- 一个类型**没登记视图** ⇒ 一张**说明页**（点名类型 id："没有为文档类型 `b-rep` 登记视图"），不是空白、也不是借别人的
  视图来显示；工厂返回空 ⇒ 另一张说明页（"这个视图工厂不收这份文档"）。两张页都**不是错误弹窗**：打不开的东西要看得见原因；
- 接管（第一次真的要显示文档时）会把外壳原来放在中央区的控件**藏起来**（`setVisible(false)`，**不 delete** —— 外壳的东西
  外壳自己管），宿主析构时**还回去**（`DockPanelManager::centralWidget()` 就是为这个加的一根读数）；
- 隐藏/显示 ≠ 重建：非 3D 文档显示自己的控件、把渲染控件隐藏（会话存活），切回 3D 只是"换 view + 显示"。
- 标签条自己画或用 `QTabWidget`（库的 tabbed container 只服务 L/R/B 的面板）—— **这一件还没做**：今天宿主内部只有页，
  没有标签条，所以"同时开三份文档、点标签切换"要靠命令（`setCurrent`）或后续补的容器。

## 9. 硬限制：运行时只有一块渲染区

`RenderBackend::initialize()` 的契约是 **"device, surface, pipelines"、"Called once per session, after the host
announced the native window (setWindowHandle)"**，并且"A backend that is already initialized **tears the previous
session down first**"；`RenderBackendFactory::create()` 又是**每次给一份新实例**；而 `gfx_backend_vsg` 的
`VSG_MAX_DEVICES=1` 是**有意绊线**（"a path that does this must throw instead of quietly working"）。

⇒ **"每个文档一块各自独立的渲染 surface"今天做不到**（第二个 device 会抛）。可行的形状是：**一块 surface + 一个
engine（外壳持有并共享）+ 每个 3D 文档一个 `SceneView`**，切换 = 换呈现的 view；想分屏就显式给 pass 设 sub-viewport
（engine 不管布局）。这是**后端的账**，不是文档模型的账 —— 将来后端支持"一个 device 服务多个 surface"才可能改。

**视图层怎么接（已落 2026-09-27）**：**一个文档 = 一个视图 = 它自己那份资源**（用户拍板）。视图**自带** `RenderControl`
（自己的原生面、自己的会话/设备、自己编的管线），`content()` 返回**视图自己**（一实例一页，和 2D 一模一样）；
`activate()` 里首次造场景并 `RenderControl::init()`（"宿主给时机、控件维护会话"），相机只在第一次对准内容。
**应用级共享的只有外壳那一套**（DockPanel、Ribbon 菜单 —— 它们跟随"当前文档"的语义）。

由此两条原来的设计被**推翻**（记下来，别重提）：

- "会话必须全进程共享"不是硬件限制，只取决于 **vsg 的 `VSG_MAX_DEVICES`**。但那个常量的三种形状里，
  `>4` 的**动态分支在 vsg 1.1.16 是坏的**：`vk_buffer<std::vector<T>>` 的 `size()` 只在写时增长、读会越界
  （实测 `std::vector<vsg::ModifiedCount>::operator[]` 断言，`test_vsg` 当场 abort）⇒ **不改 upstream 的上限是 4**
  （`<=4` 的固定数组分支，实现正确）。我们取 **4**（`gfx_backend_vsg/CMakeLists.txt` 里显式设，原文"默认 1 是有意绊线"
  那句已作废）：可以同时开 4 份 3D 文档，各自一块设备；要"无上限"就得给 vsg 打补丁（本树的规矩是 vsg 保持原版 + 门禁
  盯着我们自己的用法，所以没做）。**注意**：`<=4` 分支的 `operator[]` **不做边界检查** ⇒ 第 5 块面必须由**我们**拒掉
  （还没做，见 §12.2 第 5 条）。
- 文档视图**不需要**把窗口交给谁：各自一块设备，互相看不到，所以"外壳的内容要退场"这类编排也不需要了（原来
  §12.2 第 5 条提的那两种形状全部作废）。

钉子：`tests/test_gui/ModelViewerTest.cpp` 的 `EachDocumentViewOwnsItsOwnRenderSurface`（`content() == 视图自己`；两份
文档的控件/引擎/view **都不是同一个**；关掉一份另一份还在）。


## 10. 落地顺序

1. **core（✅ 已落）**：`Document`（`source()`/`save()`）+ `DocumentManager`（类型注册含元数据、按载荷类型注册的打开器、
   `create`/`open`/`close`/`current`）+ 语义钉子（`tests/test_appfw/DocumentTest.cpp`）。
2. **元数据与面板（✅ 已落）**：`DocumentTypeInfo` + `gui::DocumentManagerDialog` + `app_shell` 的 `show_document_types`
   命令与 Ribbon 按钮（`tests/test_gui/test_gui.cpp` 的 `DocumentManagerDialogTest`）。
3. **视图层（✅ 已落 2026-09-27）**：`DocumentView` + `DocumentViewRegistry` + `CentralDocumentHost`（§7/§8），
   `GuiApplication` 建注册表与宿主（`viewRegistry()` / `centralDocumentHost()`），钉子见
   `tests/test_gui/DocumentViewTest.cpp`（5 条）+ `ModelViewerTest.ThePluginViewIsWhatTheHostPutsInTheCentralArea`（端到端）。
   **还差一件**：外壳仍然**无条件**建 `RenderControl`（`app_shell::load()` 一上来就建 ⇒ 等于强制每个 app 都有 3D 会话）；
   宿主接管中央区时会把它藏起来（不删、退出时还回去），但那个会话已经建了 —— 和第 4 步一起收。
4. **渲染（✅ 已落 2026-09-27）**：文档视图**自带** `RenderControl`（自己的面/会话/设备），`VSG_MAX_DEVICES` 取
   **4**（vsg 里唯一正确的那一档；`>4` 的动态分支在 1.1.16 是坏的，见 §9）。"一块会话服务多个 view"那套
   （`RenderControl::present()`、`SceneView::removeWindowPass()` 公开、宿主"借来的页"规则）**已删**：模型变了，
   无用的 API 不留。运行期 attach 是普通路径（`RenderControl` 的文档本来就写着 "THE HOST GIVES THE TIMING"）；
   "必须贴着主窗上屏"那条**只对启动期那个会话**成立（见 `appfw-startup-phases.md`）。

## 11. 本设计不解决 / 有意不做
- **单文档还是多文档**：由 app 决定。单文档 app 在 `opened` 里先关旧的（或用默认派生给的开关），多文档 app 用集合 +
  标签；两者共用同一套 `Document` 语义。
- **多窗口**（一个文档一个顶层窗口）：appfw 今天只有**一个** `MainWindow`，这是另一个量级的改动，不在本文范围。
- **`open(path)` 与格式探测**：需要"哪个类型能读这个路径"的 IO 概念（`iobase`/扩展名），留给第 2 步一起定。

## 12. 参考实现：`model_viewer` 测试插件（2026-09-27 落地）

上面说的这些有一个**可点的**例子：`src/plugins/model_viewer/`。它是测试插件，但按真实插件的形状写：

| 它做了什么 | 用的机制 |
| --- | --- |
| 登记文档类型 `"model"` | `registerType`（**没有** create 工厂 ⇒ 面板如实显示"否（只能打开）"） |
| **两个打开器**：内存网格（`MeshPayload`）/ 磁盘网格文件（`MeshFilePayload`，打开器里调 `MeshLoader`） | 一种类型吃多种载荷（§5.1）；**怎么读由打开器决定** |
| 右侧 dock 上的**模型信息面板**（类型/标题/**顶点数**/**三角形数**） | 面板是 header-only 的 `gui::Control`，**自己**听 `currentChanged`（见下） |
| **中央区的模型视图**（`ModelRenderView`，真渲染） | §7/§9：`registerView(u8"model", createRenderView)` —— 视图自带 `RenderControl`，`content()` 返回自己（一实例一页） |
| 两条命令：`open_test_model` / `open_mesh_file`（写一个最小 STL 再读回来） | 命令 `group` = `模型`；打开后自己 `setCurrent`（**宿主**策略，不是框架的） |

**面板自己听事件**：`ModelInfoPanel::followDocumentManager()` 只接 `currentChanged`（`opened` 不改当前选择，而当前文档
被关掉时管理器先发 `currentChanged(nullptr)`）。这是"面板自己的内容自己听"那一档；**结构**（谁显示、放哪）仍然归宿主，
所以插件只负责建它、挂上 dock。

**面板按类型认文档**：文档类型今天还没有自己的元数据，所以它比 `typeId()` 而不是 `obj_cast`；不是模型文档时照样显示
类型与标题，两个数说"—（不是模型文档）"，不假装 0。

### 12.1 它撞出的两个缺口（记下来，别当成没发生）

1. **`MeshLoader::load()` 只吃 `std::filesystem::path`**（没有字节 / `DataStream` / `Vfs` 重载）⇒ §5 说的"位置是
   `Vfs + 虚拟路径`"今天**读不进模型**：文件载荷只能带真实路径。要接上得给加载器加一个"字节/流"入口（把 `Vfs` 落成
   临时路径那条路不可取）。
2. **面板注册表还不存在**（§7/§8 那一层）：所以这个插件的面板是**自己建、自己挂**的
   （`createDockPanel(u8"模型信息", panel, DockAreas::Right)` + `setId(u8"dock_model_info")`）。有了注册表之后，这一段
   变成一条声明（带 `document_types`），"谁该看见"才由宿主统一决定。

### 12.2 还没做的，按依赖顺序

1. ~~`CommandStatus::NotApplicable`~~ **已落（2026-09-27）**：命令判断"这不是我的文档"不再只能报 `Failed`。
   三处用户可见路径都分开了：`logOutcome()` 记 info 级、`executeDetached()`（ribbon）与 `VisualUserIO` 的控制台回写
   把解释当提示转达（空解释则沉默），失败语仅在真的 `Failed` 时出现 —— 见 `appfw-command-manager.md` 的
   「结果状态」一节。
2. ~~`Command::documentTypes()` + `CommandInfo.document_types`~~ **不做（2026-09-27 用户拍板：命令内部自己获取
   文档并判断类型）**：不适用就返回 `NotApplicable`（把缺的那句话写进 `message()`），见
   `appfw-command-manager.md`「结果状态」。因此也**没有"按类型过滤 ribbon"**这一层：UI 少显几个按钮是观感问题，
   正确性已经由命令自己保证。附带好处：`Command` 的虚表不动，插件 ABI 不用从 7u 抬到 8u。
3. ~~视图注册表（§7）~~ **已落**：文档类型 → 视图（"编辑器里 mesh 与 b-rep 显示/交互不同"的地基）。渲染那一段也已落：
   视图**自带**渲染面（§9），不是共享会话。
4. 面板注册表 + 可见性声明（依赖 3 的编排骨架）。与第 2 条的区别要记住：**命令**有 `execute()` 这一步，所以"适用不适用"
   由它自己判；**面板**没有"执行"时刻，它的适用面只能由**声明**表达，再由宿主决定显隐。5. **同时开的 3D 文档数要有个明确的拒法（未做，必须做）**：`VSG_MAX_DEVICES=4` 的 `std::array` 分支**不做边界检查**，
   第 5 块渲染面写下去就是 out-of-bounds。所以要到第 5 份时由**我们**拒（后端数着活的会话数，超了就在
   `initialize()` 里明确失败 + 一句话；`RenderControl::init()` 因此返回 false，那份文档的页保持空白/给出提示），
   而不是等它去踩数组。