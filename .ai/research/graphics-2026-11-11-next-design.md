# graphics-next：初步设计

> **状态**：调研提案的一部分（**非现行设计**；未落地、未决策）
> **日期**：2026-11-11
> **属于**：`graphics-2026-11-11-next.md`（总纲）的 §2 展开
> **范围**：**怎么做** —— 分层、概念、类型草案、数据布局、算法、线程/内存模型、诊断、后端契约。
> 要什么见 `graphics-2026-11-11-next-requirements.md`，值不值得见 `graphics-2026-11-11-next-benefits.md`。
> **注意**：这里的所有签名都是**草案**，用于评审与原型，不是公开 API 承诺。

---

## 1. 分层与依赖

```
vine (String / RefCounted / intrusive_ptr / Buffer)
 └ math
    └ geom::spatial（Aabb / Ray）
       └ graphics::sdk      —— 只有概念与值类型，无后端、无 src
          └ graphics        —— 作者层 + 发布 + 派生（与后端无关）
             └ gfx_backend_*  —— execute
                └ appfw       —— 宿主：表面、输入、GUI
```

**唯一的跨边界载荷**：值类型（`Snapshot` / `View` / `DrawList` / `FrameValue` / `FrameResult`）
+ 一个接口（`Backend`）。`graphics` 不得 include 任何后端头；`sdk` 不得 include `src`。

---

## 2. 概念（详细）

| 概念 | 拥有的数据 | 不变式 | 谁改、何时 |
|---|---|---|---|
| **Scene** | 作者树根（`Node` + `Instance` 树）、灯集、写者标记 | 同时只有一个写者；发布只由写者调 | 内容线程，随编辑 |
| **Node（原型）** | 放置、子节点、`DrawState`、载荷（`GeometryId` + 图元/实例）、`revision` | 身份构造即固定；可无父；包含无环 | 内容线程 |
| **Instance** | `NodeId` + `local` + 覆盖 + 子 `Instance` | 属于**一棵树**；换父不改 `InstanceId` | 内容线程 |
| **Geometry/Material/Program/Texture** | 不可变值 + `rev` | 值不可变；`rev` 单调 | 内容线程（构造新值） |
| **Snapshot** | 行块（hot/cold）、原型折叠缓存、素材注册表、灯集、预算、`epoch` | 发布后不再变；行数 ≤ 预算 | 只在 publish 里产生 |
| **View** | 相机值、视口（带单位）、层掩码、投影种类 | 纯值 | 每帧构造 |
| **DrawList** | 有序 `DrawItem`（arena 内） | 全序；透明段后置 | 每 (snapshot,view) 一次 |
| **FrameValue** | `SnapshotPtr` + 每 pass 的 `DrawList` + 计划 + 目标尺寸 | 纯值 | 每帧一次 |
| **Session** | 设备、句柄表、管线缓存、arena、引擎线程、计划副本 | 一条引擎线程；计划按名字差分 | 帧边界 |
| **FrameResult** | outcome、`frame_id`、`snapshot epoch`、耗时、`builds`、诊断数、呈现的 View | 纯值 | 每帧一次，发布给宿主 |

---

## 3. 类型草案

### 3.1 身份

```cpp
namespace vn::graphics {

/// 64 位：generation:24 | domain:8 | index:32
class Handle {
  public:
    static Handle make(std::uint32_t index, std::uint32_t generation, std::uint8_t domain) noexcept;
    [[nodiscard]] std::uint32_t index() const noexcept;
    [[nodiscard]] std::uint32_t generation() const noexcept;
    [[nodiscard]] std::uint8_t  domain() const noexcept;
    [[nodiscard]] bool isNull() const noexcept;
    friend bool operator==(const Handle&, const Handle&) noexcept = default;
  private:
    std::uint64_t bits_{ 0 };
};

/// 强类型：不同 kind 是不同类型，编译期不能互传（C4 的唯一性 + 类型安全）
template <class Tag> class Id {
  public:
    Id() = default;
    explicit Id(Handle handle) noexcept : handle_(handle) {}
    [[nodiscard]] Handle handle() const noexcept { return handle_; }
    [[nodiscard]] bool isNull() const noexcept { return handle_.isNull(); }
    friend bool operator==(const Id&, const Id&) noexcept = default;
  private:
    Handle handle_{};
};

struct NodeTag; struct GeometryTag; struct MaterialTag; struct ProgramTag; struct TextureTag;
using NodeId     = Id<NodeTag>;
using GeometryId = Id<GeometryTag>;
using MaterialId = Id<MaterialTag>;
using ProgramId  = Id<ProgramTag>;
using TextureId  = Id<TextureTag>;

} // namespace vn::graphics
```

### 3.2 注册表（内容域的账本）

```cpp
class VN_GRAPHICS_API ContentRegistry {
  public:
    explicit ContentRegistry(std::uint8_t domain = 0);   // 测试各自建域 ⇒ id 可复现
    static ContentRegistry& instance() noexcept;          // 进程默认域（便利）

    /// 分配即定型：调用者拿不到未包装的 Handle，所以 GeometryId / NodeId 不能互传（审计 A12）。
    template <class Tag> [[nodiscard]] Id<Tag> allocate() noexcept { return Id<Tag>(allocateHandle()); }
    void release(Handle handle) noexcept;                 // 唯一释放路径
    [[nodiscard]] bool alive(Handle handle) const noexcept;
    [[nodiscard]] std::size_t peakObjects() const noexcept;

  private:
    [[nodiscard]] Handle allocateHandle() noexcept;
    struct Slot { std::uint32_t generation{ 1 }; std::uint32_t next_free{ kNone }; };
    std::vector<Slot> slots_;
    std::uint32_t     free_head_{ kNone };
    std::uint8_t      domain_{ 0 };
    mutable std::mutex mutex_;                            // 只在 create/destroy（罕见）
};
```

规则：
- 分配：自由链表弹槽（空则增长），`++generation`；
- 释放：`++generation`（**gen 0 = 空槽**），压回自由链表；
- `generation` 达 `0xFFFFFF` ⇒ **永久退休该槽**（不复用，防 ABA）；
- `domain` 不符 ⇒ `alive` 答 false（跨域误用可检测）；**kind 的类型安全**由 `allocate<Tag>()` 保证，
  不是靠 `domain`；
- 线程：分配/释放在锁内；读只在发布时批量做。

### 3.3 行数据：hot / cold 分离（关键布局决定）

行的字段按**变更频率**分成两组，**分别分块**。这样"动画改一个关节"只复制 hot 块，cold 块零拷贝。

```cpp
inline constexpr std::uint32_t kRowsPerChunk = 32;   // 需实测（见未决问题 Q1）

/// 内容引用：id 是身份，revision 是“哪一版”。
/// 旧 Snapshot 永远带着旧 revision；引擎表按 (id, generation, revision) 驻留，不能只按 id 解析。
struct ContentRef {
    Handle        id{};
    std::uint64_t revision{ 0 }; // 与 SDK 每个 `revision()` 同宽：窄的键在四十亿次编辑后回绕（落地时改的，见 audit 步骤 4）
};

/// 随动画变的（放置链结果）—— 改一个实例只复制它覆盖的 hot 块
struct HotChunk {
    std::array<Mat4d, kRowsPerChunk> world{};        // 128 B/行
    std::array<Aabbd, kRowsPerChunk> world_box{};    // 48 B/行
    Aabbd                            world_bound{};  // 本块 world_box 的并集：块级裁剪的唯一事实
};
static_assert(sizeof(HotChunk) <= 32 * (128 + 48) + 48 + 64);

/// 只有结构/状态变化时才变的 —— 动画时整块共享，不复制
struct ColdChunk {
    std::array<Handle,  kRowsPerChunk> node{};       // NodeId（拾取/诊断用）
    std::array<Handle,  kRowsPerChunk> instance{};   // InstanceId（拾取/诊断用）
    std::array<ContentRef, kRowsPerChunk> geometry{}; // GeometryId + revision
    std::array<ContentRef, kRowsPerChunk> material{}; // MaterialId + revision（已解析）
    std::array<ContentRef, kRowsPerChunk> program{};  // ProgramId + revision（已解析）
    std::array<DrawState, kRowsPerChunk> state{};    // 深度/混合/剔除（已解析）
    std::array<std::uint32_t, kRowsPerChunk> layers{};
    std::array<std::int32_t,  kRowsPerChunk> draw_order{};
    std::array<float, kRowsPerChunk> opacity{};
    std::array<Aabbd, kRowsPerChunk> local_box{};
    std::array<std::uint8_t, kRowsPerChunk> flags{}; // visible / view_dependent / instanced …
};
```

**为什么这样切**：
- 动画 = 改 `world` / `world_box` / `world_bound` ⇒ 只复制 hot 块（32 行 ≈ 9.7 KB），cold 块共享；
- 结构或状态 = 改 cold 块，hot 块随后由折叠重写；
- **块级裁剪用 `world_bound`**（本块 `world_box` 的并集），不再用“第一行的父链”：
  一个 32 行块可以横跨兄弟子树，父链不同，`chain[0]` 会误剔除（审计 A8）。这样也省掉每行 128 B 的 `chain`。
  动画 pose 会让 `world_bound` 过期：posed 子树逐帧重算，或使用保守界，见 `-hot-paths` §3.2。
- **内容引用必须带 revision**（`ContentRef`）：只带 id 的引用在旧 Snapshot 里会解析到新值，
  直接违反 `R9` / `I1`（审计 A1）。带 revision 后，引擎表按 `(id, generation, revision)` 驻留，
  旧帧读旧版；`id+rev` 也是 dump / replay 的稳定键。

### 3.4 快照

```cpp
class VN_GRAPHICS_API Snapshot final : public RefCounted<Snapshot> {
  public:
    [[nodiscard]] std::uint64_t epoch() const noexcept;          // 回收集的键
    [[nodiscard]] std::span<const HotChunkPtr>  hotChunks() const noexcept;
    [[nodiscard]] std::span<const ColdChunkPtr> coldChunks() const noexcept;
    [[nodiscard]] const ValueRegistry& registry() const noexcept; // Handle → VersionedValue{revision, value}
    [[nodiscard]] std::span<const LightValue> lights() const noexcept;
    [[nodiscard]] const Budget& budget() const noexcept;
  private:
    std::uint64_t                epoch_{ 0 };
    std::vector<HotChunkPtr>     hot_;        // std::shared_ptr<const HotChunk>
    std::vector<ColdChunkPtr>    cold_;
    // 未变化的表共享，只有结构/值真的变了才换一张表（审计 A7）。
    std::shared_ptr<const std::vector<PrototypeFold>> prototypes_;
    std::shared_ptr<const std::vector<InstanceSpan>>  instances_;
    std::shared_ptr<const ValueRegistry>              registry_;
    std::shared_ptr<const std::vector<LightValue>>    lights_;
    Budget                       budget_{};
    friend class Publisher;                   // 只有 Publisher 能造
};

using SnapshotPtr = intrusive_ptr<const Snapshot>;
```

`ValueRegistry` 的每个条目带 revision：`VersionedValue{ std::uint32_t revision; XxxDescriptionPtr value; }`。
发布时按 `id → revision` 比对，只替换变了的条目；未变的条目与表共享。

### 3.5 派生值

```cpp
struct View {                       // 纯值
    CameraSnapshot camera{};
    Viewport       viewport{};      // 带单位（Pixels）
    std::uint32_t  layer_mask{ kAllLayers };
    ProjectionKind projection{ ProjectionKind::Perspective };  // Perspective | Ortho | Screen
};

struct PoseEntry {                  // 一个实例的局部放置覆盖；子树随之重算
    InstanceId instance{};
    Mat4d      local{};
};

struct PoseSet {                    // FrameRequest 用的非拥有视图；存储必须活到 present 返回
    std::span<const PoseEntry> entries{};
};

struct DrawItem {                   // 24 B，在 frame arena 里；id/state/material 经 row 从 cold 块取
    std::uint32_t row{ 0 };         // 稠密行号；chunk = row / kRowsPerChunk
    std::uint32_t instance_first{ 0 };
    std::uint32_t instance_count{ 0 };
    std::uint32_t flags{ 0 };       // transparent / view_dependent / instanced …
    std::uint64_t sort_key{ 0 };    // 参见 §4.4（64 位：order | layer | transparent | depth）
};
static_assert(sizeof(DrawItem) == 24);

struct PickResult {                 // 位置身份，不是原型身份（审计 A2）
    InstanceId    instance{};
    NodeId        prototype{};
    std::uint32_t row{ 0 };
    Vec3d         point{};
    double        depth{ 0.0 };
};

struct PassDraws {
    std::uint32_t                 target{ 0 };
    std::span<const DrawItem>     draws{};
};

struct FrameValue {                 // 纯值，交给后端
    SnapshotPtr                    snapshot{};
    std::array<PassDraws, kMaxPasses> passes{};   // 超限 → 稳定 code 的诊断，不是静默截断
    std::uint32_t                  pass_count{ 0 };
    TargetSize                     target{};
    std::uint64_t                  frame_id{ 0 };
};
```

`DrawItem` 不再带 `world` 指针、`NodeId`、`DrawState` 和重复的 `depth`；这些经 `row` 从 hot/cold 块取。
这样既满足 24 B 的热路径预算，也避免同一事实在冷块与命令里各存一份（审计 A6）。

### 3.6 计划（声明式会话形状）

```cpp
struct Target {
    enum class Kind { Surface, Transient, External };
    Kind        kind{ Kind::Surface };
    SurfaceId   surface{};          // Kind::Surface：哪个窗口
    String      name{};             // Kind::Transient / External：稳定名字
    TargetShape shape{};            // Kind::Transient：尺寸/格式/采样
};

struct Pass {                       // 唯一的 pass 值；RenderPass 退为构建器（见 -api §5.1）
    String        name{};           // 稳定名字：差分的键（C7）
    View          view{};           // 相机值 + 视口 + 掩码
    Target        target{};         // Surface / Transient / External
    ClearPolicy   clear{};
    DrawState     state{};          // pass 级基础状态
    std::int32_t  order{ 0 };
    std::vector<String> inputs{};   // 按稳定名字；PassId 只在 Session 内部存在
};

struct FramePlan {                  // 一帧的完整声明
    std::vector<Pass> passes{};
    bool operator==(const FramePlan&) const = default;   // 差分用值比较
};
```

`submit` 时校验：重复名字、未知输入、DAG 环、`order` 拓扑（读在前写在后）、超 `kMaxPasses`；
每条违规给稳定 `DiagnosticCode`，不靠字符串匹配（审计 A11）。

### 3.7 后端契约

```cpp
struct DeviceRequest {              // 能力协商 + 适配器选择（M1）
    String                    backend{};    // 空 = 默认后端
    std::vector<Capability>   required{};
    std::vector<Capability>   optional{};
};

struct SessionConfig {
    DeviceRequest device{};
    PipelineDepth depth{ PipelineDepth::One };
};

struct FrameQuery {                 // 这一帧“看到的世界”：查询吃它，不吃活相机/活场景
    SnapshotPtr              snapshot{};
    std::shared_ptr<const std::vector<PoseEntry>> poses{}; // 接受帧时拥有；不用 span 越界
    View                     view{};
    TargetSize               target{};
};

struct FrameResult {
    PresentOutcome outcome{ PresentOutcome::Skipped };
    std::uint64_t  frame_id{ 0 };
    std::uint64_t  snapshot_epoch{ 0 };
    FrameQuery     query{};          // presented_view + snapshot + poses（R18 / I7）
    std::uint32_t  builds{ 0 };      // 本帧描述的条目数（稳态应为 0）
    std::uint32_t  diagnostics{ 0 };
    double         record_micros{ 0.0 };   // 后端内部阶段；核心不假装它是纯函数
    double         execute_micros{ 0.0 };
};

/// C++20 没有 std::expected：一个极小的 Result，只有“值”或“一个问题”两态。
template <class T> class Result {
  public:
    static Result ok(T value);
    static Result fail(Diagnostic problem);
    explicit operator bool() const;          // true = 成功
    [[nodiscard]] T&                value();       // 前置：成功
    [[nodiscard]] const Diagnostic& problem() const; // 前置：失败
};

/// T = void 需要显式特化；否则 `static Result ok(T value)` 不能声明形参。
template <> class Result<void> {
  public:
    static Result ok();
    static Result fail(Diagnostic problem);
    explicit operator bool() const;
    [[nodiscard]] const Diagnostic& problem() const;
};

class Backend {                      // 后端 = 执行器；record 是它内部的阶段（见 §4.5）
  public:
    virtual ~Backend() = default;
    virtual Result<void> create(const SessionConfig& config) = 0;         // 协商（失败带 code）
    virtual void         alignPlan(const FramePlan& plan) = 0;            // 帧边界差分
    virtual Result<Requirements> prepare(const Snapshot& snapshot, const FramePlan& plan) = 0; // R23
    virtual FrameResult  execute(const FrameValue& frame, FrameArena& arena) = 0;
    virtual void         release() = 0;
};
```

**`execute` 的三个契约**：不读内容对象（只有值 + 句柄）；只在引擎线程调用；不改 `FrameValue`。

---

## 4. 算法

### 4.1 publish：O(变更)，元数据表共享

```
publish(scene) -> Result<SnapshotPtr>:
  1. 批未关闭（edit_depth > 0）⇒ ok(prev)（未发布；epoch 不变；不返回 null）
  2. next = prev 的浅拷贝：hot/cold 块指针数组（O(块数)）；prototypes_/instances_/registry_/lights_
     先共享 prev 的 shared_ptr<const 表>
  3. for 每个脏实例:
       span = instances_[instance]
       hot = cloneOnWrite(next.hot_[span.chunk])      // 每块只复制一次
       world[row] = parent_world × local(row)          // 只写 span 覆盖的行
       world_box[row] = transform(local_box[row], world[row])
     for 每个受影响的 hot 块: world_bound = union(world_box[块内行])
  4. for 每个被引用且 revision 变了的素材:
       写 registry_ 的一个 delta（持久化/COW 表），条目为 VersionedValue{revision, 新值}
       // 未变的表/条目共享；不是整表拷贝
  5. 结构变化（增删/换父/原型 revision）：只替换受影响的 prototypes_ / instances_ 表
     （未变的表继续共享；O(受影响子树)，不是 O(整棵树)）
  6. next.epoch = prev.epoch + 1；原子换指针；记录预算（R8）
```

- **成本**：改一个关节 = 1~2 个 hot 块复制（~10 KB）+ 子树行的矩阵乘；与总行数无关。
- **元数据不是 O(变更)**：`prototypes_` / `instances_` / `registry_` 是共享表；只有结构或值真的变了才换表。
  若实现成“每次 publish 整体浅拷贝”，成本是 O(原型数 + 实例数 + 素材数)（审计 A7 / `K11`）。
- **材质/几何改动无需通知**（R12）：`id→rev` 表在 registry 里，发布时比对；不扫行。
- **旧帧读旧值**：registry 与冷块都带 revision；引擎表按 `(id, generation, revision)` 驻留（A1）。
- **为什么保留“折叠进快照”而不是每帧折叠**：今天实测 2000 drawable 相机移动的 collect 是 ~2.1 ms
  （fold 已缓存），若每帧重折叠就是 O(n) 增量 ⇒ 会撞 `N1`。折叠结果留在快照里，
  用 **hot/cold 分离**把“动画的复制成本”压到块级。

### 4.2 fold：原型算一次，放置 N × 行数 次

```
PrototypeFold { NodeId id; std::uint64_t revision; RowSpan local; }   // 本地行（父=单位矩阵）
rows(instance) = Placement(实例链) ∘ PrototypeLocalRows(原型, revision)
```

- 原型内部（不含根放置）只依赖原型自身与 revision ⇒ 缓存进快照；
- **v1（正确性优先）**：实例行物化；改一个实例 = 重写它 span 覆盖的 hot 行；
  5000 行原型插 10 次 = **5 万次矩阵乘**，不是 10 次（审计 A5）。`R3` / `T4` 只把“原型折叠”计为 1，
  放置乘法单独计。
- **B（测量后）**：共享原型的 local rows + 每个实例一个根变换（`DrawItem` 带实例矩阵/索引），
  静态实例的内存与放置成本降到 O(rows + N)；内部覆盖（单个关节）迫使被覆盖子树物化。见 `Q8`。
- 改原型 ⇒ revision 变 ⇒ 所有引用它的实例在下一发布重写；
- 无环检查在插入时做（沿祖先走，O(深度)）。

### 4.3 select：纯函数

```
select(snapshot, view, arena) -> DrawList:
  frustum = Frustum::from(view)                       // 与今天同一个工具
  for 每个冷块 c（按行序）:
     hot = snapshot.hotChunks()[c]
     if (c.flags.view_dependent 任一)  ⇒ 逐行处理（billboard 等）
     else if (frustum.isOutside(hot.world_bound))  ⇒ 整块跳过   // 唯一裁剪事实
     else 逐行：cull → 组成 DrawItem{ row, instance_first, instance_count, flags, sort_key }
  排序（§4.4）
```

- 同一次调用内**不触碰任何内容对象**（只有值与句柄）⇒ 任意线程可跑，可并行；
- 视图无关的折叠已在快照里，select 每视图只付裁剪 + 排序（`R15`）。
- `DrawItem` 只存稠密行号：id / state / material 从冷块取，world 从 hot 块取（§3.5）。
- 动画 pose 会让 `world_bound` 过期：posed 子树逐帧重算 `world_bound`，或保守剔除（`-hot-paths` §3.2）。

### 4.4 排序键与全序

```
sort_key = (std::uint64_t(draw_order) + 0x80000000u) << 32
         | std::uint64_t(layer) << 24
         | std::uint64_t(transparent) << 23
         | depth_quantized(23 bits)
```

- `sort_key` 是 **64 位**（`DrawItem::sort_key` 也是 `std::uint64_t`；审计 A6）；
- 是**全序**（`R17`）：任意输入的任意置换，排序结果相同；
- 不透明段按 key 升序；透明段按 depth 倒序，key 的高位仍保证 `order` / `layer` 优先；
- `depth_quantized` 用近平面/远平面归一后取 23 位，避免 NaN 与精度抖动。

### 4.5 record：后端内部阶段

```
record 发生在 Backend::execute 内部；核心只要求后端在 FrameResult 报告 record_micros / execute_micros。
```

**注意**：vsg 的录制本身要碰设备状态；本设计**不把 record 写成核心的纯函数**，也不承诺核心层分块并行。
后端可以自行分块并行，但合并结果的确定性（`R20`）由后端自己的用例证明。若将来出现第二个后端需要共享
纯 `CommandList`，再按 `G8` 立接口，不在本版（审计 A13）。

### 4.6 回收：快照代际

```
每个驻留条目 (id, generation, revision) 记录 last_epoch（最后一次被哪个 snapshot 引用的代）
oldest_live = min(在飞帧各自的 snapshot.epoch, 当前 snapshot.epoch)
在帧边界：释放所有 last_epoch < oldest_live 且不再被引用的条目
```

- 精确：给定**会话已知的**活快照集，回收集**可枚举**（`T6`）；
- 不需要 `retirement` 时间窗、`markUsed`、`kept` 兜底；
- 与 `PipelineDepth > 1` 天然兼容：每个在飞槽持有自己的快照引用（这正是 `I4`）。
- **`I4` 的准确说法**：资源存活 ⟺ 被 Session 已接受或在飞的 Snapshot 引用；
  宿主在 Session 外持有的 Snapshot 不钉住 GPU 资源（下次提交会重建）。需要钉住时用 `Session::pin`（审计 A9）。

### 4.7 计划差分

```
alignPlan(next):
  key = name（字符串；64 位 FNV 只做哈希索引，命中后仍比较名字，防碰撞）
  重复名字 / 未知输入 / DAG 环 / 超 kMaxPasses ⇒ 一条稳定 code 的诊断，计划不生效
  匹配 → 复用驻留；新增 → 创建；消失 → 帧边界释放
  顺序：order 升序，同 order 按声明序（声明序是值的一部分）
  SurfaceId / Target 是差分键的一部分；表面尺寸变化只重建受影响的 transient
```

- 宿主随时可发布新计划（不必等帧），**下一帧边界**生效 ⇒ "帧在飞时改形状"不可表达（`R21`）。
- `PassId` 只在 Session 内部；宿主计划里只有名字（审计 X1）。

---

## 5. 线程模型

| 角色 | 谁 | 规则 |
|---|---|---|
| **写者 / 写者域** | 一条线程（或有 happens-before 的交接域） | 编辑场景；调 `publish`；**一个原型和所有引用它的 Scene 同属一个域**（`C2b`）；跨线程共享可变原型不支持 |
| **引擎线程** | Session 持有 | 唯一碰设备；消费 `FrameValue` |
| **任意线程** | 查询/派生 | 只读 `Snapshot`（不可变）⇒ 安全 |

**顺序交接**（多线程、不并发）是允许的：写者可以从 A 换到 B，前提是交接处有 **happens-before**
（mutex / CV / future / join / atomic）。判据就是 `scripts/tsan_check.sh`。

**共享原型的发布触发**：Node 没有 Scene 反向指针（`Node.hpp` 明说），所以原型自身的变化不会沿 parent 链
通知到引用它的每个 Scene。Scene 在 publish 时比较“自己引用的原型 revision 表”（O(引用的原型数)），
发现变化才重建；这要求一个原型的所有引用者处于同一写者域（`C2b`，审计 A3）。
已发布 Snapshot 只读，因此跨线程读取安全；问题只存在于“发布读原型 vs 另一线程写原型”。

**恰好一条值回边**：`FrameResult` 由引擎线程生产、写者消费；它携带 `FrameQuery`（snapshot + poses + view），
所以“用画面里的相机/姿态瞄准”是纯值查询，不需要再读活相机或活场景（`R18` / `I7`）。

---

## 6. 内存模型

| 项 | 方案 |
|---|---|
| 每帧派生数据 | **三缓冲 arena**（bump），帧完成即整体丢弃；峰值 = 3 × 单帧字节 |
| `DrawItem` | 24 B（`row` + 两个实例计数 + `flags` + 64 位 `sort_key`；审计 A6） |
| hot 块 | 32 × (128 + 48) + 48 ≈ **5.7 KB**；动画复制单位 |
| cold 块 | 每行 2 个 `Handle` + 3 个 `ContentRef` + 状态/层/order/opacity/box/flags；结构变化才复制 |
| 快照常驻 | 分块 + 结构共享；未变的 `prototypes_` / `instances_` / `registry_` 表共享；`N3`：≤ 2× 行表字节 |
| 引擎侧表 | 每类一张按 `index` 的槽表；槽内按 `(generation, revision)` 存条目；大小 = 峰值对象版本数（注册表可查，`R8` 的诊断同源） |
| 上传暂存 | 从 arena 切；不保留大块（不去做 T26 那条策略） |
| 预算 | `Budget{ rows, instances, bytes }` 写在快照里，发布时校验并报 episode |

---

## 7. 诊断

```cpp
struct Diagnostic {
    Severity          severity;
    DiagnosticCode    code;      // 稳定枚举，测试按 code 断言（R28）
    String            message;   // 人读
    std::uint64_t     pass_key{ 0 };
    NodeId            node{};    // 可空（原型）
    InstanceId        instance{}; // 可空（位置；拾取/pose 相关）
    std::uint32_t     slot{ 0 }; // 纹理槽等
    std::uint64_t     value{ 0 }; // 数值上下文（预算/行数/字节/revision）
};
```

| code（示例） | 触发 |
|---|---|
| `kBudgetRowsExceeded` | 行预算超限（`R8`） |
| `kPrototypeCycle` | 原型包含成环（`R7`） |
| `kPrototypeMissing` | `Instance` 引用的原型已不在（`T22`） |
| `kCapabilityMissing` | 必选能力缺失（`R25`） |
| `kContentNotPublished` | 命令引用了未发布的素材（对齐今天的行为） |
| `kPipelineNotReady` | 异步编译未完成（该 draw 跳过，episode 一次） |
| `kDuplicatePassName` / `kUnknownInput` / `kPlanCycle` | 计划校验失败（`R38` / `T30`） |
| `kMaxPassesExceeded` | 超过 `kMaxPasses`（不静默截断） |
| `kPoseInvalid` | pose 引用了不存在的实例 / 非有限矩阵 / 重复实例（`T26`） |
| `kSurfaceOutOfDate` / `kTargetOutOfDate` | 表面/目标在帧边界失效（R27 / M10） |

单一路由 + `ReportOnce` episode（沿用今天已验证的机制）；`code` 稳定，消息文本不参与断言。

---

## 8. 关闭

| 相位 | 做什么 | 不变式 |
|---|---|---|
| `Running` | 正常驱动 | 一条引擎线程 |
| `Stopped` | 引擎线程 join；不再接受帧 | 幂等 |
| `Drained` | arena 归还；在飞帧的引用释放 | 无泄漏 |
| `Released` | 设备资源释放；注册表可继续存在（内容不依赖它） | 幂等 |

任何相位重复调用都安全（`R26`）；顺序由**代码**保证（成员顺序 + 显式相位），不靠注释。

---

## 9. 未决问题（open questions，先写下来）

| # | 问题 | 怎么定 |
|---|---|---|
| Q1 | `kRowsPerChunk` 取 32 是否最优 | 原型测 16/32/64 的发布与 select 成本 |
| Q2 | 原型折叠缓存放 Snapshot 还是原型自己发布值 | 先 Snapshot（安全）；测量多场景共享后再谈 |
| Q3 | 后端内部并行 record 的真实收益 | 先做 1；vsg 的录制要碰设备状态，核心不承诺并行（审计 A13） |
| Q4 | 值类型的内存占用（`MaterialParameters` 含 `shared_ptr`） | 影响拷贝成本，测量后决定是否改成句柄 + 表 |
| Q5 | `domain:8` 在单域场景是否浪费 | 保留（8 bit 换可检测）；可压缩到 flags 位 |
| Q6 | 单位类型化对 API 的侵入度 | 先只做 `Viewport`/`TargetSize` 试点 |
| Q7 | 屏幕空间放置是否并入 `ViewPlacement` | 是；但要保证与 `ProjectionKind::Screen` 的语义不重复 |
| Q8 | 原型实例化用 A（物化）还是 B（共享 local rows + per-instance transform） | 先 A 保证正确性；测 `T23` 的内存/放置成本后再决定 B（审计 A5） |
| Q9 | 元数据表用持久化 map 还是 `shared_ptr<const Table>` + delta | 先共享整表；结构变化频繁时再上持久化表；`K11` 判死 |
| Q10 | `FrameResult.query.poses` 拷贝还是共享 | 先接受帧时拷贝（joint 数量小）；测量后再改成 `shared_ptr<const PoseBlock>` |
| Q11 | 一个设备多 Surface 的 Session 是否要做多队列 | 先单引擎线程 + 多 Surface；多队列属于后端，按 `G8` 留到有消费者 |
