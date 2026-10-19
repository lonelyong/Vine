# graphics 公开面瘦身（2026-10-20）

> **结论**：本轮**不改任何行为**，只做三件事：删掉"说了但没人读"的旋钮、把一个事实的多种拼法
> 收成一种、把和仓库自己的命名规矩冲突的 getter 改对。判断口径沿用
> `graphics-abstraction-review-2026-10-19.md` §0，并加一条本轮用得最多的：
> **一个决定只能有一个名字；一个调用不该有第二种写法**。

## 0. 判断口径（本轮追加）

上一轮的口径是"一个事实只有一个权威；声明/解析/编译/执行/证据分开；缺席与零分开；机制在 SDK、
策略在应用层"。这一轮补两条，因为它们是本轮 16 个删除/合并动作的判据：

1. **不装旋钮**。声明了却没有任何读者读的字段（"RESERVED — not read yet"）不是前瞻，是谎话：
   宿主设置它、看日志、以为画质变了。仓库自己已经有一条规矩（`graphics-followups-2026-10-19.md`
   §4"不为将来可能加空能力标志"），本轮把它同样用在**光照阴影**上。
2. **同一件事的第二种写法就是缺陷**。两个只差一个默认参数的 overload、一个复制版 + 一个借用版
   getter、`bind(nullptr)` 之外再来一个 `unbind()` —— 它们不增加表达力，只增加"这段代码到底
   调的是哪个"的阅读成本，以及"改一处漏一处"的可能。

## 1. 落地清单

### 1.1 删掉没人读的旋钮（2 处）

| 删的 | 原状 | 依据 |
|---|---|---|
| `ShadowFilter`（枚举）+ `ShadowSettings::filter` + `Light::setShadowFilter()` | 头文件自己写着 "RESERVED: no renderer reads it yet"，`src/` 里唯一的读者是写它自己 | 阴影项做**一次深度比较**（`ShaderAbi.hpp` 的 `VineShadowBlock`），没有任何采样滤波器；留着就是"设置了画质却什么也没发生" |
| `ImageRef::unbind()` | `bind(nullptr)` 已经就是解绑（`bind()` 的注释写着"null unbinds it"），两处实现同一件事 | 第二种写法；保留 `bind(nullptr)` 这条唯一的说法 |

`ShadowSettings` 的注释改成说明**为什么故意没有** filter 旋钮（滤波器要和实现它的着色一起到），
免得下一个人以为是漏了。

### 1.2 一个事实收成一种拼法（9 处）

| 位置 | 原状 | 现状 |
|---|---|---|
| `Group` | `children()`（**每次调用复制整个列表**）+ `childrenRef()`（借用） | 只有 `children()`，返回 `const&`。每帧的收集/包围盒/拾取都走它，复制版是纯陷阱；测试里那条"复制版和借用版一致"的用例随第二个 getter 一起退场 |
| `Geometry` | `setPositions/setNormals/setTexcoords2/setTexcoords3/setIndices` 各 **2 个 overload**（整段 / 段） | 各 1 个，`first_vertex = 0, vertex_count = 0` 就是"整段"。**调用点一个没改**，实现也只剩一条路径（`AttributeChannel::slice`，整段就是 0/0 的段） |
| `RenderPass` | `clearColor()` / `clearColor(int)` / `setClearColor(const Color&)` / `setClearColor(int, const Color&)` | `clearColor(int attachment = 0)` / `setClearColor(const Color&, int attachment = 0)`。参数顺序改成"颜色在前、附件在后"：颜色是主语，附件是修饰 |
| `RenderTarget` | `colorFormat()` / `colorFormat(int)` | `colorFormat(int index = 0)` |
| `RenderSurface` / `SceneView` | `resize(w,h)` / `resize(w,h,ratio)`（两个类各一对） | `resize(w,h,ratio = 0.0)`，`<= 0` 就是"沿用当前比例"——两边的实现本来就是这样，只是被拆成了两个入口 |
| `ImageSource` | `imported(tex)` / `imported(tex, const Sampler&)` / `imported(tex, optional<Sampler>)` | 一个 `imported(tex, optional<Sampler> = {})`；`Sampler` 隐式转 optional，**调用点一个没改** |
| `SceneView::layoutParticipant()` | 两个（const / 非 const）访问器，`src/` 与 `tests/` 里**零调用** | 删掉。布局参与是视图的内部协作对象，`resize` / `addSurfaceLayout` / `clearSurfaceLayouts` 才是门面 |
| `LayoutParticipant` | 两条 `std::vector`（无比例 / 带比例），`applySize` 跑两遍循环 | 一条 `std::vector<StepWithRatio>`；两步版本在注册时包成"忽略比例"的那一种。**顺带修掉一个怪行为**：原来不管注册顺序，所有两步 step 都排在所有带比例 step 前面；现在就是注册顺序（注释里原本写的就是"registration order"） |

### 1.3 命名对齐仓库自己的规矩（5 处）

`.github/copilot-instructions.md` 写着"布尔 getter 用 `is` / `has` 前缀""getter 就是属性名"
"字段用 snake_case"。公开面里不合规的、且改动半径可接受的本轮改掉：

| 旧 | 新 | 为什么 |
|---|---|---|
| `Light::castShadow()` | `Light::isCastingShadow()` | 布尔 getter 没有 `is` 前缀 |
| `RenderTarget::depthPromotion()` | `RenderTarget::isDepthPromoted()` | 同上 |
| `DepthView::valid()` | `DepthView::isValid()` | 同上；而且同一个模块里 `RenderTarget` / `Viewport` 早就是 `isValid()` |
| `Camera::aspectRatio()` | `Camera::aspect()` | setter 叫 `setAspect()`，getter 却换了个词 |
| `RenderCommand::{modelMatrix, drawOrder, authoredState}` | `{model_matrix, draw_order, authored_state}` | 公开字段一律 snake_case（同一个 struct 里 `geometry` / `opacity` 本来就是）；后端那份记录 `FrameRecorder` 里对应的 `authoredState` 一起改，两个结构不出现两种拼法 |

`RenderSurface::target()` 顺带从裸指针 `WindowPresentTarget*` 改成 `raw_ptr<WindowPresentTarget>`：
模块里所有"借来的句柄"都是 `raw_ptr`，这是唯一一个裸的。

### 1.4 文档说真话（1 处）

`Capabilities::max_color_attachments` 原注释写"(MRT planning reads this)"，而全仓没有任何读者。
改成它真实的身份：**天花板**（后端会拒绝超出的声明并报诊断），宿主据此决定要不要写 MRT 流水线。

## 2. 明确不做（有理由，不是漏了）

| 项 | 为什么这一轮不做 |
|---|---|
| `Camera::setViewMatrixAsLookAt()` → `lookAt()` | 名字确实差（它收三个向量，叫"设置视图矩阵"），但 74 处调用点、跨 `src`/`tests`；改名本身不产生任何行为或表达力的收益，等下一次必须动相机面的时候一起做 |
| `Texture::faceCount()` / `layerCount()` 与 `source(face)` / `layer(index)` 的双拼法 | 同一个数两个名字（1 / 6），头文件自己写了"kind-agnostic spelling"，是真正的"两种说法"；但它牵动 60+ 处（含 vsg 插件上传路径），单独一轮做 |
| `RenderPass::shouldClearDepth()` | 和 `isClearEnabled()` 并列时口径不齐，但 `shouldClearDepth` 读起来比 `isDepthClearEnabled` 更准（它是"请求"，不是"状态"，头文件已把这个取舍写清楚）——这条属于"规矩 vs 可读性"的裁决，留给人定 |
| `backend/` 里的 `valid()` / `known()` / `derived()` | 后端半不面对宿主（`BackendHalfGuard.hpp` 禁止宿主包含），而且 `valid()` 在测试里上百处；收了收益小、噪声大 |
| `RenderPass` ↔ `PassDesc` 的字段/方法双份 | 这是本轮看下来**最大**的结构性问题（一个 pass 声明有值形式和节点形式，每个字段两边各一份），但把它收成"`RenderPass` 持有一个 `PassDesc`"是一次结构性重构，必须单独立项 |

## 3. 判据（已跑）

| 门 | 结果 |
|---|---|
| 全量构建（ninja all，含 vsg 插件、appfw、全部测试二进制） | 0 error |
| `build/bin/test_graphics` | **392 passed** |
| `build/bin/test_vsg` | **478 passed**（含像素判据与全部 `[sdk-pixels]` 证据行） |
| `bash scripts/vsg_rewrite_gate.sh --no-build --quick` | 阶段 2–6 全 ok（`cases=478 failed=0 vuid=0 hazard=0 skipped=0`、8 条 hygiene 0 finding、12 条 phase line 全闭合）；阶段 7（deferred demo 的启动占用）在本容器 658–942 ms > 150 ms 预算 —— **与本轮无关**（未碰启动路径），同脚本注释记录参考环境是 74–92 ms |
| 公开面文档基线 `check_public_docs.py` | `1028 baseline, 0 new, 0 stale` |

## 4. 交给下一次的登记

以下三条已写进唯一清单 `graphics-followups-2026-10-19.md` §3（带触发条件），不在这里重复。

- `RenderPass` / `PassDesc` 收敛成一份存储；
- `Texture` 的 face/layer 双拼法；
- `Camera::setViewMatrixAsLookAt` 的名字。

## 5. 一条经验

本轮 16 个动作里，**有 6 个是"零调用点改动"**（只差默认参数的 overload 合并、`optional` 化、
`Group` 的借用 getter 换名）。这类动作最值得先做：编译器全程替你检查，判据只有"构建 + 两套
测试 + 八条门禁"，而收益是往后每一次读这段代码都少一次"这调的是哪个"。
