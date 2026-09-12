# Graphics 模块设计（现代化架构）

> ⚠ **本文为早期总体设计稿（v3）**，其 `Node/Drawable/Geometry(持 Shape)`/addDrawable/
> RenderCommand.drawable 等描述**已被场景图重构取代**（2026-09-03）：`Node` 拆基类、
> `Drawable` 已删除（职责并入叶子 `Geometry`）、变换归 `MatrixTransform`、Geometry 纯数据化
> (buffers)。**场景图/状态/着色请以 `graphics-scene-graph.md` / `graphics-state.md` /
> `graphics-shader.md` 及各自顶部落地记录为准**；本文仅保留相机/管道/后端抽象等仍有效部分。
> 待后续按需重写或并入三稿。
>
> 状态：已对齐代码（2026-09-02），设计稿 v3（场景图部分已过时，见上）
>
> **目标**：为 Vine 提供场景图管理和渲染抽象层，支持多相机、多渲染通道、交互操纵、拾取。
> 核心功能：场景树管理、可视对象、相机操纵、射线求交、渲染管道、后端抽象。

## 1. 背景与目标

`Graphics` 模块是 Vine 可视化系统的中间层，负责：
1. **场景管理**：以树形结构（`Scene`/`Node`）组织可渲染对象
2. **相机系统**：支持多相机、视图矩阵、投影矩阵
3. **相机操纵**：轨道、摇移、缩放、第一人称等交互方式
4. **拾取系统**：射线-几何体求交
5. **渲染管道**：多 Pass、多 RenderTarget 支持
6. **渲染抽象**：定义后端接口，支持 OpenGL/Vulkan 等

不负责：
- 实际渲染实现（交给后端 RenderBackend）
- UI 布局（属于 AppFramework）
- 物理模拟（属于其他模块）

## 2. 模块结构

```
src/viz/graphics/
  CMakeLists.txt
  sdk/vine/graphics/
    graphics_global.hpp          # API export 宏 + 命名空间

    # 场景与几何体（边界框类型使用 vine::math::Aabbd，即 Rect3<double>，
    #            由 vine/math/Rect3.hpp 提供，BoundingBox 已移除）
    Node.hpp                     # 场景图节点（变换 + 子节点 + 可绘制对象）
    Drawable.hpp                 # 可绘制对象基类（几何 + 材质）
    Geometry.hpp                 # 几何体（包装 vine::geometry::Shape）
    Material.hpp                 # 材质定义（纯属性）
    MaterialManager.hpp          # 材质管理器抽象基类（后端实现转换/缓存）
    Scene.hpp                    # 场景容器（管理根节点）

    # 相机与视图
    Camera.hpp                   # 相机（视图矩阵 + 投影矩阵）
    CameraManipulator.hpp        # 相机操纵器（轨道/摇移/缩放/第一人称）

    # 拾取与求交
    Ray.hpp                      # 射线定义
    RayIntersection.hpp          # 射线-几何体求交

    # 渲染管道
    RenderPass.hpp               # 渲染通道（相机+目标+清除状态）
    RenderTarget.hpp             # 渲染目标（颜色+深度缓冲）
    RenderCommand.hpp            # 渲染命令
    RenderEngine.hpp             # 高层渲染引擎（帧循环）
    RenderBackend.hpp            # 渲染后端抽象接口
    RenderBackendRegistry.hpp    # 渲染后端自注册表（按名创建，无编译期依赖）

  src/
    Camera.cpp
    CameraManipulator.cpp
    Drawable.cpp
    Geometry.cpp
    Material.cpp
    Node.cpp
    Ray.cpp
    RayIntersection.cpp
    RenderBackendRegistry.cpp
    RenderCommand.cpp
    RenderEngine.cpp
    RenderPass.cpp
    RenderTarget.cpp
    Scene.cpp
```

## 3. 核心类设计

### 3.1 `Drawable`（可绘制对象基类）

纯可渲染对象（几何 + 材质）。不携带场景图变换或层级，那些由所属 `Node` 提供。

```cpp
class V_GRAPHICS_API Drawable : public Object, public RefCounted<Drawable> {
    V_OBJECT_META_DECL;

  public:
    Drawable();
    ~Drawable();

  public:
    /** @brief 绘制对象名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 局部坐标系边界框。 */
    Aabbd boundingBox() const;

    /** @brief 绑定的材质（可为空）。 */
    Material* material() const;
    void setMaterial(Material* m);

  protected:
    /** @brief 子类实现：计算局部边界框。 */
    virtual Aabbd computeBoundingBox() const = 0;

  private:
    struct Data;
    Data* const d;
};

using DrawablePtr = intrusive_ptr<Drawable>;
```

> 说明：可见性、局部/世界变换不放在 `Drawable`，而是由所属 `Node` 管理（见 3.5b）。

### 3.2 `Geometry`（几何体）

> ⚠ 本节为**历史草稿**（`Drawable` 基类 + `shape()` 已弃）。`triangleCount()` 已于 2026-09-04 移除：
> Geometry 现为开放 loc buffer 数据面，不保证是三角网格；图元计数只留 `vertexCount()`。

包装 `vine::geometry::Shape` 的几何可绘制对象。

```cpp
class V_GRAPHICS_API Geometry : public Drawable {
    V_OBJECT_META_DECL;

  public:
    Geometry();
    ~Geometry();

  public:
    /** @brief 关联的 vine::geometry::Shape（可为空）。 */
    const vine::geometry::Shape* shape() const;
    void setShape(vine::geometry::Shape* shape);

    /** @brief 三角形数量（非网格形状返回 0）。 */
    std::size_t triangleCount() const;

    /** @brief 顶点数量（非网格形状返回 0）。 */
    std::size_t vertexCount() const;

  protected:
    Aabbd computeBoundingBox() const override;

  private:
    struct Data;
    Data* const d;
};

using GeometryPtr = intrusive_ptr<Geometry>;
```

> 说明：具体形状类型通过 `shape->shapeType()` 查询（`ShapeType` 定义于
> `vine::geometry`），不再在 `Geometry` 上复制一份类型枚举。

### 3.3 `Material`（材质）

定义对象的渲染属性。

```cpp
class V_GRAPHICS_API Material : public Object, public RefCounted<Material> {
    V_OBJECT_META_DECL;

  public:
    Material();

  public:
    /** @brief 材质名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 漫反射颜色 RGBA。 */
    Colorf diffuse() const;
    void setDiffuse(const Colorf& color);

    /** @brief 高光颜色 RGB，A=镜面强度。 */
    Colorf specular() const;
    void setSpecular(const Colorf& color);

    /** @brief 环境光颜色 RGB。 */
    Colorf ambient() const;
    void setAmbient(const Colorf& color);

    /** @brief 光泽度（Phong 指数）。 */
    float shininess() const;
    void setShininess(float shine);

    /** @brief 透明度 [0, 1]，1=完全不透明。 */
    float opacity() const;
    void setOpacity(float alpha);

    /** @brief 本材质采样的纹理（可为空）。 */
    raw_ptr<Texture> texture() const;
    void setTexture(intrusive_ptr<Texture> texture);

  private:
    struct Data;
    Data* const d;
};

using MaterialPtr = intrusive_ptr<Material>;
```

> 说明：颜色使用浮点 `Colorf`（[0,1]），而非 8-bit 的 `Color`。
> 材质采样的是 `graphics::Texture` **对象**，不再是路径字符串：
> 路径字符串在 `src/` 里零调用，是个死 API。`Texture` 的 shape / 源图设计见 `.ai/design/imaging-design.md`。

### 3.4 `Scene`（场景）

场景图容器，管理所有根级 `Node` 的树形关系。

> ⚠ **已过时（2026-09-07）**：以下代码为早期**多 root** 版本（`addNode/removeNode/nodes`）。
> `Scene` 已收敛为**单根**——`setRoot(intrusive_ptr<Node>)/root()`，内容经根 `Group` 组合；
> 以 `graphics-scene-graph.md` 顶部落地记录为准。

```cpp
class V_GRAPHICS_API Scene : public Object, public RefCounted<Scene> {
    V_OBJECT_META_DECL;
    V_DISABLE_COPY_MOVE(Scene);

  public:
    Scene();
    ~Scene();

  public:
    /** @brief 场景名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 添加根级节点。 */
    void addNode(Node* node);

    /** @brief 移除根级节点。 */
    void removeNode(Node* node);

    /** @brief 获取所有根级节点。 */
    std::vector<NodePtr> nodes() const;

    /** @brief 根据名称递归查找节点。 */
    NodePtr findNode(const String& name) const;

    /** @brief 删除所有节点。 */
    void clear();

    /** @brief 场景总边界框。 */
    Aabbd boundingBox() const;

    /** @brief 收集渲染命令（含视锥剔除与排序）。
     *
     * @param camera 相机；为 nullptr 时返回空列表。
     * @return 渲染命令（不透明近→远在前，透明远→近在后）。
     */
    std::vector<RenderCommand> collectRenderCommands(const Camera* camera) const;

  private:
    struct Data;
    Data* const d;
};

using ScenePtr = intrusive_ptr<Scene>;
```

> 说明：场景直接管理 `Node`（而非 `Drawable`）。命令收集内部会先按相机
> view-projection 做视锥剔除，再按深度排序透明/不透明物体。

### 3.5 `Camera`（相机）

仅管理相机参数和矩阵计算，不管理渲染目标或视口。

```cpp
class V_GRAPHICS_API Camera : public Object, public RefCounted<Camera> {
    V_OBJECT_META_DECL;

  public:
    enum class ProjectionType {
        Perspective,   // 透视投影
        Orthographic,  // 正交投影
    };

  public:
    Camera();
    ~Camera();

  public:
    /** @brief 相机名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 投影类型。 */
    ProjectionType projectionType() const;

    /** @brief 用 look-at 参数设置视图矩阵。 */
    void setViewMatrixAsLookAt(const Vec3d& eye, const Vec3d& center, const Vec3d& up);

    /** @brief 用透视参数设置投影矩阵。 */
    void setProjectionMatrixAsPerspective(double fovy, double aspect, double zNear, double zFar);

    /** @brief 用正交参数设置投影矩阵。 */
    void setProjectionMatrixAsOrtho(double left, double right, double bottom, double top,
                                    double zNear, double zFar);

    // 相机参数（最近一次 setViewMatrixAsLookAt 的记录）
    Vec3d eye() const;
    Vec3d target() const;
    Vec3d up() const;

    // 视锥参数（最近一次投影设置的记录）
    double nearPlane() const;
    double farPlane() const;
    double fieldOfView() const;
    double aspectRatio() const;
    double orthographicHeight() const;

    /** @brief 视图矩阵。 */
    Mat4d viewMatrix() const;

    /** @brief 投影矩阵。 */
    Mat4d projectionMatrix() const;

    /** @brief 屏幕坐标（归一化 [0,1]）转世界射线。 */
    Ray screenToWorldRay(const Vec2d& screenPos) const;

  private:
    struct Data;
    Data* const d;
};

using CameraPtr = intrusive_ptr<Camera>;
```

> 说明：镜像 OSG 相机 API，视图/投影矩阵一次性整体设置，避免中间不一致状态。
> 宽高比直接由调用方传入 `setProjectionMatrixAsPerspective`，无独立 `Viewport` 概念。

### 3.5b `Node`（场景图节点）

场景层级的基本单元，携带局部变换、父子链接、可见性和可绘制对象列表。

```cpp
class V_GRAPHICS_API Node : public Object, public RefCounted<Node> {
    V_OBJECT_META_DECL;

  public:
    Node();
    ~Node();

  public:
    /** @brief 节点名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 节点（及子树）是否可见。 */
    bool isVisible() const;
    void setVisible(bool visible);

    /** @brief 局部变换。 */
    Mat4d localTransform() const;
    void setLocalTransform(const Mat4d& transform);

    /** @brief 世界变换（父链级联）。 */
    Mat4d worldTransform() const;

    /** @brief 父节点（根节点为 null）。 */
    Node* parent() const;

    /** @brief 添加/移除子节点。 */
    void addChild(Node* child);
    void removeChild(Node* child);

    /** @brief 所有子节点。 */
    std::vector<intrusive_ptr<Node>> children() const;

    /** @brief 添加/移除可绘制对象。 */
    void addDrawable(Drawable* drawable);
    void removeDrawable(Drawable* drawable);

    /** @brief 所有可绘制对象。 */
    std::vector<DrawablePtr> drawables() const;

    /** @brief 子树的世界空间包围盒。 */
    Aabbd boundingBox() const;

  private:
    struct Data;
    Data* const d;
};

using NodePtr = intrusive_ptr<Node>;
```

### 3.5c `CameraManipulator`（相机操纵器）

处理用户交互，更新相机参数。

`CameraManipulator` 与其他核心类一致继承 `RefCounted<CameraManipulator>`；
`RenderEngine` 以 `intrusive_ptr` 持有当前操纵器，`setCameraManipulator`
收 `intrusive_ptr`（retain），`cameraManipulator()` 返 `raw_ptr`（借用）。

```cpp
class V_GRAPHICS_API CameraManipulator : public RefCounted<CameraManipulator> {
  public:
    enum class Mode {
        Orbit,           // 轨道（绕目标点旋转）
        Pan,             // 摇移（平移）
        Zoom,            // 缩放
        FirstPerson,     // 第一人称
    };

  public:
    explicit CameraManipulator(Camera* camera);

  public:
    /** @brief 当前模式。 */
    Mode mode() const;
    void setMode(Mode m);

    // 轨道模式
    void orbit(double deltaYaw, double deltaPitch);
    void setOrbitCenter(const Vec3d& center);
    double orbitRadius() const;
    void setOrbitRadius(double radius);

    // 摇移模式（屏幕像素增量）
    void pan(double screenDx, double screenDy);

    // 缩放模式（滚轮）
    void zoom(double factor);

    // 第一人称模式
    void moveForward(double distance);
    void moveRight(double distance);
    void moveUp(double distance);
    void rotate(double deltaYaw, double deltaPitch);

    // 通用
    /** @brief 应用到相机。 */
    void apply();

  private:
    struct Data;
    Data* const d;
};
```

> 说明：`pan()` 直接接收屏幕像素增量，内部按当前轨道半径换算世界位移，无需 `Viewport`。

### 3.6 `RenderCommand`（渲染命令）

表示单个可绘制对象的渲染指令。

```cpp
struct V_GRAPHICS_API RenderCommand {
    /** 可绘制对象。 */
    DrawablePtr drawable;

    /** 材质。 */
    MaterialPtr material;

    /** 世界坐标系矩阵。 */
    Mat4d modelMatrix;

    /** 是否透明（需要后序渲染）。 */
    bool isTransparent = false;

    RenderCommand() = default;

    /** @brief 构造并初始化（材质透明度 < 1 时标记为透明）。 */
    RenderCommand(Drawable* d, Material* m, const Mat4d& model);
};
```

> 说明：排序由 `Scene::collectRenderCommands` 完成（不透明近→远、透明远→近），
> `RenderCommand` 本身不提供 depth 计算。

### 3.7 `RenderBackend`（渲染后端抽象）

定义不同渲染实现的接口。

```cpp
class V_GRAPHICS_API RenderBackend {
  public:
    virtual ~RenderBackend() = default;

    /** @brief 初始化后端。 */
    virtual bool initialize() = 0;

    /** @brief 清理后端资源。 */
    virtual void shutdown() = 0;

    /** @brief 开始一帧渲染。 */
    virtual void beginFrame() = 0;

    /** @brief 结束一帧渲染。 */
    virtual void endFrame() = 0;

    /** @brief 执行一个渲染通道（含命令收集）。 */
    virtual void executePass(const RenderPass* pass,
                             const std::vector<RenderCommand>& commands) = 0;

    /** @brief 设置渲染目标（nullptr 为默认帧缓冲）。 */
    virtual void setRenderTarget(RenderTarget* target) = 0;

    /** @brief 渲染命令列表。 */
    virtual void render(const std::vector<RenderCommand>& commands,
                        const Camera* camera) = 0;

    /** @brief 清空缓冲。 */
    virtual void clear(const Color& backgroundColor, bool clearDepth = true) = 0;

    /** @brief 交换缓冲区（double buffer）。 */
    virtual void swapBuffers() = 0;

  protected:
    RenderBackend() = default;
};
```

### 3.8 `RenderPass`（渲染通道）

描述一个完整的渲染阶段：相机 + 渲染目标 + 清除状态。

```cpp
class V_GRAPHICS_API RenderPass : public Object, public RefCounted<RenderPass> {
    V_OBJECT_META_DECL;

  public:
    RenderPass();

  public:
    /** @brief 通道名称。 */
    String name() const;
    void setName(const String& name);

    /** @brief 渲染目标（可为空，表示默认帧缓冲）。 */
    RenderTarget* renderTarget() const;
    void setRenderTarget(RenderTarget* target);

    /** @brief 通道使用的相机。 */
    Camera* camera() const;
    void setCamera(Camera* camera);

    /** @brief 清除颜色。 */
    Color clearColor() const;
    void setClearColor(const Color& color);

    /** @brief 是否清除深度缓冲。 */
    bool shouldClearDepth() const;
    void setShouldClearDepth(bool clear);

    /** @brief 执行此通道：清屏 → 收集命令 → 交给后端渲染。 */
    void execute(Scene* scene, RenderBackend* backend);

  private:
    struct Data;
    Data* const d;
};

using RenderPassPtr = intrusive_ptr<RenderPass>;
```

### 3.9 `RenderTarget`（渲染目标）

离屏帧缓冲，管理颜色与深度缓冲。

```cpp
class V_GRAPHICS_API RenderTarget : public Object, public RefCounted<RenderTarget> {
    V_OBJECT_META_DECL;

  public:
    enum class ColorFormat {
        RGBA8,    // 8-bit unsigned normalized RGBA.
        RGBA16F,  // 16-bit float RGBA.
        RGBA32F,  // 32-bit float RGBA.
    };

    enum class DepthFormat {
        D16,   // 16-bit depth.
        D24,   // 24-bit depth.
        D32,   // 32-bit unsigned depth.
        D32F,  // 32-bit float depth.
    };

  public:
    RenderTarget();

  public:
    /** @brief 附加颜色缓冲。 */
    void attachColor(ColorFormat format);

    /** @brief 附加深度缓冲。 */
    void attachDepth(DepthFormat format);

    /** @brief 渲染目标尺寸。 */
    int width() const;
    int height() const;
    void setSize(int w, int h);

    /** @brief 回读颜色缓冲（RGBA8，w*h*4 字节）。
     *
     * 未实现占位：logical RenderTarget 无 GPU 像素，恒返回全零；真实读回入口为
     * RenderBackend::readColorBuffer()/readDepthBuffer()（默认 unsupported，
     * 实现点在各后端，见 VsgRenderer 离屏 image 的 TRANSFER_SRC usage）。
     */
    std::vector<std::uint8_t> readColorBuffer() const;

    /** @brief 回读深度缓冲（[0,1]，w*h 个 float）。 */
    std::vector<float> readDepthBuffer() const;

  private:
    struct Data;
    Data* const d;
};

using RenderTargetPtr = intrusive_ptr<RenderTarget>;
```

### 3.10 `RenderEngine`（高层渲染引擎）

平台无关的帧循环驱动：begin → 主通道 → end → swap。

```cpp
class V_GRAPHICS_API RenderEngine : public Object, public RefCounted<RenderEngine> {
    V_OBJECT_META_DECL;

  public:
    explicit RenderEngine(RenderBackend* backend);
    ~RenderEngine();

  public:
    /** @brief 绑定的后端。 */
    RenderBackend* backend() const;

    /** @brief 初始化后端。 */
    bool initialize();

    /** @brief 释放后端资源。 */
    void shutdown();

    /** @brief 渲染一帧（begin → 主通道 → end → swap）。 */
    void frame();

    /** @brief 场景。 */
    void setScene(Scene* scene);
    Scene* scene() const;

    /** @brief 主通道相机。 */
    void setCamera(Camera* camera);
    Camera* camera() const;

    /** @brief 主通道。 */
    void setMainPass(RenderPass* pass);
    RenderPass* mainPass() const;

  private:
    struct Data;
    Data* const d;
};
```

> 说明：引擎构造时自动创建默认场景、相机与主通道，主通道默认绑定该相机。

#### 现状对齐：有序场景通道管线（2026-09 落地）

`RenderEngine` 每帧执行一条以主通道（order 0）为锚的**有序场景通道管线**：

```
pre passes (order < 0)  →  main pass (order 0)  →  post passes (order > 0)  →  overlays (升序 zOrder)
```

- `addPass(intrusive_ptr<RenderPass> pass, int order)` 注册额外场景通道，同一 order 内按
  插入顺序稳定执行；`removePass` / `clearPasses` / `passCount` 管理。
- **内容关联由 Engine 管理**：`addPass(pass, content, order)` 给通道绑显式场景；
  `bindPassContent(pass, scene)` 运行时重绑（null = 回落引擎场景）；`contentOf(pass)`
  返回“有效内容”。执行时解析 `slot.content ?: engine.scene_`，因此 `setScene()` 对
  未绑定的通道是**单点更新**。`RenderPass` 本身**不携带 Scene**（可复用 stage）。
- **RenderPass = 借用的 view(Camera) + 持有的输出 RenderTarget + clear/viewport**：
  `setRenderTarget` 收 `intrusive_ptr`（通道持有输出资源，析构在 .cpp 出外联）；
  `renderTarget()==nullptr` 表示画到默认 backbuffer。Camera 仍是借用（共享视图）。
- 每个额外通道用自己的 camera / renderTarget / clear 状态绘制引擎场景或其绑定的 content。
  典型用途：`order<0` 的 shadow map / depth / g-buffer 预通道；`order>0` 的后处理 / 合成通道。
- **顺序必须显式且确定**：渲染正确性依赖先后（阴影贴图必须先于被照亮几何体、后处理必须
  在着色之后、UI 覆盖最后）。引擎按 int `order` 升序、稳定排序，正是为此提供确定序。
- 顶部 / HUD pass = 普通高 `order` 注册 pass（**无独立 Overlay 类**，2026-09 已删）：内容经
  addPass 内容绑定、相机经独立 `applyCameraMirror` 跟随、子视口/清屏/显隐都在 RenderPass 上
  （setViewport/setClearEnabled/setEnabled/onSurfaceResized）；绘制在 order 末尾 → 与 3D 场景
  分层。详见 `graphics-overlay.md`。
- ⚠ 2026-09-03 **Design B 变更**：上述“主通道锚点”模型已重构——`RenderEngine` 不再有
  main pass / `setMainPass` / `mainPass`，空启动，pipeline 完全显式（统一 `passes_` 按 order
  执行 + overlays 最后）；引擎只保留可选的 `scene()`(默认内容) 与 `masterCamera()`(交互主相机，
  manipulator 驱动，可 null)。“窗口 pass”= camera==masterCamera 且 RT==null 的注册 pass。
  默认 viewer 由 fw `RenderControl` 引导。`graphics-render-pipeline.md` 顶部有完整变更记录。
- 多 Pass 调度 / 内容关联 / pass 衔接的**完整设计基线**：见 `graphics-render-pipeline.md`。

## 4. 设计模式与约定

### 4.1 引用计数
- `Scene`、`Camera`、`Drawable`、`Material`、`Node`、`RenderPass`、`RenderTarget`、`RenderEngine`、`CameraManipulator` 都继承 `RefCounted<T>`
- 使用 `intrusive_ptr<T>` 管理所有权
- 借用/所有权分界：getter 返回与“不 retain”的借用入参用 `vine::raw_ptr<T>`；
  会 retain（存入 owning 字段/容器）的 setter/add 入参用 `intrusive_ptr<T>`
  （by value + `std::move`），与 `setBackend`/`addPass` 及 robotics
  `Visual::setMaterial(const intrusive_ptr<...>&)` 一致
- 公开 API 接受原始指针（调用者管理生命周期）或返回 `intrusive_ptr`

### 4.2 不可复制/移动
- `Scene` 使用 `V_DISABLE_COPY_MOVE` 防止意外复制
- 其余核心类因继承 `RefCounted<T>` 隐式不可拷贝

### 4.3 数据隐藏（Pimpl）
- 所有公开类都使用 `Data* const d` 指针隐藏实现
- 便于二进制兼容性和后续扩展

### 4.4 命名规范
遵循 Copilot 编码指引：
- 实例方法：camelCase，无 `get` 前缀（如 `name()`）
- 布尔 getter：`is`/`has` 前缀（如 `isVisible()`）
- setter：`set` 前缀（如 `setName()`）
- 类型别名：`Ptr` 后缀（如 `ScenePtr`）

## 5. 实现阶段规划

### 阶段 1：框架（已完成）
- [x] 定义所有公开头文件和 API
- [x] 实现基础类框架（Pimpl + Data 结构）
- [x] 编写单元测试框架

### 阶段 2：场景管理（已完成）
- [x] 实现 `Scene`/`Node` 树形结构
- [x] 实现 `Node` 的层级变换（worldTransform 级联）
- [x] 递归边界框计算
- [x] 场景查询接口（findNode / drawables）

### 阶段 3：视图管理（已完成）
- [x] 实现 `Camera` 相机参数（OSG 风格矩阵式）
- [x] 视图矩阵和投影矩阵计算
- [x] 屏幕-世界坐标转换（screenToWorldRay）
- [x] 视锥剔除（Frustum Culling，位于 `collectRenderCommands`）

### 阶段 4：几何与材质（已完成）
- [x] 集成 `vine::geometry::Shape`
- [x] `Geometry` 包装层
- [x] `Material` 属性管理
- [ ] 纹理管理（可选，未实现——仅存纹理文件路径）

### 阶段 5：渲染后端（部分完成）
- [x] 定义 `RenderBackend` 接口
- [x] 命令缓冲系统（`Scene::collectRenderCommands` + 排序）
- [x] 排序与批处理（不透明/透明排序已实现；批处理未实现）
- [x] 首个 `RenderBackend` 实现（`gfx_backend_vsg` 插件；手撸 Vulkan 后续提供）

## 6. 分层架构与依赖关系

### 6.1 分层定位（重要）

`graphics` 是**基础与抽象层**，不绑定任何具体图形 API。渲染后端通过
`RenderBackend` 接口注入，**vsg 只是其中一种实现**，后续还会有纯手撸
（如手写 Vulkan/OpenGL）实现。所有后端一律实现同一个 `RenderBackend`
接口，上层代码（`RenderEngine`/`RenderPass`）只依赖抽象。

```
┌────────────────────────────────────────────┐
│ graphics（基础与抽象层）                      │
│  ├─ 数据结构：Scene/Node/Camera/Material    │
│  ├─ 查询：RayIntersection/boundingBox()       │
│  ├─ 抽象接口：RenderBackend                 │
│  └─ 驱动：RenderEngine/RenderPass           │
└───────────────────┬────────────────────────┘
                    │ 实现 RenderBackend
      ┌─────────────┴──────────────┐
      │                            │
┌─────▼──────────────┐    ┌───────▼────────┐
│ gfx_backend_vsg    │    │ 手撸（实现 2）  │
│ （插件，实现 1）    │    │ (未来)         │
│ VsgRenderer       │    │                │
└───────────────────┘    └────────────────┘
```

后端实现原则：
- 后端只实现 `RenderBackend` 接口，不反向依赖 graphics 之外的模块
- 场景图/相机等数据结构由 graphics 层定义，后端只做"翻译 + 提交"
- `RenderEngine` 负责帧循环，后端负责具体 API 调用
- 换后端 = 换一个 `RenderBackend` 实现，上层代码零改动

### 6.2 依赖关系

```
Graphics
  ├── vi::Core              (Object, RefCounted, intrusive_ptr)
  ├── vi::Global            (Math: Vec3d, Mat4d, Color, Colorf, etc.)
  ├── vi::Geometry          (Shape, TriangleMesh, IndexedTriangleMesh)
  └── 实现层（可选，均为 appfw 插件，运行时加载）：
      ├── gfx_backend_vsg   (vsg 实现：VsgRenderer 实现 RenderBackend)
      └── (未来) gfx_backend_vulkan (手撸实现)
```

`graphics` 本身不反向依赖任何实现层；后端插件也只依赖 `vi::Appfw` +
`vi::Graphics` + 各自的三方库。

### 6.3 后端自注册（RenderBackendRegistry）

应用创建具体后端时，**不应直接 `new`**（会引入对后端模块及其三方库的编译期
依赖）。改为**自注册工厂**：

```
┌─ graphics ────────────────────────────────────┐
│ RenderApi（技术族位标志集合）                  │
│   None/Vulkan/OpenGL2/OpenGL3/OpenGLES/       │
│   Direct3D（V_ENABLE_ENUM_FLAGS 位运算）       │
│   renderApiToString() → "vulkan | opengl3"    │
│ RenderBackendInfo（元数据）                    │
│   name / display_name / description /         │
│   version / vendor / api_flags(RenderApi 位集) │
│ RenderBackendFactory（抽象基类）                │
│   info() → RenderBackendInfo                  │
│   name() → info().name（便捷）                 │
│   create(scene, camera) → intrusive_ptr<RenderBackend> │
│ RenderBackendRegistry（单例）                  │
│   registerFactory(factory)  ← 后端自注册        │
│   create(name, scene, camera) → intrusive_ptr<RenderBackend>│
│   names() / has(name) / entries()（遍历查询）   │
│   Registrar<TFactory>（RAII 静态自注册助手）     │
└──────────────────────┬────────────────────────┘
                       │ 实现并注册
      ┌────────────────┴────────────────┐
      │                                 │
┌─────▼────────────────────┐    ┌───────▼────────┐
│ gfx_backend_vsg 插件      │    │ gfx_backend_ogre│
│ VsgRenderBackendFactory  │    │ （未来）        │
│ api_flags=Vulkan,        │    │ api_flags=      │
│ 注册 "vsg"               │    │  OpenGL|GLES|   │
│                          │    │  Direct3D|Vulkan│
│                          │    │ 注册 "ogre"     │
└──────────────────────────┘    └────────────────┘
```

**一个后端插件可注册多个后端实现**（如不同 GL 版本）：Registry 是
`map<name, factory>`，每个实现一个工厂、各带自己的 `RenderBackendInfo`；
`entries()` 遍历时天然包含同一插件注册的全部后端。

用法（上层不 include 任何后端头）：
```cpp
// create() 返回引用计数句柄 intrusive_ptr<RenderBackend>；后端生命周期自动管理
auto backend = RenderBackendRegistry::instance().create(u8"vsg", scene, camera);
engine->setBackend(backend);  // RenderEngine 持有引用，后端不早于引擎销毁

// 遍历已注册后端及其元数据（如 show_render_backends 命令）
for (const auto& entry : RenderBackendRegistry::instance().entries()) {
    const auto& info = entry.info;   // name / description / version ...
}
```

后端作为 appfw 插件注册（插件 `load()` 内）：
```cpp
// GfxBackendVsgPlugin.cpp
RenderBackendRegistry::instance().registerFactory(&s_factory);  // static VsgRenderBackendFactory s_factory;
...
V_DECLARE_PLUGIN(vine::vsg::GfxBackendVsgPlugin, u8"gfx_backend_vsg", ...);
```

`Registrar<T>` 助手也保留（供非插件、静态链接场景使用）。

要点与限制：
- **编译期**不依赖后端模块；`gfx_backend_vsg` 走纯运行时插件（DLL 部署到
  `<exe>/plugins/vine`，`PluginManager::loadAll()` 加载后注册）。
- 插件 `load()` 显式注册，规避跨 TU 静态初始化序问题，且卸载时机可控。
- 静态 `Registrar<T>` 自注册场景下：应在模块已链接后再 `create()`（如从
  `RenderEngine::initialize()` 或 main 之后），而非依赖注册先于 main 完成。

## 7. API 示例

```cpp
using namespace vine::graphics;

// 创建场景
auto scene = make_intrusive<Scene>();
scene->setName(u8"Main Scene");

// 创建材质
auto material = make_intrusive<Material>();
material->setName(u8"RedPlastic");
material->setDiffuse(Colorf(1.0f, 0.0f, 0.0f, 1.0f));
material->setOpacity(0.8f);

// 从 Shape 创建几何体，挂到节点上
auto node = make_intrusive<Node>();
auto geom = make_intrusive<Geometry>();
geom->setName(u8"Sphere");
geom->setShape(sphere_shape);
geom->setMaterial(material);
node->setLocalTransform(transform);
node->addDrawable(geom);

// 添加到场景（早期多 root 旧例；当前 Scene 为单根：先 setRoot(根 Group) 再 root->addChild）
scene->addNode(node);

// 创建相机
auto camera = make_intrusive<Camera>();
camera->setName(u8"MainCamera");
camera->setViewMatrixAsLookAt(Vec3d(0, 10, 15), Vec3d(0, 0, 0), Vec3d(0, 1, 0));
camera->setProjectionMatrixAsPerspective(60.0, 16.0 / 9.0, 0.1, 1000.0);

// 收集渲染命令（自动视锥剔除 + 排序）
auto commands = scene->collectRenderCommands(camera.get());

// 渲染（后端由调用方提供）
backend->clear(Color(51, 51, 51, 255));
backend->render(commands, camera.get());
backend->swapBuffers();
```

## 8. 文件清单

### 头文件（sdk/vine/graphics/）
- `graphics_global.hpp` — API 导出和命名空间宏
- `Drawable.hpp` — 可绘制对象基类
- `Geometry.hpp` — 几何体类（包装 Shape）
- `Material.hpp` — 材质定义（纯属性）
- `MaterialManager.hpp` — 材质管理器抽象基类
- `Node.hpp` — 场景图节点
- `Scene.hpp` — 场景容器
- `Camera.hpp` — 相机
- `CameraManipulator.hpp` — 相机操纵器
- `Ray.hpp` — 射线
- `RayIntersection.hpp` — 射线求交
- `RenderCommand.hpp` — 渲染命令结构
- `RenderPass.hpp` — 渲染通道
- `RenderTarget.hpp` — 渲染目标
- `RenderEngine.hpp` — 渲染引擎
- `RenderBackend.hpp` — 后端抽象接口

### 实现文件（src/）
- `Camera.cpp`
- `CameraManipulator.cpp`
- `Drawable.cpp`
- `Geometry.cpp`
- `Material.cpp`
- `MaterialManager.cpp`
- `Node.cpp`
- `Ray.cpp`
- `RayIntersection.cpp`
- `RenderCommand.cpp`
- `RenderEngine.cpp`
- `RenderPass.cpp`
- `RenderTarget.cpp`
- `Scene.cpp`

### 测试文件（tests/test_graphics/）
- `GraphicsTest.cpp` — 全部功能测试（场景/相机/操纵器/求交/渲染引擎 + 排序/剔除）

## 9. 未来扩展点

1. **光源系统**：环境光、定向光、点光源、聚光灯（`Light.hpp` 设计稿曾有，未实现）。
   计划：光源挂在 `Scene` 上；有序通道管线天然支撑 shadow map（以光源相机为
   `order<0` 通道）与 forward/deferred 光照通道；后处理通道消费前序通道的 RenderTarget。
2. **阴影**：阴影贴图、实时阴影
3. **粒子系统**：粒子发射、动画
4. **动画系统**：骨骼动画、关键帧插值
5. **优化**：八叉树加速、脏标记、缓存策略、批处理（draw call 合并）
6. **多渲染后端**：OpenGL、Vulkan、软光栅化（`RenderBackend` 接口已就绪）
7. **UI 集成**：ImGui 或自定义 UI 系统

## 10. 可见性 / 透明度三级模型（2026-09 落地）

为统一控制，`Scene`/`Node`/`Drawable` 各带**可见性**与**透明度**，`Material` 保留自身
`opacity()`：

- **可见性（AND）**：对象可绘制 ⇔ `Scene.isVisible ∧ 路径上每个 Node.isVisible ∧
  Drawable.isVisible`。命令收集时逐级判断，隐藏对象不进 `RenderCommand`。
- **透明度（乘积）**：有效 `opacity = Scene.opacity × Node.opacity（沿祖先链）×
  Drawable.opacity × Material.opacity`，`collectRenderCommands` 对每个 drawable
  算好写入 `RenderCommand.opacity`；`isTransparent = opacity < 1` 驱动不透明/
  透明分组 painter 排序。
- 新增 API：`Scene/Node/Drawable::opacity()/setOpacity()`；
  `Scene/Drawable::isVisible()/setVisible()`（`Node` 原有）。
- `RenderCommand` 语义升级：携带“已解析”的有效透明度（不再是后端各自再算）。
- vsg 后端把有效透明度写进逐顶点 alpha（blending 常开），任意一级透明度变化实时
  生效、零重建；具体见 `.ai/design/vsg-design.md` §9。
8. **拾取增强**：选择框选（框选）、可拾取对象接口（`Pickable.hpp` 设计稿曾有，未实现）
9. **视口系统**：独立 `Viewport` 类（设计稿曾有，当前宽高比直接传给 Camera）；
   跨平台窗口与输入事件由 `base/window` 模块（`vi::Window`）提供，`graphics`
   通过 Signal 订阅其事件驱动相机操纵
10. **纹理加载**：从文件加载纹理资源（当前 `Material` 仅存路径）

