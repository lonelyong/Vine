# 光源系统设计（v4）

> 状态：**v4a 已拍板，设计中**（2026-09-03）
> 前置：v1 有序多 pass 调度 ✅；v2 离屏 RTT ✅；v3 命名产出槽 publish/resolve + ScreenPass ✅。
> 本设计解决：v3 遗留的「离屏/主视图光照不一致」；并为阴影（v4b）铺路。

## 0. 问题与目标

现状（v3 实测暴露）：
- **主视图**由 vsg `createRenderGraphForView(..., assignHeadlight=true)` 内置一个与相机绑定的
  `DirectionalLight(headlight)`；
- **离屏视图**（`renderOffscreenTarget`）与 **overlay**（轴）各自只挂了一个 `AmbientLight`；
- 结果：离屏 PiP 颜色明显比主视图暗（只有环境光、无方向光）。

根源：光源目前是 **vsg 后端各视图手工拼的默认值**，Vine SDK 层没有光源概念，也无法表达
“这份内容(scene)配什么光”。shadow map / 多 pass 需要一份**平台无关、随内容走**的光源模型。

目标：
1. Vine SDK 提供平台无关的光源模型；`Scene` 承载光源列表。
2. Engine 把每个 pass 的“有效内容(Scene)”的光源解析出来交给后端 → **主/离屏/overlay 同源光照一致**。
3. 不破坏现状默认观感（scene 未配光时，各视图沿用各自既有默认光）。
4. 为 v4b（方向光 shadow map）预留：depth-only pass + 命名产出槽 + 采样。
5. 分期落地，小步验收（同 v2a→v2b→v3 的做法）。

## 1. 术语与模型

- **内容(Scene)** 是“数据生产者”：几何 + 光源（本文新增）。
- **光源**挂在 Scene 上（设计决策 #1：随内容走，而非随 pass/相机走）。
- **pass 的光照** = 该 pass 有效内容 Scene 的光源列表；无光源时回退到视图默认光。
- vsg 侧：光源 = `vsg::Light` 节点，作为 `vsg::View` 的子节点；`ViewDependentState` 每帧把它们
  收进 per-view `lightData` uniform（Phong/PBR 管线据此着色）。因此“给某个视图配光”=
  “维护该视图下的 vsg 光源节点集合”。

## 2. Vine SDK 光源模型（`vine/graphics`）

新头文件 `sdk/vine/graphics/Light.hpp`：

```cpp
enum class LightType { Ambient, Directional, Point, Spot };   // 本期只做 Ambient/Directional

class V_GRAPHICS_API Light : public Object, public RefCounted<Light> {
    V_OBJECT_META_DECL;
  public:
    Light();                                  // 默认 Ambient? 用显式工厂更清晰
    static intrusive_ptr<Light> createAmbient();
    static intrusive_ptr<Light> createDirectional(const Vec3d& direction);

  public:
    LightType type() const;
    bool isEnabled() const;      void setEnabled(bool);
    Color color() const;         void setColor(const Color&);   // 0-255 RGBA，沿用现有 Material 风格
    float  intensity() const;    void setIntensity(float);      // 倍数，1 = 常规
    // Directional:
    bool hasDirection() const;
    Vec3d direction() const;     void setDirection(const Vec3d&);   // 光传播方向
    // （Point/Spot 字段：position/attenuation/角度 —— 本期只留到类声明，v5 再实现）
    bool castShadow() const;     void setCastShadow(bool);      // v4b 方向光用
  private:
    LightType type_ = LightType::Ambient;
    bool enabled_ = true;
    Color color_{ 255, 255, 255, 255 };
    float intensity_ = 1.0f;
    Vec3d direction_{ 0.0, 0.0, -1.0 };
    bool cast_shadow_ = false;
};
using LightPtr = intrusive_ptr<Light>;
```

`Scene` 增加光源槽：
```cpp
void addLight(intrusive_ptr<Light> light);
void removeLight(raw_ptr<Light> light);
void clearLights();
const std::vector<LightPtr>& lights() const;   // 或 raw_ptr 视图
bool hasLights() const;
```

字段命名：私有 `snake_case_`；访问器 Qt 风格（无 `get`）。`Light` 用 `RefCounted`，持有方语义同
`Material`/`Node`（SDK 内已有惯例）。

### 默认光策略（决策 #2）
- 引擎/应用创建的内容 Scene **默认无光** → 各视图沿用其既有后端默认光（主 headlight / 离屏与
  overlay ambient），**零观感回归**。
- 只要某个 pass 的内容 Scene 显式配了光，该 pass 的视图就**用内容光替换默认光**。
- demo（VINE_VSG_OFFSCREEN）在引擎 scene 上加 1 Ambient + 1 Directional → 主/离屏同源，PiP 与
  主视图一致（这正是 v3 遗留观感修复）。

## 3. 内容→光照解析（Engine 层，决策 #3）

不做“全局灯表”，沿用 v3 “Engine 解析 pass 的有效内容”：
- `RenderPass::execute(scene, backend)` 已拿到该 pass 的**有效内容 scene**（main=engine scene、
  extra=slot.content 或 engine scene、overlay=overlay.content）。这些 scene 的光源即该 pass 的光。
- 因此把“传光”放在 `RenderPass::execute` 内、调用 `backend->render(...)` **之前**：
  ```cpp
  backend->setLights(scene->lights());   // 见 §4
  ```
  无需 Engine 新增逻辑（内容解析已在 v1 完成）——这是本设计里最省的一环。

> 例外：ScreenPass 不渲场景几何、不取 scene 光；其 `execute` 不调用 `setLights`（或传空）。

## 4. RenderBackend 契约

新增（默认 no-op，Mock/旧后端不受影响）：
```cpp
/** @brief Sets the light sources for the upcoming render() pass.
 *
 * Called by RenderPass::execute() from the pass's content scene before
 * render(). Backends that support scene lights replace any view-level default
 * light (e.g. a headlight) with the given lights; an empty list restores the
 * backend default. Lights are borrowed for the duration of the call.
 */
virtual void setLights(const std::vector<raw_ptr<const Light>>& lights) {}
```

vsg 侧实现要点（决策 #4，需小幅重构主视图的创建方式）：
- 后端要为**主场景视图**取得一个可改的 `vsg::View*`：`initialize()` 改用手工构建主
  `RenderGraph`+`View`（离屏/overlay 已是这个模式，见设计 §10.4 “每 pass 一图”收敛方向），
  不再依赖 `createRenderGraphForView(..., assignHeadlight=true)` 把 headlight 焊死在内部。
- 每个视图持一个“光源根”（`vsg::Group`，作为 view 的直接子节点）：
  - `main/offscreen`：默认挂 headlight 等价的 fallback（或保持现状）；收到内容光时先清光根再挂入。
  - `overlay`：默认空（现有 ambient 已够）或保留当前 ambient。
- `setLights()` 将 vine `Light` 转成 vsg 节点缓存（像 `VsgMaterialManager` 一样**按键缓存、只增改**）：
  `AmbientLight` / `DirectionalLight(direction)`，色值 0-255 → 归一化 float。
- 修改光源集合后需要 `viewer->compile()`（新管线/描述符）或仅改 lightData（ViewDependentState
  每帧刷新 value，静态集合无需重编——见 §5 性能）。

## 5. 与现有视图的关系

| 视图 | 现在的光 | v4a 之后 |
|---|---|---|
| 主场景 | vsg headlight（相机相对） | 手工 View；无 scene 光→保留默认方向光(可相机相对)，有 scene 光→用内容光 |
| 离屏 RT | AmbientLight(1,1,1,1) | 同上规则（内容光），与主一致 |
| overlay(轴) | AmbientLight | 默认 ambient；也可显式给 overlay content 配光 |

**预期行为**：demo 给 engine scene 配 1 ambient + 1 directional → 主视图与离屏 PiP 光照完全一致。

## 6. 阴影（v4b，本期只做方向光，先设计后做）

- 一个 `DirectionalLight` 若 `castShadow()`：Engine 自动在其之前插一个 order<0 的
  **shadow pass**（复用 v1 调度）：
  - 相机：光空间正交/透视矩阵（由光方向 + 场景包围盒 AABB 推导，存后端或 Light 派生）；
  - 输出：**depth-only RenderTarget**（复用 v2 离屏 + v3 publish，名字 `"ShadowMap"`）；
  - 内容：同 pass 的有效内容 scene（需 depth-only 收集/深度管线）。
- 消费：渲几何的 pass 声明输入 `"ShadowMap"`（v3 resolve），Phong 采样阴影。
  - vsg 侧：深度附件需可读（finalLayout 处理已在 v2 为 color 做过，深度同理）；
  - 采样：需把阴影深度绑定进几何管线 —— vsg Phong 默认不含 shadow 采样，**备选**：
    a) 升级/自制带 shadow 的着色器；b) 用 vsg 内置 `ShadowSettings`（`ViewDependentState` 已有
    shadow map 通路，需确认本 build Phong 是否支持，未验证）。
  - 本期**只做设计落档 + 深度可读前置**，采样方案单独立项验证（真 GPU 上）。
- PCF/软阴影、级联（CSM）留 v5。

## 7. 分期落地与验收

| 阶段 | 范围 | 验收 |
|---|---|---|
| **v4a** | SDK Light + Scene.lights；RenderPass::execute 传光；vsg setLights + 主视图手工化 + 默认光回退 | GraphicsTest：Light 访问器/Scene 增删、RenderPass 把 scene 光传给 MockBackend；lavapipe：VINE_VSG_OFFSCREEN 下 PiP 与主视图颜色一致 |
| **v4b** | 方向光 shadow：光相机 + depth-only order<0 pass publish “ShadowMap”；几何 pass resolve 采样 | 地面出现方向光阴影；validation/真机复验 |
| **v5（可选）** | Point/Spot、多光、CSM/PCF、自动依赖排序 | — |

实现顺序（按你的习惯：先 v4a 完整跑绿 + 实测，再 v4b 设计细化）。

## 8. 关键决策记录

1. **光源归属：Scene 级（已拍板）**：光不与 pass 绑定（同一内容渲主/离屏/shadow 需同光，pass 级会造成
   v3 色差问题复发），也不本期做 node 级（只有需要世界坐标位置/可挂载的点光、聚光才需 node 级；v4 只做
   Ambient+Directional 无需位置）。接口以 scene 为查询边界：v4 用“兄弟列表”，v5 升级为“遍历根节点收集
   光节点”（`Light` 变 Node 子类、可挂树），`scene->lights()` 与调用方不变，只换实现。
2. **默认光回退**：scene 无光 → 各视图沿用后端默认（主 headlight / overlay ambient），零回归；
   有光 → 内容光替换默认。**“无光”按“无可用光”理解（2026-09-11 补）**：空列表、全部 `setEnabled(false)`、
   或全部是不翻译的灯类型，都算无可用光 → **不动光根**，默认光保留。视图零光源会把整个 pass 照成黑
   （vsg Phong 对光集求和），而“禁用一盏灯”是关灯、不是要一个无光照场景；此规则与全屏程序路径
   （`fillLightPushBlock` 无可用光时补默认 ambient）一致。
3. **传光接口**：`RenderBackend::setLights(vector<raw_ptr<const Light>>)`，RenderPass::execute 在
   `render()` 前调用；默认 no-op，向后兼容。
4. **vsg 主视图手工化**是 v4a 的必要小重构（拿到 View 句柄才能挂/换光），与 §10.4 “每 pass 一图”
   的收敛方向一致；overlay/离屏已是该模式。
5. **阴影(v4b)先设计后做**：阴影采样方案（自定义 shader vs vsg ShadowSettings）单独立项并在真机验证，
   不在 v4a 里赌。

## 9. 落地记录（2026-09-03）

- v4a 拍板：SDK `Light`(Ambient/Directional) + `Scene::addLight/removeLight/lights`；
  `RenderBackend::setLights`（默认 no-op，`RenderPass::execute` 在 render() 前从内容 scene 传光）；
  vsg 主视图手工化（拿 View 句柄挂/换光根）+ 默认光回退（scene 无光沿用现状默认）；
  demo 给 engine scene 配 1 ambient + 1 directional 验证主/离屏同源。
- **v4a 已实现（lavapipe 实测）**：新增 `vine::graphics::Light`（Ambient/Directional，Colorf 色 + intensity）；
  `Scene::addLight/removeLight/clearLights/lights/hasLights`；`RenderBackend::setLights`（no-op 默认）由
  `RenderPass::execute` 在 render() 前把内容 scene 的光下发；vsg：主视图改手工构建（`RenderGraph::create
  (window, main_view)` + `main_light_group`(默认 createHeadlight) + vsg_scene），`VsgRenderer::setLights`
  排队、render()/renderOffscreenTarget() 里 `setGroupLights` 用内容光替换各视图默认灯（无光保留默认）；
  demo（VINE_VSG_OFFSCREEN）给 engine scene 配 ambient 0.25 + sun 方向光。实测：PiP 与主视图同光源、
  颜色一致（不再偏暗）；GraphicsTest 73 全过（新增 Light×3/Scene 光槽/RenderPass 传光 5 用例）。
  注意：vine `String` 不能直接赋给 vsg `std::string name`，故 vsg 灯节点不拷名。

