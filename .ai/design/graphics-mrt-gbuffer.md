# graphics MRT / GBuffer 设计（一次几何遍历产出多张图）

> 状态：设计稿 v1（2026-09-04）· **S1 ✅ · S2a ✅ · S2b ✅ · S3 ✅ · S4a/b/c ✅**（deferred 主窗已实现，但
> **2026-09-04 默认退回 FORWARD**：S3 曾把 deferred 设为默认，用户实测默认启动窗口空白（本环境无法
> 截图/像素级核验）→ 默认还原 forward，deferred 主窗改由 `VINE_VSG_DEFERRED_FULL` 开关驱动，待
> 真机像素目检定位后（疑光照/重建静默黑屏，日志无错）再决定切默认）。
> 上游：`graphics-render-pipeline.md`（多 pass + 命名产出槽）、`graphics-shader.md`（用户 GLSL /
> ShaderProgram）、`vsg-custom-shader.md`（自写 shading ABI）、`vsg-target-unification.md`
> （统一 Target / (camera, order) 内容槽）。
> 关联：`graphics-lighting.md`、`graphics-shadow.md`、`graphics-render-pipeline.md` §11(PassContext)。

## 1. 目标（用户 2026-09-04 定案）
- "一个 Pass 一次几何遍历同时产出多张图"（MRT / GBuffer）——不是"拷来拷去"的多次渲染。
- 端到端形态：**主场景走 deferred**——GBuffer 离屏 pass（MRT）+ deferred 光照 pass 出最终
  lit 画面到窗口；HUD/overlay/PiP 仍按 (camera, order) 叠在窗口之上。

## 2. 关键事实（已核实）
- 现状链式单输出：`RenderTarget` 单 color 附件 → pass 单 RT → engine 单名发布；`drawScreenTexture`
  只采样附件 0。
- vendored vsg phong/flat 是**序列化 blob、单 fragment 输出**（只写 location 0），无法改出第二输出；
  因此真实负载必须走自写多输出 shader（= custom-shader P0 与 MRT 交叠）。
- SDK 已有 `ShaderProgram`（GLSL → 运行期 glslang）+ `GraphicsPipelineConfigurator` 自描述装配。
- 缺口：
  1. `RenderTarget` 只支持 1 个 color 附件；
  2. 后端 `buildOffscreenTarget` 只建 1 color(+depth)，render pass/framebuffer 单 color；
  3. `SceneBridge` 程序路径只绑 `vsg_Vertex`（无 normal/材质喂入），管线 `ColorBlendState` 附件数=1；
  4. 采样端不能选附件；发布端无"多附件名"概念。

## 3. 关键裁决（用户选项有矛盾，此处定标准 deferred）
用户原选 "lit color + normal" 与 "deferred 光照 pass" 冲突（gbuffer 无 albedo 则无法重光照）。
裁决：**GBuffer = albedo(unlit 材质色) + view normal(+view pos 由深度重建亦可)**；lit 观感由
deferred 光照 pass 复刻当前 phong 产出（与现在画面一致 A/B）。需要 gbuffer 内再带 lit color 时
仅多一个 fragment 输出，随时可加。

## 4. 数据模型
### 4.1 RenderTarget（SDK）：N 个 color 附件
- `attachColor(format)` **追加**一个 color 附件（兼容：现有调用方最多 1 次 → 行为不变）；
- `colorCount()` / `colorFormat(int i)`（`colorFormat()` = i0，保持）；`hasColor()` = colorCount()>0；
- `readColorBuffer()` 仍读附件 0（+可选 `readColorBuffer(int i)` 以后扩展）。注：RenderTarget 的
  读回是占位（恒全零，见 graphics-design.md）；真实读回入口为 `RenderBackend::readColorBuffer(int)` /
  `readDepthBuffer()`（默认 unsupported，后端实现点）。
- 附件 0..N-1 各自独立可采样；渲染时 fragment `layout(location=i) out` 写附件 i。

### 4.2 命名产出（engine）：保持 1 RT = 1 name，纹理在 RT 内
- **不**为每个附件造独立 name（避免注册表双写/悬垂）。`outputs_[name] = RenderTarget*` 不变；
  一个 MRT RT（含 N 张纹理）以一个名字发布。
- 消费端按需选附件：~~`ScreenPass::setSourceAttachment(int)`~~（**2026-09-13 删除**：附件就是 program 的 sampler binding，见 `vsg-custom-shader.md` §11.11）；未来 LightPass 绑
  att0/1/2。发布语义 = "这个 RT 画完了"，附件是 RT 的属性 → 多输出能力的承载点是 **RenderTarget**。

### 4.3 RenderBackend（采样附件）
- `drawScreenTexture(RenderTarget*)`（附件 0，保留、委托）→ 新增
  `drawScreenTexture(RenderTarget*, int attachment)`（真实现）。mock/测试同步。

## 5. vsg 后端
### 5.1 Target 附件（Impl::Target）
- `color_image/color_view` 单值 → 每附件一份（vector）；depth 不变。
- `buildOffscreenTarget` 依 `target->colorCount()` 建 N 个 Image/ImageView
  （COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC），render pass 泛化：N 个 color
  AttachmentDescription（均 load=clear、store=store、finalLayout=SHADER_READ_ONLY_OPTIMAL）+ 可选 depth
  （保留现有 makeSampleableRenderPass 对单附件的行为，扩展为 N）。
- framebuffer = N color views (+ depth)；graph->clearValues = N 个 color clear + 可选 depth clear。
- 清屏色：附件 0 用 pass clearColor（如现有/默认灰），其余附件默认黑/透明（或 RenderTarget 后续
  加 per-attachment clear，暂不做）。

### 5.2 槽 ShaderSet 附件数（ColorBlendState）
- 单附件 target：现状（colorBlend 1）。
- N 附件 target：该 target 的 `depth_on/depth_off_shader_set` 的默认
  `ColorBlendState` 附件数 = N（Vulkan 要求 colorBlend.attachmentCount == subpass color 数；
  自写多输出程序管线继承之）。`buildShaderSet(..., color_count)` 新参。

### 5.3 SceneBridge 程序路径扩展
- `buildProgramShaderSet`：额外声明 `vsg_Normal`(loc1)/`vsg_Color`(loc2) attributeBindings +
  push "pc"（不动）；程序路径也 assign normal/color 数组（默认路径同款），并可选绑
  "material" PhongMaterialValue 描述符（gbuffer shader 读 albedo/spec/…），使程序能吃到材质。
  （既有程序若不用 normal/material，多绑 unused 无害。）
- 多输出程序 → fragment 写 location 0..N-1，与 MRT subpass 的 N 附件对齐。

## 6. GBuffer 几何 pass（Slice1 验证形态）
- 离屏 MRT RT（RGBA8 albedo + RGBA16F normal + RGBA16F viewpos，可选 depth D24）。
- 引擎注册一个 order<0 pass：camera=master，RT=该 MRT；`setProgramOverride` 或逐几何 program =
  自写 gbuffer ShaderProgram（不光照：albedo←材质 diffuse(+ambient 常量可省)、normal、viewpos）。
  VS 读 vsg_Vertex/vsg_Normal + pc；FS 输出 3 个 location。
- publish 该 RT 一个名字（如 "GBuffer"）；两个 ScreenPass/ScreenPass(附件 i) 在窗口做 PiP
  分别预览 att0/att1/att2 → GPU 上眼见为实（llvmpipe + validation）。
- 与现 forward 观感对照 A/B：gbuffer att0(albedo) 是未光照，不是最终画面（预期）。

## 7. deferred 光照 pass（Slice2，主场景迁移）
- 新 pass 类型或 ScreenPass+Program：绑 GBuffer 的 att0/1/2（三张纹理）为输入，全屏四边形，
  自写 FS 复刻当前 phong（headlight/ambient + 方向光 N·L + spec），输出到窗口（backbuffer）。
- 主窗口拓扑 = 光照 pass（order 0，取代现在的主场景内容槽）+ HUD(order 10)/PiP 照旧按序叠。
- 其它离屏/PiP 单附件路径零回归（仍走 phong 默认）。

## 8. 分 slice 落地（每 slice 构建+测试+llvmpipe 冒烟，逐 slice 提交）
- **S1 ✅（2026-09-04 已落地）**：RenderTarget 多附件（`attachColor` 追加、`colorCount/colorFormat(i)`）
  + 采样指定附件（ScreenPass `setSourceAttachment`、`RenderBackend::drawScreenTexture(rt, att)`
  1 参转发）+
  后端 N-attachment FBO/render pass（`makeSampleableRenderPass` 泛化为 N color、clear 每附件、
  `Target.color_images/views` 向量、PiP 槽按 (source, attachment) 键）+ 槽 ShaderSet
  `color_count`（ColorBlendState=N）+ SceneBridge 程序路径（vsg_Normal/vsg_Color 数组与
  PhongMaterialValue 描述符喂入，`buildProgramShaderSet` 声明 set0/b0 material +
  normal/color 属性）+ 自写 3 输出 gbuffer shader（att0=albedo/unlit、att1=view normal、
  att2=view pos，RGBA8/16F/16F+D24）。
  验证：`VINE_VSG_GBUFFER=1` 640x360 MRT 目标 + 3 个 PiP 各采样 att0/1/2，llvmpipe 无错误；
  test_graphics 116（+2：RenderTarget 多附件契约、ScreenPass 选 MRT 附件）/ test_vsg 10；
  五种冒烟全干净；既有自定义 program 盒（magenta/points）不受影响。
- **S2 → 拆 S2a / S2b（deferred 光照，2026-09-04 开始）**。光照 pass = **全屏自定义着色 pass**，
  不是参数化小改，机制决策已核实：
  * **每帧参数用 push constant 注入**（非动态 uniform 缓冲）：`vsg::PushConstants(stageFlags, offset, Data*)`
    录制时拷贝 `data->dataPointer()` 当前字节 → 每帧改写一个保留的 Data 再录制即可；管线 layout 需在
    ShaderSet 声明对应 pushConstantRange（FRAGMENT, 0, size ≤128）。
  * **gbuffer 存 view-space normal/pos**（S1 已是）→ 光方向必须在 view 空间。CPU 每帧用主相机
    LookAt 的旋转（right/up/fwd）把场景光方向转到 view 空间，压进 push 块；shader 不再需要 view 矩阵。
    块布局（≤128B）：ambient(vec4 rgb+a) + 至多 2 个方向光(dir_view vec4 + color·intensity vec4) + misc。
  * **全屏节点**：复用 makeScreenTextureNode 的全屏三角形（gl_VertexIndex，无相机）模式，但 FS 换成
    光照 GLSL，`assignTexture` 绑 gbuffer 的 att0/1/2 三个 ImageView；StateGroup 内
    Commands{ PushConstants(data), Draw }；与 PiP 类似以 (source) 为键保留并每帧更新 viewport/data。
  * **缺口（S2a 承认并注记）**：gbuffer 无 per-pixel spec/shininess → S2a 光照 shader 用固定
    shininess + 统一 spec 强度近似，与 forward 的 spec 会有差异；S3 再在 gbuffer 加 spec/粗糙度。
  * SDK 面：给 ScreenPass 加"可选程序 + 多源附件"能力（pass 级 Program + 命名槽，对齐
    graphics-shader.md §5），还是新增专用 LightPass，实现时定（倾向通用化 ScreenPass）。
  * **S2a（旁路 A/B，不动默认主窗）**：gbuffer 离屏 pass + deferred 光照全屏 pass 渲进一个
    "deferred-lit 预览 PiP"，与窗口主 forward 场景并排对照（同一场景/相机/灯光观感 A/B）；
    env 门控。llvmpipe 验证无校验错 + 明暗可见。
  * **S2b（主窗迁移）**：默认管线改为 gbuffer(order<0) + 光照全屏(order 0 取代窗口场景槽)
    + HUD(10)/PiP(100+) 照旧；A/B 一致后切默认。

- **S2a ✅（2026-09-04 已落地，env `VINE_VSG_DEFERRED`）**。实现为**通用化 ScreenPass**（可选
  fragment 程序 + 采样源全部附件）：ScreenPass::setProgram(intrusive_ptr<ShaderProgram>)，
  execute 里程序路径先 `setLights(内容场景光)` 再 `backend->drawScreenProgram(source, program,
  camera)`。RenderBackend 新增 `drawScreenProgram`（默认 no-op；mock 记录）。vsg 实现：
  `Target.program_slots[source]` 保留全屏程序槽（View+node+push Data）；`makeFullscreenProgramNode`
  编译后端全屏 VS(gl_VertexIndex) + 用户 FS，绑源 MRT 每附件为采样纹理（binding 0..N-1），声明
  FRAGMENT push 范围 128；StateGroup 内 `Commands{ PushConstants(FRAGMENT,0,Data), Draw(3) }` 录制时
  拷贝 Data 当前字节。每帧 `fillLightPushBlock(camera, lights, LightPushBlock)`：CPU 用相机 LookAt
  基（r,u,f）把方向光转到 view 空间（world→view 旋转，行 r/u/-f），ambient+至多 2 方向光 +
  固定 shininess 32 写进 128B push Data（LightPushBlock 96B: ambient/dir0/col0/dir1/col1/misc）。
  demo：gbuffer 几何 pass(order -3, 3 输出) + 光照 ScreenPass(130, 采样 GBuffer 全部附件, 340x191
  预览)。验证：llvmpipe "deferred fullscreen program ... attached" 多帧零错误；test_graphics 117
  (+1 ScreenPassProgramSamplesMrtForDeferredLighting)/ test_vsg 10；6 种冒烟（默认/offscreen/
  multislot/slot/gbuffer/deferred）全干净；默认路径零改动。
- **S2b（未开始）**：主窗迁移（gbuffer + 光照全屏 order 0 取代窗口场景槽）。已知待改：window 主槽
  语义换成光照结果、RenderControl::init 默认窗口 pass 在 deferred 模式不再注册（或改内容）、
  现有 forward 默认保留（另一 env 分支）。
- **S2b ✅（2026-09-04 已落地，env `VINE_VSG_DEFERRED_FULL`）**。主窗迁移到 deferred：
  * 后端排序：`setupContentSlot` 的子 View 序扫描把 **fullscreen-program 视图当作 INT_MIN**（先画），
    普通非内容槽（PiP）仍 INT_MAX；`drawScreenProgram` 建槽时**头插**进窗口 children → 全屏光照
    结果作为窗口"底层"，HUD 内容槽(10+) 叠在其上。默认模式无 program_slots → 行为零变化。
  * demo（addDeferredMain）：gbuffer 几何 pass(order -3, engine 场景) + 全窗口光照 ScreenPass
    (order 0, camera=master, RT=null, 采样 GBuffer 全部附件)。光照 pass camera=master+RT null →
    `RenderEngine::hasWindowPass()` 为真 → RenderControl::init **不再注册 forward 主窗 pass**，
    窗口主画面即 deferred lit。光照全屏 0,0 全表面。
  * 验证：llvmpipe "deferred fullscreen program 640x360 -> 0,0 378x234 attached" 零错误；test_graphics
    117 / test_vsg 10；7 种冒烟（默认/offscreen/multislot/slot/gbuffer/deferred/deferred_full）全干净，
    默认路径零回归。AppShell 抽出 makeGbufferGeometryProgram/makeGbufferTarget/makeDeferredLightProgram
    helper（gbuffer 与两个 deferred demo 共用）。
- S3（可选）：gbuffer 附带 spec/粗糙度（消除 S2a 固定 shininess 近似，A/B 更贴近 forward）/
  per-attachment clear / 深度重建 pos / deferred_full 切默认（去掉 env 门控，forward 仅留作对照）。
- **S3 ✅（2026-09-04 已落地）**：
  * **gbuffer 带 per-pixel spec/shininess**（不新增附件，打包进现有 alpha）：albedo 附件
    alpha = specular 强度（max(specular.rgb) clamp 0..1，RGBA8）；normal 附件 alpha =
    shininess/256（RGBA16F）。deferred 光照 FS 读回（spec 强度乘高光项，shininess=normal.a*256），
    消除固定 shininess 32 近似。AppShell 收敛为单源 helper（makeGbufferGeometryProgram /
    makeGbufferTarget / makeDeferredLightProgram），addDeferredDemo（S2a）改走 helper。
  * **deferred 成为默认主窗（2026-09-04 曾落地，随后回退）**：addDeferredMain 曾去掉 env 门控默认执行；
    用户实测默认启动空白 → 已还原为默认 FORWARD，deferred 主窗由 `VINE_VSG_DEFERRED_FULL` 门控
    （forward 永远可用）。待像素目检定位 deferred 黑屏后重审默认化。
  * 验证：default 冒烟日志 "deferred fullscreen program 640x360 -> 0,0 378x234 attached"；
    FORWARD 冒烟无 deferred；test_graphics 117 / test_vsg 10；default+FORWARD+五 demo 冒烟全干净。
- S4（未做）：per-attachment clear 色、多光>2。（对账 2026-09-25：多光在新后端**已不设限**——见下方 S4 剩余条的注；
  per-attachment clear 色仍不成立。）
- **S4a ✅（2026-09-04）**：specular **RGB**（第 4 附件 RGBA8，helper target/gbuffer 几何/光照 FS
  同步：geometry loc3=out_specular=clamp(specular.rgb)；光照 FS binding3=spec_tex，高光项乘 spec_col；
  att0 alpha 回 1）。消除 S3 的标量 spec 强度近似，高光色随材质。default deferred 冒烟 0 错误，
  117/10 测试绿，FORWARD/gbuffer/deferred 冒烟干净。
- **S4b ✅（2026-09-04）**：**深度重建 view pos（去掉 viewpos 附件）**：
  * `makeSampleableRenderPass` 的 depth 附件改为 storeOp=STORE + finalLayout=SHADER_READ_ONLY_OPTIMAL，
    子 pass 外部依赖覆盖 color+depth 的写与 fragment 读（depth 可被后续 pass 采样重建）。
  * color+RT target 的 depth 清屏改为**远平面 1.0**（背景像素 depth≈1 可在光照 pass 识别为空）。
  * 全屏程序节点可额外采样源 depth（binding = colorCount，NEAREST 采样）；drawScreenProgram 传入
    src.depth_view。
  * push 块 misc → projparms{near, far, proj00, proj11}（仍 96B）；fillLightPushBlock 从透视相机
    fov/aspect/near/far 填入。
  * helper gbuffer target 3 色（albedo RGBA8 / normal RGBA16F(alpha=shininess/256) / spec RGBA8）
    + depth；几何 FS 去 viewpos 输出；光照 FS binding albedo0/normal1/spec2/depth3，由
    depth→NDC→(A,B,t) 重建 view pos。
- **S4c ✅（2026-09-04）**：**方向光 2 → 3**（push 块仍 128B，无范围/移植风险）：LightPushBlock
  改为 `ambient + projparms + dirs[3][4] + cols[3][4]`（恰 128B）；fillLightPushBlock 上限 3；
  光照 FS push 块改 `sun_dir[3]/sun_color[3]`、循环 0..2。demo 加 env `VINE_VSG_EXTRA_SUNS`（再添
  2 个方向光，共 3 个方向光）验证。default(3 光) 与 FORWARD(3 光) 冒烟 0 错误；117/10 绿。
- S4 剩余（未做）：per-attachment clear 色；>3 方向光（需 >128B push，改 UBO 或 range 扩容）。
  * 对账（2026-09-25）：**>3 方向光在新后端已不设限**——光源走 lights 块（1024 槽，`BlockStorage::writeLights`）
    而非 128B push（这正是当初说的“range 扩容”）。**per-attachment clear 色仍不成立**——`core/ClearPlan` 的
    Rule 4：一条清屏请求清**所有**色附件，且只有 attachment 0 收到 pass 的颜色。
- S3（可选）：gbuffer 附带 spec/粗糙度（或 lit color 附件）/ per-attachment clear / 深度重建 pos。

## 9. 回归与验证
- GraphicsTest（MockBackend，后端无关）：多附件 RenderTarget 契约、ScreenPass 附件选择、
  命名产出 1 RT=N 纹理语义。test_vsg 不变（headless，不 initialize）。
- llvmpipe 冒烟：VINE_VSG_GBUFFER=1 离屏 gbuffer + PiP(附件0/1/2)；S2a 后 VINE_VSG_DEFERRED=1
  旁路 deferred-lit 预览 A/B；S2b 后默认主窗即 deferred。
