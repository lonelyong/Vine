# graphics review 11: 三维概念、规范与语义

> **Status**: draft, non-authoritative — 工作记录。§2 是源码实际陈述的三维约定，§3 是发现，§4 是不建议照搬的对标项，§5 是建议顺序，§6 是复核命令。本轮除本页外不改任何文件；每条发现都留给 owner 决策。
> **Last verified**: 2026-11-25, tree `9829e682` on `dev`（工作区干净）。
> **Scope**: 前十轮审查分别走了实现质量(7)、增量编辑/线程(6)、seam 概念(8)、vk 后端系统(9)、规范语料/概念清单/扩展面(10)，**没有任何一轮把三维语义本身当审查维度**。本轮只看：空间与矩阵约定、相机/投影/拾取、变换层级、几何与光照材质的三维语义。不重走 6–10 的线程、帧机制、后端系统、概念盘点。
> **Sources**: `src/viz/graphics/sdk/vine/graphics/*.hpp`、`src/viz/graphics/src/**`、`src/viz/graphics/shaders/**`、`src/plugins/gfx_backend_vk/src/internal/**`、`src/base/math/**`、`tests/test_graphics/**`、`tests/test_vk/**`、`.ai/design/graphics-*.md`、`.ai/decisions/**`。
> **Backlog**: 本轮不擅自加条目；§5 列出候选，`T#` 编号留给 owner。
> **Numbering**: `V#` 是本轮自己的编号（8/9/10 轮用过 `C#`/`K#`/`L#`）；模块的 `F#`/`C#`/`R#`/`N#`/`P#`/`D#`/`T#`/`G#` 仍是设计页的编号。
> **Rule of evidence**: 每条发现给出 `file:line`、测试名或可复跑命令。对标陈述说的是 peer 的公开设计，不是性能基准。

## 1. 为什么需要这一轮

一个图形模块可以在工程意义上完全自洽——seam 干净、帧值不可变、诊断齐全、ABI 有单一声明——同时在**三维语义**上错误：法线用错矩阵、镜像翻转剔除面、无设法线的高亮读到未定义输入、相机退化时静默产出零矩阵、光照单位与导入格式不一致。这类错误不会被 review 9/10 的"一个概念一个 owner / 规范是否描述树"发现，因为它们的 owner 存在、拼写也一致，错的是**数学含义**。

本轮问三个问题：

1. **概念**：模块有没有一个统一的三维规范（手性、up 轴、世界单位、空间流转），还是只散落在各文件注释里？(§3 `V1`)
2. **语义**：变换 → 视图 → 裁剪 → 着色这条链上，每个量的数学含义是否与文档、测试、peer 的通行约定一致？(§3 `V2`–`V8`)
3. **框架**：约定变更（法线矩阵、front-face、单位）落在哪一层才不会变成第二个真相源？(§3 各发现的 fix 栏)

### 1.1 本轮对标谁、为什么

| Peer | 读的机制 | 为什么是合适的参照 |
|---|---|---|
| three.js | `object.normalMatrix.getNormalMatrix(modelView)`；`frontFaceCW = matrixWorld.determinantAffine() < 0` | 最直接的法线矩阵 / 负行列式反面判定参照，且是公开源码 |
| OSG | `PositionAttitudeTransform`、`AutoTransform` 的命名与"父变换被自动旋转抵消"语义 | 本模块的节点名与行为直接继承 OSG，应核对是否继承完整 |
| glTF 2.0 + `KHR_lights_punctual` | 右手 Y-up、米；法线图 OpenGL 朝向；光强 cd/lux、`1/d²` 衰减 | 材质的通道打包与导入表已经声明 glTF 兼容，光照侧应对齐同一参照 |
| Vulkan / D3D12 | NDC 深度 [0,1]、framebuffer y 向下；reverse-Z 是可选约定而非 API 规定 | 本模块把 GL 约定留在 SDK、在 vk 边界折叠，需要核对两约定边界的陈述 |
| Unity / Unreal / Filament | 世界单位显式、逆置换位法线、阴影 PCF、IBL 作为独立环境项 | 判断"刻意不做"与"尚未做"的区别 |

对标不是依赖，也没有代码复制；只回答"peer 如何定义这个量、定义写在哪里"。

## 2. 模块当前陈述的三维约定

下面是把源码注释、实现与测试放在一起后，模块**实际**持有的约定。它本身是本轮的产出之一：这些句子目前没有一个统一的规范页承载。

| 空间 / 量 | 当前含义 | 承载处 |
|---|---|---|
| 世界 | 右手、默认 up `(0,1,0)`、相机默认 eye `(0,0,5)` 看向 `(0,0,0)`；**无世界单位声明** | `Camera.hpp:250-252`、`Camera.cpp:11-15` |
| 世界 CCW | 正面为世界空间逆时针；vk framebuffer 下翻后 → `frontFace = CLOCKWISE` | `StateMapping.hpp:32,44`、`ContentCaches.cpp:780` |
| 视图 | `lookAt` 行基（right/up/backward），相机看向视图 −Z；eye==target 时返回**零矩阵** | `Transform3.cpp:61-115`（`68-70` 退化路径） |
| 裁剪（SDK） | 经典 GL：x/y ∈ [−1,1]、z ∈ [−1,1]，near = −1、far = +1 | `Camera.hpp:38-43`、`Transform3.hpp:280-296` |
| 裁剪（vk 设备） | `fold = diag(1,−1,−0.5)` + `z'=0.5−0.5z`：y 下翻、reverse-Z、near = 1、far = 0 | `ContentDraw.cpp:87-93` |
| 视锥剔除 | 从 SDK 的 `P*V` 用 Gribb–Hartmann 行提取六面；p-vertex 测 AABB | `Frustum.hpp:39-82` |
| 几何通道 | location 0 位置 / 1 法线 / 2 颜色 / 8 UV / 12–15 实例矩阵，列主序 | `ShaderAbi.hpp`、`Geometry.hpp`、`Primitive.hpp` |
| 着色 | 全部在**视图空间**：灯随相机旋转平移后打包；GGX/Smith/Schlick 单一 BRDF；漫反射无 `1/π` | `LightAbi.cpp:117-191`、`vine_params.glsl:100-151`、`vine_brdf.glsl:58-80` |
| 透明度 | 排序键 = draw order → layer → 不透明/透明 → 量化相机空间深度 → tie；代表点是世界盒中心 | `DrawListBake.hpp:425-460`、`DrawListBake.cpp:525-545` |

这里**做对的地方**也应明确：reverse-Z 在唯一边界折叠且有像素级诊断记录（不是两处各自实现）；forward 与 deferred 共用一份 BRDF、一份材质采样、一份灯光求值；实例矩阵列主序在 C++、GLSL、测试三处同源；透明排序用相机空间深度且用盒中心而非节点原点。本轮的问题不在这些机制，而在上面这张表的**语义落地**。

## 3. 发现

### V1（概念 / 中高）：三维规范没有单一 owner，手性、up 轴、世界单位都是"默认而非声明"

**证据**：`Camera.hpp:250-252` 的 `(0,0,5)/(0,0,0)/(0,1,0)` 隐含右手 Y-up；`Transform3.hpp:220-225` 的说明只在 `lookAt` 一处提到右手 −Z；`Light.hpp:243` 的 range 只说"world units"；`Geometry.hpp:489` 说无顶点法线时渲染器会推导；`ADR-2026-11-04-surface-placement-value-type.md` 只规定 `geometry::Placement<T>` 恒右手，明确是局部值类型。`src/loaders` 全文搜不到 `up_axis`（`tests/test_meshio/ModelLoadTest.cpp` 有 `Y_UP` 样本但没有消费端转换）。

**为什么是问题**：`Placement<T>` 恒右手是 CAD/几何侧的决策，不能自动上升为 graphics 世界的规范；导入器保留文件的 up 轴而渲染器默认 Y-up，Z-up 资产在不显式旋转时会"躺着"。host 无法向 API 询问手性/up/单位，只能在多处重复假设——而本项目的纪律恰好是"一个概念一个 owner、禁止第二个拼写"。

**对照**：glTF 把右手 Y-up、米写进规范；Unity/Unreal 把坐标系与单位写进工程设置；OSG 把 up 作为相机变量的显式默认值。

**fix**：新建 ADR 作为唯一规范页，声明世界手性、up 轴、世界单位（以及是否按米解释 glTF 光照强度），规定导入边界的 up 轴转换归属；在 `docs/guides/graphics.md` 给出可查询常量或类型，而不是让每个调用点推断。**ADR + doc + test**（导入一个 Z-up 样本并断言姿态）。

---

### V2（渲染语义 / 高）：非均匀缩放下法线用 `mat3(modelView)` 而非逆转置

**证据**：`builtin_forward.vert:68` 与 `builtin_gbuffer.vert:60` 都是 `vn_view_normal = mat3(vn_model) * vn_Normal;`；`PositionAttitudeTransform.hpp:28-29` 明确"非均匀缩放合法"，`PositionAttitudeTransform.cpp:81-82` 实际构造 `T*R*S*T(-pivot)`；`ShaderAbi.hpp:131-134` 的 `VineDrawBlock` 只有 `mat4 model` + `vec4 params`，全模块没有 `normalMatrix` 或等价通道。

**数学后果**：把法线当作方向直接乘 `M` 只在正交（纯旋转+均匀缩放）下等于正确的 `det(M)·(M⁻¹)ᵀ n`。取 `n=(1,1,0)/√2`、`S=(10,1,1)`：现实现得到 `(0.995,0.0995,0)`，正确值 `(0.0995,0.995,0)`，两个方向近似垂直。一个沿轴拉伸的 CAD 零件或机械臂连杆会整片打错光；法线图也一样错，因为切线基建立在错法线上。

**对照**：three.js 每次渲染 `object.normalMatrix.getNormalMatrix(object.modelViewMatrix)`（即逆转置）并作为 uniform 上传；Unity/Unreal/Filament 同理。

**fix**：在 SDK 的 ABI 层给每个 draw 增加法线矩阵（3×4 或 mat4，std140 对齐），由唯一 packing 函数从 world/view 矩阵导出，forward 与 gbuffer 顶点阶段共读；或者把缩放限制为正均匀缩放并**修改 `PositionAttitudeTransform` 的公开契约**（当前文档明确允许非均匀，因此不能当作已限制）。**code + ABI + test**（非均匀缩放下平面法线的像素/数值断言）。

---

### V3（几何语义 / 高）：负缩放翻绕序，但 front-face 固定为 CLOCKWISE

**证据**：`PositionAttitudeTransform.hpp:29` 明确负缩放镜像并翻转绕序；`AutoTransform.hpp:108` 同样允许；`StateMapping.hpp:44` 固定 `VK_FRONT_FACE_CLOCKWISE`，`ContentCaches.cpp:780` 无条件写入管线；全后端无按行列式翻转的代码（`grep frontFace/kFrontFace` 只有这几处）。

**后果**：启用背面剔除时，镜像子树的外表面被当成背面剔除，内表面反而可见；`CullMode` 无论取 Back/Front 都得到内外颠倒。three.js 的 `frontFaceCW = matrixWorld.determinantAffine() < 0` 正是这个常见情况的修正。

**fix**：在 draw 的解析状态里增加"负行列式"事实，由 pipeline key 或动态 front-face 状态消费；必须对实例矩阵逐 draw 判定（`Primitive::instance_transforms` 可以每个实例不同行列式，若用静态管线翻转则还需按实例分批，或改用双面/自定义剔除策略）。先决定"镜像是否正式支持"：若不支持，把 setter 改为拒绝负缩放并写入诊断，与现有"非幂次样本数拒绝"的风格一致。**code + ABI + test**（镜像盒的像素剔除断言）。

---

### V4（几何语义 / 高）：无顶点法线的几何在默认平滑程序下读到未定义输入；文档声称会被推导

**证据**：`Geometry.hpp:489` 写"when unset the renderer derives normals from the positions"；实际 `ContentCaches.cpp:964-987` 只把几何**存在**的通道写进顶点输入，缺失的 location 1 不绑定；`builtin_forward.vert:16-17,68` 无条件读 `vn_Normal` 并乘入 `vn_view_normal`；`builtin_forward.frag:60` 直接 `normalize(vn_view_normal)`；`builtin_gbuffer.frag:46` 同样。全仓库搜不到任何由位置合成法线的实现（`grep hasNormals` 只命中容器查询）。`VN_FLAT` 变体使用 `dFdx/dFdy` 求面法线（`builtin_forward.frag:48-58`），因此只有选 flat 程序时才碰巧正确。

**后果**：没有法线的 Mesh（导入器在源网格无法线时会产生；`tests/test_graphics/GraphicsTest.cpp:2421` 就在断言这种几何）用默认 lit 程序绘制时，读到 Vulkan 未定义的顶点输入，`normalize` 可产生 NaN → 整片异常颜色或斑块。这与"DSL/值语义"的工程正确性无关，是三维数据契约不成立。

**对照**：glTF/three.js 允许无法线的几何，渲染端使用 flat/面法线回退；OSG 也把法线生成当作明确的局部策略。

**fix**：二选一并写进规范：(a) 在内容桥 `geometryFromShape()` 与 `setPositions()` 路径里生成面法线（数据侧），或 (b) 让默认程序在缺失通道时使用 `VN_FLAT` 顶点/片元路径（渲染侧）。同时更正 `Geometry.hpp:489` 的句子，并加一条端到端像素测试。**code + doc + test**。

---

### V5（相机语义 / 中高）：退化相机静默产出零矩阵，视锥参数无校验，存储的 up 不是实际基

**证据**：`Transform3.cpp:68-70` 中 `eye == target` 直接 `return {}`，`Camera.cpp:34-40` 原样接受；`setPerspective()`（`Camera.cpp:42-48`）不检查 `fovy`、`zNear>0`、`zFar>zNear`，`setOrthographic()`（`:50-62`）不检查左右/上下/近远顺序。对照 `setAspect()`（`:129-134`）明确忽略非正值并有测试（`GraphicsTest.cpp:1033-1036`）——同一类的校验风格在视锥参数上缺席。`Camera.cpp:38` 只把传入 up 归一化后存起来，但 `lookAt` 在 up 与视线平行时**另选参考轴**（`Transform3.cpp:76-96`），`up()` 于是报告的不是视图矩阵实际使用的 up；`CameraSnapshot::up` 把该值带给 `AutoTransform.cpp:250` 的 billboard 朝向。

**后果**：eye==target 时视图矩阵全零 → 视锥六面退化、不剔除任何东西、GPU 端 model-view 全零；near/far/fov 非法时投影矩阵产生 NaN 或负深度区间，只有画面异常而无诊断。平行 up 时 billboard 朝向与画面不一致。

**对照**：three.js/OSG/Unity 都把退化 lookAt 与非法视锥当作调用错误或显式回退；本项目自身对非幂次样本数采用"拒绝并报告"。

**fix**：给 `Camera` 增加命名拒绝/回退答案（沿用 `PlanRefusal`/`EngineNotReadyReason` 风格），退化 lookAt 保持上一有效视图并报告；把 up 设置为 `lookAt` 解析后的真实基向量，或让 `up()` 从 `view_` 反解；补四条参数校验测试。**code + test**。

---

### V6（约定边界 / 中）：SDK 与设备的裁剪约定分裂，个别文档句子把两者说反了

**证据**：`Camera.hpp:38-43` 正确声明 SDK 投影是 GL z∈[−1,1]，`ContentDraw.cpp:87-93` 在唯一边界折叠为 reverse-Z [0,1]+y 下翻。但 `CameraSnapshot.hpp:58-60` 说"the projection carries which clip depth is NEAR (this tree renders Reverse-Z)"，而 `CameraSnapshot::of()`（`Camera.cpp:188`）写入的正是 GL 投影；`CameraSnapshot::screenRay()`（`Camera.cpp:204-206`）实际靠"到 eye 的距离"判定哪端近，所以代码是对的、**句子是错的**。`BuiltinShaders.cpp:125-126` 说光裁剪 z 是"0 at near, 1 at far"，同段代码 `:137` 用 `1 - (z*0.5+0.5)` 把 GL 的 −1 映到 1（近），+1 映到 0（远），注释同样说反。

**后果**：宿主如果把 SDK 的 `projectionMatrix()` 当作设备投影使用（写自己的 depth effect、GPU 端 unproject、重建深度），画面会静默错误；错误的注释会让维护者在改阴影或拾取时按相反的深度方向推理。代码目前靠"距离判定"和"单一折叠点"躲过了 bug，但躲不过下一个读者。

**fix**：保留双层约定（这是合理设计），但把约定表提升为一份规范页（可与 `V1` 的 ADR 合并），写明"谁产出 GL 投影、谁折叠、谁不得折叠两次"，并改正上述两处注释；补一条测试断言 `CameraSnapshot::projection` 的 near 行为 GL 的 −1 而不是设备的 1。**doc + test**。

---

### V7（光照语义 / 中）：光照单位与 glTF/物理约定不一致，"PBR"标签强于实际单位

**证据**：`vine_params.glsl:127` 的衰减是 `1/(d²+1)`（再乘 range 窗口 `:133-134`），而 `KHR_lights_punctual` 规定 point/spot 用 candela、directional 用 lux，衰减 `≈1/d²`，推荐 `window / d²`；`Light.hpp:67,182` 的 intensity 只是无单位 multiplier；`vine_brdf.glsl:15-18` 明确漫反射不带 `1/π`（为兼容旧亮度），而镜面是物理形状的 GGX；`LightAbi.cpp:185-189` 在无灯时填 0.15 ambient fill。材质侧 `graphics-material.md:179` 已承认无环境贴图时金属近黑。

**后果**：glTF 的 `KHR_lights_punctual` 强度导入后亮度与参考渲染器不可比；漫反射与镜面的量纲混用意味着"调高 roughness"改变的是两种不同单位项的比例，PBR 参数不能跨工具复用；金属件在无 IBL 的场景里几乎只剩高光。这是**刻意选择的单位约定**（文档写明了），但对外宣称 metallic-roughness PBR / glTF 兼容时应同时声明单位例外。

**对照**：glTF 的上述规范；Filament/Unity 把 IBL 作为独立环境贡献，方向光的 lux 与点光的 cd 在类型上区分。

**fix**：写清"引擎自有 intensity 单位"的 ADR/指南条目，并标明与 candela/lux 的换算或"不可直接互换"；若要严格兼容，则把衰减改为 `1/d²`（带软化下限）、漫反射补 `1/π` 并整体调权，同时更新导入表与像素基线。这是语义决策，不宜只改一行 shader。**decision + doc（+ code 视选择）**。

---

### V8（阴影 / 中）：单次深度比较、仅首个投影灯、固定偏移，属于"刻意简化"但需被标注为能力边界

**证据**：`BuiltinShaders.cpp:143` 一次 `texture().r` 比较，无 PCF；`Light.hpp:39-41` 明确没有滤波旋钮；`Light.hpp:284-288` 与 `ChainPresets.cpp` 只为首个启用且投影的 directional/spot 灯建深度 pass；`ShadowSettings::bias` 是归一深度标量（`BuiltinShaders.cpp:143` 直接加到 reverse-Z 深度上），无 slope-scale 或基于世界单位的偏移。backlog `S6`（逐灯阴影 / 立方阴影）与 `S7`（逐对象选灯）已跟踪前两项。

**评价**：这与模块"只声明被实现的开关"的纪律一致，不是 bug；但从三维体验看，硬边阴影、阴影只跟随第一盏灯、掠射面 acne/ detach 是用户首先察觉的差别。

**fix**：不急着实现；在指南里把它写进**能力矩阵**（每场景一个投影灯、无 PCF、bias 为深度单位），使宿主可以提前选择自己实现。若实现 alpha-to-coverage 时顺带做 PCF，应与 `T58` 同批决策。**doc + backlog（已存在 S6/S7）**。

---

### V9（透明度语义 / 中）：Mask 下 opacity 参与裁切阈值，却被排进不透明队列；设计页 2.7 与代码不一致

**证据**：`Material.cpp:23-31` 的 `alphaModeSortsTransparent()` 对 Mask 恒返回 false；`builtin_forward.frag:89` 与 `builtin_gbuffer.frag:62` 的阈值是 `vn_draw.params.x * vn_surface.alpha < cutoff`，即 drawable 的 opacity 改变镂空形状。`.ai/design/graphics-material.md:70-73` 却写 "`isTransparent()` 变成 opacity<1 or Blend"，与 §2.7 的代码语义相反；同页 §3.5/C4 又写 Mask 不进透明队列。

**后果**：宿主用一个统一的 `setOpacity()` 淡出场景时，Mask 物体不是变淡而是逐渐消失/破洞，且因为在不透明队列里无法混合出半透明效果。glTF 的 alphaMask 阈值来自材质 alpha（纹理×factor），物体 opacity 是另一个量。

**fix**：决定语义：(a) 阈值只用材质 alpha，opacity 仅用于 Opaque/Blend 的输出/排序（与 glTF 一致）；或 (b) 保留现行为但把 opacity 从 `alphaModeSortsTransparent` 的文档句子中移除，并在指南里警告"淡出 Mask 会侵蚀轮廓"。无论哪种，§2.7 的句子必须与代码同源。**code or doc + test**。

---

### V10（文档一致性 / 低）：排序键注释把深度方向说反了

**证据**：`DrawListBake.hpp:429` 写 "Quantized camera-space depth (near = larger)"，但 `DrawListBake.cpp:541-544` 计算 `depth = -(view*point).z`（近处 z≈−1 → depth≈1，远处 z≈−10 → depth≈10），`SelectTest.cpp:199` 断言 "camera-space depth grows away from the eye"，`operator<`（`DrawListBake.hpp:456-457`）据此让不透明从近到远、透明从远到近。实现与测试都对，注释反了。

**fix**：把注释改为 "depth grows away from the eye"；doc-only。**doc**。

## 4. 不建议照搬的对标项

| Peer 机制 | 本模块的判断 |
|---|---|
| three.js 的单头 JS API / 动态材质编译 | 本模块是跨 DLL 的 C++ ABI；法线矩阵应进 ABI，不能靠对象上的隐式字段 |
| Unity/Unreal 的世界单位固定为米 / 厘米 | 本模块服务 CAD 与机器人几何，单位交给上层更符合现有 `Placement<T>` 的中性设计；需要的是**声明与边界转换**，不是硬编码米 |
| glTF 的完整 IBL / 光强物理单位 | 本轮只建议声明差异；强行切单位会破坏现有亮度基线与 `graphics-material.md` 的迁移记录 |
| WebGPU 的完全显式 barrier / bind group | 已由 vk 后端按本项目 ADR 处理，与三维语义无关 |
| OSG 的多父 DAG / 节点级灯 | 已由 `ADR-2026-10-23` 与 backlog T7 明确否决/跟踪，不重开 |
| wgpu/Filament 的 bindless 阴影与多灯 | 本模块的 8 槽 + 首灯投影是容量决策（`S6/S7/T69`），不是语义错误 |

## 5. 建议顺序（供 owner 批准，不自动进入 backlog）

1. **先定规范**（`V1` + `V6`）：一份 ADR 声明手性/up/单位/空间流转，改正两处说反的深度注释。此后所有修复都有唯一引用点。
2. **再修高风险的三维数学**（`V2`、`V3`、`V4`）：法线矩阵进 ABI → 负行列式 front-face 策略 → 缺失法线回退。三者都涉及 ABI 或程序变体，最好同批规划，避免多次 ABI bump；每项配一条像素/数值测试。
3. **相机健壮性**（`V5`）：沿用现有"命名拒绝"风格，不引入新机制；与 `CameraSnapshot`/`AutoTransform` 的 up 语义一起改。
4. **语义澄清**（`V7`、`V8`、`V9`）：先写清单位与能力边界，再决定是否改数值；`V9` 可与材质指南的既有漂移修正一起做。
5. **单行修正**（`V10`）。

候选 backlog 条目（**未添加**）：法线矩阵与负行列式（可合并一条，trigger：首个非均匀缩放/镜像资产进入渲染）；无设法线回退（trigger：首个无 normal 通道的导入模型走默认 lit 程序）；相机退化/非法视锥拒绝（trigger：宿主观察到 NaN 画面或需要诊断）；光照单位与 glTF 换算（trigger：第一个 `KHR_lights_punctual` 导入需求）。

## 6. 复核命令

```bash
# 本轮证据的几个入口
grep -rn "mat3(vn_model) \* vn_Normal" src/viz/graphics/shaders
grep -rn "kFrontFace\|frontFace" src/plugins/gfx_backend_vk/include/vine/backend_vk/internal/StateMapping.hpp src/plugins/gfx_backend_vk/src/internal/ContentCaches.cpp
grep -rn "derives normals from the positions\|hasNormals" src/viz/graphics
grep -rn "1.0 / (vn_distance \* vn_distance + 1.0)" src/viz/graphics/shaders/vine_params.glsl
grep -rn "near = larger\|depth grows away" src/viz/graphics/src/DrawListBake.hpp tests/test_graphics/SelectTest.cpp
grep -rn "up_axis" src/loaders tests/test_meshio | head
```
