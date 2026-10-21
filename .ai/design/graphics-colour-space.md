# 颜色：把"色彩空间 + 数值域"放进类型（2026-10-21）

> 出处：`graphics-followups-2026-10-19.md` §3.1 的 F2b（原触发条件是"第二次同类色彩/数值域缺陷"，
> 用户本轮直接指定做）。本文件记做完之后的形状、判据与**不做**的东西；上一轮的复核记录在
> `graphics-abstraction-review-2026-10-20.md`。

## 0. 缺陷的三张面孔

1. **清屏色是引擎里唯一一个"字节"颜色**。`PassDesc::clear_color` 是 `Color`（0–255 字节），后端把它
   `/255` 之后当**线性**清屏值交上去。于是 `Color(51,51,51)` 想表达的"#333333"落成线性 0.2 ——
   在 sRGB 交换链上被硬件编码成 #7C7C7C，看着**亮了一个 gamma**。同一帧里 `Material` / `Light` /
   `FpsOverlay` / `AxisGizmo` 的颜色都是 `Colorf`（线性浮点）。同一个概念（作者给的颜色）两种拼法、
   两种语义。
2. **数值域与空间都只写在注释里**。`Color::toColorf()` 就是 `/255`，名字里没有一个字说"这一步**不**
   换空间"，于是它成了最容易写错的那一步：宿主拿调色板里的 sRGB 字节，除以 255 就当线性值交上去。
3. **反方向不存在**。没有"线性 → 8 位"的编码函数；`Colorf::toColor()` 是裸量化（同样不换空间），
   `Colorf::fromColor()` 又是 `toColorf()` 的第二种拼法。三个名字，每个只说一半真话。

## 1. 形状：两个类型，一个事实，一对有名转换

| 类型 | 数值域 | 色彩空间 | 谁填它 |
|---|---|---|---|
| `vn::Colorf` | 0..1 浮点（HDR 允许 >1） | **线性**（scene-referred） | 引擎内部与**每一个**交给 GPU 的颜色 |
| `vn::Color` | 0..255 字节 | **sRGB 编码**（display-referred） | 宿主手边的调色板 / PNG / `QColor` / 主题色 |

**唯一的一对转换，名字里带方向**：

- `Colorf Color::toLinear() const` —— sRGB 解码（IEC 61966-2-1 的分段曲线，**不是** `pow(x, 2.2)`）
- `Color Colorf::toColor() const` —— 线性 → sRGB 编码，再量化

退场的三个半真话：`Color::toColorf()`（`toLinear()` 的错语义前身）、`Colorf::fromColor()`（同一个
事实的第二拼法）、以及"裸量化"语义的 `Colorf::toColor()`。

**alpha 不参与编码**。alpha 不是颜色：`toLinear()` / `toColor()` 只换 r/g/b，a 只做 `/255` 与 `×255`。
编码 alpha 会让混合与透明读回全错，这是这条契约里最容易顺手写错的一行。

**传输函数只有一份实现**：`src/base/core/src/ColorTransfer.hpp`（私有头，两方向各一个 `inline`），
由 `Color.cpp` 与 `Colorf.cpp` 各取一半。分在两个 `.cpp` 里各写一次就等于两个事实。

## 2. SDK 边界：作者的颜色一律线性

`PassDesc::clear_color` / `clear_extra_colors`、`RenderPass::clearColor()` / `setClearColor()`、
`ClearPolicy::color` / `extra_colors`、`FramePlan::Pass::clear_color` 从 `Color` 改成 `Colorf`。
于是 **SDK 公开面只剩一种作者颜色**，与 `Material` / `Light` / `LightRig` / `FpsOverlay` 同型。

- 默认清屏色仍然是"`#333333` 深灰"这个**意图**，写法把意图与空间一起说出来：
  `const Colorf kDefaultClearColor = Color(51, 51, 51, 255).toLinear();`（≈ 0.0331）。
- 后端 `VsgBackend::announceClear` 不再 `/255`：线性浮点直接进 `core::ClearPolicy::color_value`。
- `FramePlan` 的报告文本跟着换词：`clear=linear(0.033,0.033,0.033,1.000)` —— 报告里说的空间与类型的
  空间一致，"rgba(51,51,51,255)"那种说法在两种空间下都读得通，正是要消灭的歧义。

### 2.1 可见后果（诚实登记）

窗口在本机拿到的 surface format 是 `B8G8R8A8_SRGB`（`VkCmdDrawIndexed-renderPass`-02684 的实测记录
见 `vsg-reimplementation.md`），硬件在写入时编码，所以：**demo 的窗口背景会从 #7C7C7C 变成 #333333**。
那不是回归，是这一轮修掉的缺陷第一次显形。UNORM 交换链平台上同一个线性值会落成更暗的字节 ——
那是平台层"格式不带编码"的问题（登记在 §4），不是颜色类型能修的。

## 3. 为什么不是"引擎替宿主解码"

D3 已经裁定（`vsg-reimplementation.md` §11.16cp）：**引擎内部一律线性，编码只发生在硬件两头**
（sRGB 交换链写入时编码、`*Srgb` 纹理采样时解码）。在 `clearPolicy()` 里替宿主做软件解码，等于在
第三个地方处理编码 —— 而那是引擎唯一看不见的一张图（宿主手里那张）。宿主在自己的边界解码一次
（`toLinear()`）、引擎只认线性，这条链上就没有第二个真相。

同理，`setClearColor` **不**提供 `const Color&` 重载：两个重载就是两个空间，编译器就再也拦不住
"拿字节颜色直接清屏"这个错误。

## 4. 明确不做

| 不做 | 为什么 |
|---|---|
| `UnitScalar`（0..1 强类型） | 没有读者：今天的 0..1 参数（alpha、opacity、anisotropy 夹紧值）都由 setter 自己夹紧且有用例；多一个类型只是每个签名多一层括号 |
| `RenderTarget::ColorFormat` 加 sRGB 变体 | 引擎词汇表**刻意**是投影：sRGB/线性不是引擎能分辨的那一半，设备格式（`device_color_formats`）才是 render pass 身份的一部分，跨族复用已按它拦住（`RenderPassCompatibility` 实测）。加一列只会出现"引擎说 sRGB、设备说 UNORM"的第二个真相 |
| 把 `Colorf` 改名 `LinearColor`、`Color` 改名 `SrgbColor` | 247 + 105 处，且 `geometry` / `robotics` / `loaders` 用 `Colorf` 表示**空间未声明**的作者数据（URDF `<color rgba>`）；改名等于把没核过的事实说成真的。类型层面该拿到的安全已经拿到：设计颜色参数是 `Colorf`，字节颜色不再能直接塞进去，唯一的桥是那对有名转换 |
| 8 位图与 sRGB 纹理的色彩空间 | 已经存在且正确：`imaging::PixelFormat` 的 `*Srgb` / `*Unorm` 变体 + 采样器解码 |

## 5. 判据

| 门禁 | 守住什么 |
|---|---|
| `ColorfTest.TheTwoSpacesAreExactInversesOverEveryByte` | 256 个字节值逐个往返：`Color → toLinear → toColor` 回到原值（alpha 也在内） |
| `ColorfTest.TheCurveIsSrgbNotAPowerOfTwoPointTwo` | sRGB 128 → 0.2159（`pow(2.2)` 会给 0.2195，差 2 个量化级）；线性 0.5 → 188（`pow(0.5, 1/2.2)` 会给 186） |
| `ColorfTest.ToColorClampsAndEncodes` | 越界夹紧 + 线性 0.5 落成 188（旧语义 128 会红） |
| `ColorfTest.AlphaCrossesTheBoundaryWithoutBeingEncoded` | a 只做 /255 与 ×255 |
| `ColorfTest.InvertedComplementsTheLightAndLeavesAlphaAlone` | `inverted()` 是 `1 - channel`，alpha 不动 |
| `ColorfTest.TheLinearComplementIsNotTheEncodedComplement` | 线性补色（0.2 → 0.8）与编码补色（#333333 → #cccccc → 0.6039）是两个数 |
| `ColorfTest.InvertMutatesInPlaceAndTheCopyAgrees` / `ColorTest.InvertMutatesInPlaceAndTheCopyAgrees` | 就地与返回副本是同一个操作（副本走前者，二者无法漂） |
| `ColorfTest.ThePresetsAreTheSameNumbersInBothSpellings` | 六个预设两边同数（0/1 是 sRGB 定点），以及 `toColor()` 与 `Color` 的同名预设相等 |
| `ColorfTest.LerpIsTheComponentWiseMix` | 逐通道混 + alpha 一起淡；`t = 0/1` 是两端，越界外推不夹紧 |
| `ColorfTest.LerpBlendsLightNotSamples` | 黑白中点是 **0.5 光 = 188**（混编码样本会得 128） |
| `RenderEngineTest.ADeclaredPassClearsTheWayItsKindDoesUntilItSaysOtherwise` | 默认清屏色 == `Color(51,51,51).toLinear()`（不是魔法浮点，也不是字节） |
| `RenderEngineTest.APassAnnouncesItsClearPolicyToTheBackend` | 声明成的、后端收到的都是同一个线性值（r/g/b 逐个比） |
| `RenderEngineTest.FramePlanDescribesThePassesInTheOrderTheyWillRun`（`clear=linear(...)` 那一行断言） | 宿主读的报表把空间说出来，格式也钉住 |
| `VsgBackendTest.AClearColourIsAStoredLinearValueNotAnEncodedOne` | 设备判据：离屏 RGBA8（UNORM）清 `Color(128,128,128).toLinear()` 读回 **55**；若引擎偷偷做了 sRGB 编码会是 128 —— 只有编码真的没发生，这个数才成立 |
| `ColorTest.HexTextIsOneTextPerValueAndTheEncodedHalf` | 文本是**编码那一半**：不透明写 6 位、半透明写 8 位；线性 0.5 写成 `#bc0000`（若写成 `#800000` 就是拿光当样本） |
| `ColorTest.HexTextParsesBothAlphaFormsAndForgetsTheCase` | 6/8 位都收、大小写都收；缺 alpha = 不透明 |
| `ColorTest.HexTextAcceptsTheShortFormsAndWritesThemBackInFull` | `#CCC` / `#C0F` / `#C0F8` 逐位重复；且短式只是**输入**：写回去是 `#cccccc` / `#cc00ff88` |
| `ColorTest.HexTextRoundTripsEveryValueItWrites` | 自己写的文本自己读回来相等（a = 0/1/128/254/255） |
| `ColorTest.EverythingElseIsARefusedParseNotAGuess` | 15 种“同一个值的别的拼法”逐个拒绝（`0x` 前缀、裸数字、2/5/7/9 位、`rgba(...)`、空白） |
| `ColorTest.TheOrderedFormsAreNotThePositionalOne` | `0x80FF0000` 在 `0xAARRGGBB` 与 `0xRRGGBBAA` 下是**两个颜色**：具名入口存在的理由 |
| `ColorTest.BgrIsColorRefOrderAndIgnoresItsHighByte` | `COLORREF` 的低字节是红、高字节不参与、结果不透明 |
| `ColorTest.TheOrderedFormsAreUsableInConstantExpressions` | `fromArgb` / `fromBgr` 是 `constexpr`（`static_assert`） |

## 6. 交互登记（不在本轮，别当成已解决）

- `geometry::ColorMaterial` / `PbrMaterial` / `PhongMaterial`、`robotics::io` 的 XML `<color rgba>`、
  `meshio` / `urdf2vine` 都持有 `Colorf`，而它们的**输入空间没被声明过**（URDF/SDF 实际按 sRGB 给值）。
  它们不经过 graphics 的 `Material`，所以本轮不牵动；哪天这些颜色进了渲染并且画面报"亮一个 gamma"，
  先在那一侧用 `toLinear()` 解码，别在引擎里加第二处解码。

## 7. 定位：类型只声明"值 + 转换"，空间由**消费方**声明

第一版把这段契约写进了 `vn::Colorf` 的类注释（"这是线性光，引擎一律线性"）。那是**过度限定**：`Color` /
`Colorf` 住在 `base/core`，同一个 `Colorf` 还被 `geometry` / `robotics::io` / `loaders` 拿来表示
**空间未声明**的作者数据（§6）。在 core 的头文件里宣布"这个类型就是线性光"，等于替那些模块许了一个
仓库兑现不了的承诺 —— 正是本模块反复拒绝的"第二个真相"，只不过这次是文档层面的。

所以收回到两句话：

| 写在哪 | 说什么 |
|---|---|
| `vn::Color` / `vn::Colorf` 的类注释 | 这个值**是什么**（8 位 sRGB 样本 / 浮点通道）、两个半之间唯一的桥是 `toLinear()` / `toColor()`（sRGB 曲线，`/255` 不是解码）、alpha 不编码 |
| `graphics` 的 `ClearPolicy` / `PassDesc::clear_color` / `RenderPass::setClearColor` | 本模块**怎么用它**：交给引擎的颜色一律线性，编码的在这一侧解一次；编码只发生在硬件两头（D3） |
| `docs/guides/graphics.md` §3.9 + 本文件 §2–§3 | 使用者视角的规则与理由（颜色 vs 数据、两条说错的方向、可见后果） |

判据仍然有效：类型层面该拿到的安全不靠注释，而靠**签名**（颜色参数是 `Colorf`，字节颜色塞不进去）与
**唯一的桥**（想换空间只能叫那个名字）。`Color` / `Colorf` 自己不需要知道谁在渲染。

## 8. 值类型还该有什么：一次按"读者"做的清点（2026-10-21）

有人问"是不是缺 `lerp` / 反色 / int32 解构 / 字符串"。**按本模块的规矩（没有读者的抽象不加）清点了一遍**：

| 候选 | 仓库里有读者吗（查过） | 结论 |
|---|---|---|
| 颜色插值 | 0 处生产读者，但用户判定它也是**基础接口** | **已落地 2026-10-21**：`static Colorf lerp(from, to, t)`（静态、对称，跟 `Quaternion::slerp` 同形）。**空间怎么写清的**：静态签名让"两个输入"都是显式参数，类型本身是浮点/线性那一半，注释把结论钉成一句可观测的事实（黑白中点是 **0.5 光 = 188**，不是 128）；不夹紧 `t`（越界就外推） |
| `Colorf` 的反色 | **0 处生产读者**，但用户判定它是**基础接口** | **已落地 2026-10-21**：一对拼法（跟 math 类型同一个约定）—— `invert()` 就地、`inverted()` 返回副本（副本由前者实现，不会漂）。语义是**线性补色** `1 - channel`（alpha 不动），与 `Color::invert()/inverted()`（补编码样本）是**两个数** |
| `Color` 的 int32 解构 | **有**：`explicit Color(uint32_t)`（`0xRRGGBBAA`）+ `toRgba32()` + 公开通道字段；`ConsoleTheme` 真的在用打包 ctor（10 处） | **已落地 2026-10-21**：既有拼法不动，补上**按顺序命名**的两个入口 `Color::fromArgb`（`0xAARRGGBB`）/ `Color::fromBgr`（`0x00BBGGRR`，`COLORREF`），见 §9.2。**没有**加 `toArgb32()`：反方向没有读者 |
| 颜色的字符串 | **0**：没有主题文件 / 样式表 / 配置解析；`ConsoleTheme` 是 C++ 字面量。（core 的 `Version` / `Uuid` 有 `toString()` 的先例，但那是它们有交换格式。） | **已落地 2026-10-21（用户点名做；短式按用户要求补于同日）**：`Color::toString()` + `Color::parse()`，**一种语法**（CSS hex：`#rgb[a]` / `#rrggbb[aa]`），见 §9.1 |
| 常用颜色的**静态成员** | **有读者**：`kWhite` / `kBlack` 在生产代码里 6 处（`AxisGizmo`、`FpsOverlay`、`DemoScene`），此前写成 `Colorf(1.0f, 1.0f, 1.0f, 1.0f)` 四元组 | **已加 2026-10-21**：与 `Color` 同名同义的六个（`kTransparent` / `kWhite` / `kBlack` / `kRed` / `kGreen` / `kBlue`，`k` 前缀按常量规矩，见 §8.1），`static const` + `.cpp` 定义（与 `Color` 同形——类内 `inline static constexpr` 自引用时类型尚未完整，不合法）。**这套能共享的理由是判据而不是习惯**：0 与 1 是 sRGB 曲线的**定点**，所以这些名字在两种拼法下就是同一组数（这就是为什么**不提供预设灰**：灰与空间有关） |

三条当时都进了待办总表 §3.1（带触发条件）。**现在三条全部落地**（插值 / 反色 / 预设 2026-10-21 同批，文本与
字节序见 §9），落地的形状就是下面这三句判据：

- 插值：**签名里带空间**（否掉的一个形状：`Colorf lerp(const Colorf&, const Colorf&, float)` —— 它把
  "被改的那个颜色"藏成了隐式主体）。`lerp` 已按这个口径落地：静态、对称、类型是线性那一半，
  判据把"0.5 光 = 188"钉住。
- 字符串：**只有一种语法**（否掉的形状：同时给 `#rrggbb` 与 `rgba(...)` 两种解析）—— 两个语法就是两个真相。
  落地时补了短式 `#rgb[a]`（用户点名）：那是**同一条语法的产出**，不是第二种语法（见 §9.1）。
- 字节序：**带序的具名函数**（`fromArgb` / `fromBgr` 那种），不是通用的移位/掩码接口。

反方向也点了名：`Color::toRgba32()` 与 `Color::inverted()` 目前只有自己的用例在读。`inverted` 这一对自
2026-10-21 起不再算死接口 —— 反色被定为基础接口，两个拼法各补各的空间（`Colorf` 补线性光、`Color` 补编码
样本）；`toRgba32()` 保留，因为它是打包 ctor 的自然另一半。

### 8.1 命名：常量就是常量（`k` 前缀），不给值类开例外

新增的六个预设让一处**没写清的规矩**显形了。仓库里同时存在四种写法，各管各的身份：

| 写法 | 例子 | 管什么 | 出处 |
|---|---|---|---|
| `k` + PascalCase | `kFingerprintSize`、`Colorf::kWhite` | **常量**（文件级或类内，值在程序生命周期内固定） | 规矩里写着 |
| `s_` + snake_case | `MemoryStream::s_minCapacity` / `s_maxBump` | 类内**持状态**的静态量 | 规矩里写着 |
| `static constexpr` 函数 | `Vector3::unitX()` / `zero()` | 同一个词汇，但用访问器表示 | 同名规矩（静态成员函数 camelCase） |
| snake_case 无前缀 | `Vec3d` 的公开字段（如 `x`） | 实例字段 | 规矩里写着 |

**第一版写成 `Color::White` / `Colorf::White`（无前缀 PascalCase）是错的**，而且当时的理由（"前缀标作用域，
这些是 API 词汇"）经不起对照：Google 风格指南的常量就是 `k` + 混合大小写（`kDaysInAWeek`），仓库自己也早就把
文件级常量写成 `kConfigFolder` 那样。前缀不是装饰，它标的是**这个名字是什么**（常量 / 状态 / 函数）——
"它在公开面上"不是开例外的理由。于是 `Color` 与 `Colorf` 一起改成 `kTransparent` / `kWhite` / `kBlack` /
`kRed` / `kGreen` / `kBlue`，规矩里那条 PascalCase 例外改成"常量规则也适用于类内静态常量"（
`.github/copilot-instructions.md`）。

代价诚实地记下来：`static const` + `.cpp` 定义意味着预设**不是常量表达式**——类内
`inline static constexpr Colorf kWhite{…}` 在自己的类体里遇到不完整类型，不合法。真需要在常量表达式里用颜色时，
正确形状是加一个 `static constexpr` 的**访问器**（按 `Vector3::unitX()` 的规矩命名），等第一个这样的读者。

## 9. 文本与打包字节序：一种语法、一个顺序一个名字（2026-10-21，§3.1 收口）

§3.1 最后两条（**触发条件都没命中**，用户点名做）落在同一个值类型上。它们的共同判据只有一句：别人手里的
东西能不能原样进来、原样出去，而且**同一种东西只有一种说法**。

### 9.1 `Color` 的文本：`#rgb[a]` / `#rrggbb[aa]`

| 方向 | 声明 | 契约 |
|---|---|---|
| 写 | `String Color::toString() const` | 小写、一个 `#`；alpha 对**只在非不透明时**写（`#ff0000` / `#ff0000fe`）⇒ 一个值只有一份文本，而"缺席"永远只表示一件事：255 |
| 读 | `static std::optional<Color> Color::parse(const String&)` | 同一个语法：`#` + **3 / 4 / 6 / 8** 位十六进制（大小写都收）；短式逐位重复（`#CCC` = `#CCCCCC`，`#C0F8` = `#CC00FF88`），缺 alpha = 不透明。**失败在类型里**（空 optional），不在注释里，也不靠哨兵值 |

为什么是这一份语法：

- **文本属于编码那一半**。`#rrggbb` 是调色板 / 样式表 / 取色器给你的那种东西，它就是 sRGB 样本；线性光的
  文本必须把空间写进字符串里，那就是第二个真相。要线性值的文本先走 `toColor()` —— 判据把这一点钉成数字：
  线性 0.5 写成 `#bc0000`（188），不是 `#800000`。
- **收整族 CSS hex，但只写一种**。读侧收 3 / 4 / 6 / 8 位：短式是 CSS 同一条语法的**产出**（每位重复一次），
  拒掉它的一半（只收 3 不收 4）才是任意的洞。写侧只有规范形式（6 或 8 位、不透明不带 alpha 对），
  所以**一个值仍然只有一份文本** —— `#CCC` 读进来、写出去就是 `#cccccc`。
- **拒掉的邻居**：`0xRRGGBB`、裸数字、`rgba(255,0,0,1)`、2 / 5 / 7 / 9 位、前后空白 —— 每一种都是同一个值的
  第二种**拼法**（不是第二种长度），而格式契约存在的意义就是只有一种。
- **不给 `Colorf` 文本**。`Colorf` 的文本必须先回答"这是哪个空间"；`Color` 的文本已经回答了。

实现上两处刻意的选择：十六进制数字是手写的 `hexDigit()`，不走 `String::toInt(&ok, 16)`（后者收符号、
前导空白和 `0x` 前缀，正好是要拒绝的那几种）；写侧复用 `String::hex(span)`，与 `String` 自己的十六进制
拼法只有一份。

### 9.2 打包字节序：顺序就是名字

| 入口 | 布局 | 它替谁说话 |
|---|---|---|
| `explicit Color(std::uint32_t)` / `toRgba32()` | `0xRRGGBBAA` | 既有拼法，`ConsoleTheme` 在用 |
| `static constexpr Color fromArgb(std::uint32_t)` | `0xAARRGGBB` | Qt `ARGB32`、Win32 `ARGB` |
| `static constexpr Color fromBgr(std::uint32_t)` | `0x00BBGGRR`（高字节忽略，结果不透明） | Win32 `COLORREF` |

- **不收通用移位 / 掩码接口**：`(v >> 16) & 0xFF` 读不出它以为的顺序，而 `0x80FF0000` 在两种顺序里是两个不同的
  颜色 —— 这正是具名入口要拦的错误，所以顺序写在**名字**里，不写在注释里。
- **`fromBgr` 忽略高字节**：`COLORREF` 的高字节不在值里，带标记的与不带的是同一个颜色；它也**没有 alpha**
  （`COLORREF` 没有覆盖率），结果一律不透明。
- **没有 `toArgb32()` / `toBgr()`**：反方向今天没有读者（要有"把值交给持有那种 int 的 API"才需要），
  而 `toRgba32()` 已与打包 ctor 成对。加了就是一条只能靠用例养着的接口。
- 两个都是 `constexpr`（`static_assert` 钉住），所以常量表可以直接写 `Color::fromArgb(0xFF112233)`。

判据全部在 §5 的表里（`ColorTest.HexText*` / `EverythingElseIsARefusedParseNotAGuess` /
`TheOrderedFormsAreNotThePositionalOne` / `BgrIsColorRefOrderAndIgnoresItsHighByte` /
`TheOrderedFormsAreUsableInConstantExpressions`）。
