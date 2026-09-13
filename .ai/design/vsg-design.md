# vsg 渲染后端设计（gfx_backend_vsg 插件）

> 状态：设计稿 v5（2026-09-02）
>
> **历史文档（2026-09-13 注）**：本文描述的是本后端**最初**的实现——着色走 vsg 内建 phong set、
> 材质映射成 `vsg::PhongMaterialValue`、灯挂成 vsg 灯节点。这三样在 2026-09-13 的收尾里全部退出代码
> （见 `vsg-custom-shader.md` §11.12：材质是 `VineMaterialBlock`、灯只有 `vine_lights` 块、属性只按
> `vine_*` 名绑定）。读“今天怎么做”请看 `vsg-custom-shader.md` 与 `src/plugins/gfx_backend_vsg/docs/`；
> 本文保留为“当时为什么这么做”的记录。
>
> **定位**：`gfx_backend_vsg` 是 `graphics` 模块的一个**渲染后端实现**，以
> appfw **插件**形式存在。它实现 `vine::graphics::RenderBackend` 抽象接口，
> 把 `graphics` 层的场景图、相机和渲染命令翻译成 VulkanSceneGraph（VSG）调用。
> 后续会新增纯手撸（如手写 Vulkan/OpenGL）后端，均实现同一 `RenderBackend`。

## 1. 架构定位

```
┌────────────────────────────────────────────┐
│ graphics（基础与抽象层）                      │
│  RenderBackend  ← 抽象接口                   │
│  RenderBackendRegistry  ← 后端自注册         │
│  RenderEngine / RenderPass  ← 驱动           │
└───────────────────┬────────────────────────┘
                    │ 实现（运行时插件加载）
┌───────────────────▼────────────────────────┐
│ gfx_backend_vsg（实现 1，appfw 插件）        │
│  VsgRenderer : RenderBackend               │
│  ├─ SceneBridge   (graphics::Scene → vsg)  │
│  ├─ CameraBridge  (graphics::Camera → vsg) │
│  ├─ VsgMaterialManager (Material → Phong)  │
│  └─ 内部：window/viewer/pipeline            │
└────────────────────────────────────────────┘
```

原则：
- 后端实现全部下沉到插件，`graphics` 只暴露抽象接口（`RenderBackend`、
  `RenderBackendRegistry`、`MaterialManager`），**不依赖任何后端库**
- 插件加载时 `GfxBackendVsgPlugin::load()` 自注册 `"vsg"` 工厂到
  `RenderBackendRegistry`；上层按名字创建后端，不感知具体类
- 上层（RenderEngine/RenderPass）不感知具体后端

## 2. 模块结构

```
src/plugins/gfx_backend_vsg/        # appfw 插件（MODULE DLL，v_add_plugin）
  CMakeLists.txt                    # v_add_plugin + FetchContent 拉取 VSG
  include/vine/vsg/                 # 对外头文件（用户要求直接放 include/ 下）
    vsg_global.hpp                  # API export 宏 + 命名空间
    VsgRenderer.hpp                 # 实现 RenderBackend 的后端
    SceneBridge.hpp                 # 场景桥接（Scene/Node/Geometry → vsg::Node）
    CameraBridge.hpp                # 相机桥接（Camera → vsg::Camera）
    VsgMaterialManager.hpp          # 材质管理器实现
    VsgRenderBackendFactory.hpp     # 后端工厂（自注册用）
  src/
    VsgRenderer.cpp
    SceneBridge.cpp
    CameraBridge.cpp
    VsgMaterialManager.cpp
    VsgRenderBackendFactory.cpp
    GfxBackendVsgPlugin.hpp/.cpp    # 插件外壳（v_add_plugin glob src/ 一并编译）
    VsgUtils.hpp                    # 矩阵转换等工具
```

注：
- `Shaders.hpp`（手工 SPIR-V）已删除——改用 VSG 内置
  `createPhongShaderSet()`，不再手工拼装 shader。
- 旧 `src/viz/vsg/` 目录已删除；`src/viz/CMakeLists.txt` 现在只含
  `add_subdirectory(graphics)`。
- 头文件从 `sdk/vine/vsg/` 迁移到 `include/vine/vsg/`（插件私有，无需
  进公共 SDK 树）。

## 3. 核心类设计（v5 新增插件化）

### 3.0 插件化与自注册

`gfx_backend_vsg` 作为 appfw MODULE 插件，随 `PluginManager::loadAll()`
在 app 启动时被扫描加载（部署到 `<exe>/plugins/vine/gfx_backend_vsgd.dll`）。

```cpp
// GfxBackendVsgPlugin.cpp 末尾
V_DECLARE_PLUGIN(vine::vsg::GfxBackendVsgPlugin,
                 u8"gfx_backend_vsg", u8"VSG 渲染后端",
                 u8"1.0.0", u8"Vine", u8"", {})
```

- `load()` 调用 `RenderBackendRegistry::instance().registerFactory(&s_factory)`
  （`s_factory` 为 `static VsgRenderBackendFactory`）
- `RenderBackendRegistry::Registrar<T>` 提供 RAII 静态自注册辅助；插件场景
  用 `load()` 显式注册（保证插件卸载时机可控、避免跨 TU 静态初始化序问题）
- 上层通过 `RenderBackendRegistry::instance().create(u8"vsg", scene, camera)`
  按名字拿到后端实例，**不依赖后端模块或 VSG 库**（测试只链接
  `vi::Appfw vi::Graphics`）

### 3.1 `VsgRenderer`（实现 `RenderBackend`）

```cpp
class V_VSG_API VsgRenderer : public vine::graphics::RenderBackend {
  public:
    VsgRenderer(vine::graphics::Scene* scene, vine::graphics::Camera* camera);
    ~VsgRenderer() override;

    // RenderBackend 接口
    bool initialize() override;
    void shutdown() override;
    void beginFrame() override;
    void endFrame() override;
    void executePass(const vine::graphics::RenderPass* pass,
                     const std::vector<vine::graphics::RenderCommand>& commands) override;
    void setRenderTarget(vine::graphics::RenderTarget* target) override;
    void render(const std::vector<vine::graphics::RenderCommand>& commands,
                const vine::graphics::Camera* camera) override;
    void clear(const vine::Color& backgroundColor, bool clearDepth) override;
    void swapBuffers() override;

    // vsg 特有便捷接口
    ::vsg::ref_ptr<::vsg::Viewer> viewer() const;
    ::vsg::ref_ptr<::vsg::Camera> vsgCamera() const;
    ::vsg::ref_ptr<::vsg::Node> vsgScene() const;

  private:
    struct Data;
    Data* const d;
};
```

`RenderBackend` 接口到 VSG 的映射：

| RenderBackend 方法 | VSG 实现 |
|---|---|
| `setWindowContext(ctx)` | 记录宿主窗口上下文（v6 新增） |
| `initialize()` | 有宿主窗口上下文时，`WindowTraits::nativeWindow = HWND`（`WindowContext::nativeHandle()`），**嵌入已有原生表面**而非自建窗口；否则自建 `WindowTraits`/`Window`。之后建 `Viewer`；用 `createPhongShaderSet()` 注入 `SceneBridge`；构建场景图/相机；`createRenderGraphForView(..., assignHeadlight=true)` 加头灯；编译 pipeline |
| `resize(w, h)` | `window->resize()` 重建 swapchain（v6 新增，随宿主 resize 信号触发） |
| `shutdown()` | `deviceWaitIdle()` + `close()` |
| `beginFrame()` | `viewer->advanceToNextFrame()` + `handleEvents()` |
| `endFrame()` | `viewer->update()` |
| `render(commands, camera)` | 同步相机矩阵；`recordAndSubmit()` + `present()` |
| `clear(color, depth)` | 记录清除颜色/深度（当前 RenderGraph 清除为固定值，见已知问题） |
| `setRenderTarget(target)` | 当前仅支持窗口默认帧缓冲（offscreen 暂未实现） |
| `swapBuffers()` | 无操作（`present()` 已在 `render()` 内完成） |
| `executePass(pass, commands)` | 设置 clear 后调用 `render()` |

帧顺序：`frame()` = `beginFrame()`（advance+handleEvents）→ `endFrame()`（update）
→ `render()`（recordAndSubmit+present），与 VSG 官方生命周期一致。

### 3.2 `SceneBridge`（场景桥接）

把 `graphics` 场景树翻译成 vsg 场景树（保留模式）。

- `build(Scene*)` → `vsg::Group` 根节点
- `buildNode(Node*)` → `vsg::MatrixTransform`（局部变换）+ 子节点 + 可绘制对象
- `buildGeometry(Geometry*)` → **每个几何独立**的 `vsg::StateGroup`
  （含 pipeline + 材质 descriptor + `VertexIndexDraw` 子节点）
- `setShaderSet(ShaderSet*)` → 注入渲染后端使用的 shader set（未注入时回退到 phong）
- `setMaterialManager(VsgMaterialManager*)` → 注入材质管理器（未注入时用内部默认）

**材质贯通（v3 新增，v4 引入 MaterialManager）**：`buildGeometry` 为每个几何
创建一个 `vsg::GraphicsPipelineConfigurator`，通过材质管理器把
`graphics::Material` 映射为 `vsg::PhongMaterialValue` 并
`assignDescriptor("material", mat)` 绑定到 StateGroup：

| `graphics::Material` | `PhongMaterialValue` 字段 |
|---|---|
| `diffuse()` | `diffuse`（RGBA） |
| `specular()` | `specular` |
| `ambient()` | `ambient` |
| `shininess()` | `shininess` |
| `opacity()` | 计入“有效透明度”（见 §4/§9）；blending 常开，透明度由逐顶点 alpha 承载，`diffuse.a` 固定 1 |

要点：
- `vsg_Color` 顶点数组为白色，其 **alpha 每帧承载“有效透明度”**（Scene×Node×
  Drawable×Material 的乘积）——Phong fragment 把顶点色与材质 diffuse 相乘，透明度
  走顶点 alpha、材质 `diffuse.a` 固定 1，避免多几何共享同一材质时透明度互相串。
- 无材质时使用默认灰色 Phong 材质（diffuse 0.8，shininess 32）。
- `VertexIndexDraw` 作为 StateGroup 的直接子节点（不是 `vsg::Geometry` 包装），
  避免 Geometry 空数组导致编译跳过。

### 3.2b `VsgMaterialManager`（材质管理器，v4 新增）

`graphics::MaterialManager` 是抽象基类（生命周期接口，不暴露后端类型）；
`vsg::VsgMaterialManager` 实现它，负责**属性 → 渲染资源**的转换与缓存：

- `getOrCreate(Material*)` → 返回缓存的 `vsg::PhongMaterialValue`
  （key = `Material*`；同一材质多几何共享同一资源）
- `updateMaterial(Material*)` → 属性变化时重建缓存资源
- `releaseMaterial(Material*)` / `clear()` → 释放单个/全部缓存
- `VsgRenderer::initialize()` 创建 manager 并注入 `SceneBridge`

**为什么放后端**：`PhongMaterialValue` 是 vsg 专属类型，归后端管理；graphics
只定义抽象接口，不泄漏后端类型（符合"不暴露三方库"原则）。未来 PBR 材质可
在 manager 内映射到 `PbrMaterialValue`，graphics 接口不变。

**透明排序**：`RenderCommand` 携带收集阶段解析好的**有效透明度** `opacity`
（= Scene×Node×Drawable×Material）与 `isTransparent`；graphics 端 painter 排序，后端只按
命令顺序提交。可见性同理：命令流只含 Scene/Node/Drawable **三级均可见**的对象。

**嵌入宿主窗口（v6）**：`RenderEngine::initialize()` 把 `WindowContext` 转发给后端
（`RenderBackend::setWindowContext`）。`VsgRenderer` 用 `nativeHandle()` 的 HWND 设
`WindowTraits::nativeWindow`，`vsg::Window::create` 包装该 HWND（参照 vsgQt
`initializeWindow()`）。**必须在窗口 expose（有真实尺寸）后初始化**——`RenderControl`
监听 `WindowContext::exposed` 信号延迟初始化；resize 时 `RenderEngine` 转发
`resized` → 后端 `resize()` 重建 swapchain。

### 3.3 `CameraBridge`（相机桥接）

把 `graphics::Camera` 的视图/投影矩阵翻译成 vsg 相机。

- `create(Camera*)` → `vsg::Camera`（`LookAt` + `Perspective`/`Orthographic`）
- `apply(Camera*, vsgCamera)` → 原位同步（相机变化时调用）

相机不含 `viewportState`（桥接层无窗口概念），由 `VsgRenderer::initialize()`
在拿到窗口后设置。

## 4. 命令流 vs 场景图（关键设计决策）

`graphics` 的 `RenderBackend::render()` 接收**立即模式命令流**
（`std::vector<RenderCommand>`），而 VSG 是**保留模式场景图**。二者通过
`SceneBridge` 桥接：`RenderEngine` 每帧 `Scene::collectRenderCommands(camera)`
（含视锥剔除 + 不透明/透明排序，命令里已带世界矩阵与有效透明度）→
`VsgRenderer::render()` 把命令流**保留式地 reconcile 到同一个 vsg 根节点** →
recordAndSubmit。

**关键点（2026-09 落地，取代早期“每帧重建 vsg 场景”思路）**：
- vsg 场景**不镜像 vine 的 Node 层级**——每个 drawable 对应根 `vsg::Group` 下
  一个保留 `MatrixTransform`（世界矩阵已烤好），拍平结构。
- `SceneBridge` 按 `Geometry*` 缓存 Item（`unordered_map`）；每帧只做廉价操作：
  写矩阵（变了才写）、写逐顶点 alpha（有效透明度变了才写）、必要时重建/新几何。
- **结构变化**（几何首次出现 / shape / 换材质对象 / 真移除）才重建 + `viewer->compile()`
  （目前全量；增量 compile 见 §9 Phase3）。
- 隐藏/剔除导致的**缺席不立即删**：Item 保留已编译节点、只从渲染根摘下，
  `absent_frames` 超过阈值（600）才 evict。
- 死代码 `VsgRenderer::update()` 已删除（不再按 scene 整体 rebuild）。

## 5. 已解决问题记录

1. **渲染 POC bug（已修复）**：`VertexIndexDraw` 被 `vsg::Geometry` 包装且
   数组为空 → `Geometry::compile()` 提前返回 → 顶点缓冲未分配。
   → 修复：`VertexIndexDraw` 直接作为 StateGroup 子节点。
2. **手工 ShaderSet 不兼容（已修复）**：改用 `createPhongShaderSet()` 内置
   shader set，不再手工拼装。
3. **导出宏（已修复）**：`v_add_plugin(GFX_BACKEND_VSG_TARGET)` 生成
   `V_GFX_BACKEND_VSG_LIB`，但头文件检查 `V_VSG_LIB` → 显式
   `target_compile_definitions(... PRIVATE V_VSG_LIB)`。
4. **相机无 viewportState（已修复）**：`VsgRenderer::initialize()` 从窗口
   `extent2D()` 创建并设置。
5. **帧顺序（已修复）**：advance → handleEvents → update → recordAndSubmit → present。
6. **V_DECLARE_PLUGIN C2059（v5 已修复）**：`GfxBackendVsgPlugin.cpp` 缺失
   `#include <vine/appfw/plugin_export.hpp>` 导致宏未定义 → 补上 include。
7. **测试单例冲突（v5 已修复）**：`test_vsg` 每个测试各自
   `createApplication()`，第二个测试再建 `QCoreApplication`（Qt 全局单例）
   挂起 → 改为套件级 fixture，整个测试套件只 boot 一次 Application。
8. **后端 initialize() 不适合无头测试（v5 处理）**：`registry.create("vsg")`
   构造 VsgRenderer 本身不碰 Vulkan 窗口；`initialize()` 会创建真实窗口/设备，
   会阻塞无头测试 → 测试只验证"按名创建成功"，初始化交给真实 app。

## 6. 依赖关系

```
gfx_backend_vsg（MODULE 插件，v_add_plugin）
  ├── vi::Appfw               (插件外壳：Plugin 基类 + V_DECLARE_PLUGIN)
  ├── vi::Graphics            (RenderBackend 抽象 + RenderBackendRegistry + 场景数据)
  ├── vsg::vsg                (VulkanSceneGraph，FetchContent 自包含)
  └── Vulkan SDK              (系统安装，VULKAN_SDK 环境变量)
```

运行时依赖：`<exe>/plugins/vine/gfx_backend_vsgd.dll`（由 v_add_plugin 部署）。

## 7. 测试

- `tests/test_vsg/GfxBackendVsgPluginTest.cpp`（**通过 app 启动的集成测试**，
  不再链接插件实现源码；只链接 `vi::Appfw vi::Graphics`）：
  - `PluginRegistersVsgBackend` — `createApplication(config, argc, argv)` +
    `pluginManager()->loadAll()`（镜像 app 的 main），断言
    `RenderBackendRegistry::has(u8"vsg")` 为真，证明插件被加载并自注册
  - `CreateBackendByName` — `registry.create(u8"vsg", scene, camera)` 返回
    非空 `RenderBackend`（通过插件路径拿到 VsgRenderer 实例）
  - 测试套件级 fixture 只 boot 一次 Application（QCoreApplication 单例约束）
  - 历史单测（SceneBridge 材质贯通 / VsgMaterialManager 缓存 / CameraBridge）
    已随旧 `src/viz/vsg` 移除；若需回归可重建为链接实现源码的单测

## 8. 后续实现计划

1. ~~修复渲染 POC bug~~ ✅
2. ~~`VsgRenderer` 实现 `RenderBackend` 接口~~ ✅
3. ~~材质贯通：`graphics::Material` → `PhongMaterialValue`（diffuse/specular/ambient/shininess/opacity + 透明混合）~~ ✅
4. ~~材质管理器：`graphics::MaterialManager` 抽象基类 + `VsgMaterialManager` 转换与缓存~~ ✅
5. ~~后端自注册 + appfw 插件化：`src/viz/vsg` → `src/plugins/gfx_backend_vsg`，app 启动加载~~ ✅
6. ~~渲染命令流真正驱动渲染（保留式 reconcile，见 §4/§9）~~ ✅
7. 离屏渲染（`RenderTarget` → vsg offscreen）
8. 纯手撸后端（独立插件，实现同一 `RenderBackend`，注册到 Registry）

## 9. 命令驱动的保留式渲染（2026-09 落地记录）

### 更新语义

| vine 侧变化 | vsg 动作 | compile |
|---|---|---|
| 节点矩阵 / 相机 | 原位写 Item 矩阵（变了才写） | 无 |
| 同材质改颜色/光泽 | 覆写共享 Phong uniform（每帧） | 无 |
| 任一级透明度（Scene/Node/Drawable/Material） | 覆写逐顶点 alpha（变了才写） | 无 |
| 可见性切换（隐藏/显示） | 从渲染根摘下/重挂，Item 保留 | 无 |
| drawable/子树增删、换材质对象、shape 变化 | 新建/重建 Item + 重挂 | 有（全量） |

### 3 级可见性 / 透明度（graphics 层）
- 可见性：`Scene::isVisible ∧ Node::isVisible ∧ Drawable::isVisible`，AND 语义；
  隐藏的对象不进 `RenderCommand`。
- 透明度：有效 `opacity = Scene.opacity × Node.opacity（沿树）× Drawable.opacity ×
  Material.opacity`，收集时逐 drawable 算出，写入 `RenderCommand.opacity`；
  `isTransparent = opacity < 1` 驱动 painter 排序。
- `Drawable/Node/Scene` 均新增 `opacity()/setOpacity()`；`Drawable/Scene` 新增
  `isVisible()/setVisible()`。

### 取舍与待办
- **blending 常开 + 逐顶点 alpha**：透明度任意跨越 1.0 都实时、零 rebuild，代价是
  opaque 物体也走混合（GPU 开销很小）。
- **evict 阈值 `kAbsentEvictFrames=600`**：区分“临时缺席（隐藏/剔除）”与“真移除”；
  可调。
- **Phase 2（未做，待明确负载）**：vine 端脏场景图——`Node` 缓存 world 矩阵/世界
  AABB、沿父链失效；后端不需要（只吃命令流，结构判据=指针比较）。
- **Phase 3（未做）**：结构变化只对新增子树做增量 `viewer->compile()`，或合并到帧边界。

