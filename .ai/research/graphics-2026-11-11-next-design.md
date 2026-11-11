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

    [[nodiscard]] Handle allocate() noexcept;             // 唯一分配者
    void release(Handle handle) noexcept;                 // 唯一释放路径
    [[nodiscard]] bool alive(Handle handle) const noexcept;
    [[nodiscard]] std::size_t peakObjects() const noexcept;

  private:
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
- `domain` 不符 ⇒ `alive` 答 false（跨域误用可检测）；
- 线程：分配/释放在锁内；读只在发布时批量做。

### 3.3 行数据：hot / cold 分离（关键布局决定）

行的字段按**变更频率**分成两组，**分别分块**。这样"动画改一个关节"只复制 hot 块，cold 块零拷贝。

```cpp
inline constexpr std::uint32_t kRowsPerChunk = 32;   // 需实测（见未决问题 Q1）

/// 随动画变的（放置链结果）—— 改一个实例只复制它覆盖的 hot 块
struct HotChunk {
    std::array<Mat4d, kRowsPerChunk> world{};        // 128 B/行
    std::array<Aabbd, kRowsPerChunk> world_box{};    // 48 B/行
    std::array<Mat4d, kRowsPerChunk> chain{};        // 到本块第一行的父链（给块级裁剪用）
};
static_assert(sizeof(HotChunk) <= 32 * (128 + 48 + 128) + 64);

/// 只有结构/状态变化时才变的 —— 动画时整块共享，不复制
struct ColdChunk {
    std::array<Handle,  kRowsPerChunk> node{};       // NodeId（拾取/诊断用）
    std::array<Handle,  kRowsPerChunk> instance{};   // InstanceId
    std::array<Handle,  kRowsPerChunk> geometry{};   // GeometryId
    std::array<Handle,  kRowsPerChunk> material{};   // MaterialId（已解析）
    std::array<Handle,  kRowsPerChunk> program{};    // ProgramId（已解析）
    std::array<DrawState, kRowsPerChunk> state{};    // 深度/混合/剔除（已解析）
    std::array<std::uint32_t, kRowsPerChunk> layers{};
    std::array<std::int32_t,  kRowsPerChunk> draw_order{};
    std::array<float, kRowsPerChunk> opacity{};
    std::array<Aabbd, kRowsPerChunk> local_box{};
    std::array<std::uint8_t, kRowsPerChunk> flags{}; // visible / view_dependent / instanced …
};
```

**为什么这样切**：
- 动画 = 改 `world`/`world_box` ⇒ 只复制 hot 块（32 行 ≈ 9.7 KB），cold 块共享；
- 结构或状态 = 改 cold 块，hot 块随后由折叠重写；
- 块级裁剪用 `chain[0]` 把 `local_box` 的并集变换到世界，早期整块剔除。

### 3.4 快照

```cpp
class VN_GRAPHICS_API Snapshot final : public RefCounted<Snapshot> {
  public:
    [[nodiscard]] std::uint64_t epoch() const noexcept;          // 回收集的键
    [[nodiscard]] std::span<const HotChunkPtr>  hotChunks() const noexcept;
    [[nodiscard]] std::span<const ColdChunkPtr> coldChunks() const noexcept;
    [[nodiscard]] const ValueRegistry& registry() const noexcept; // ObjectId → 值
    [[nodiscard]] std::span<const LightValue> lights() const noexcept;
    [[nodiscard]] const Budget& budget() const noexcept;
  private:
    std::uint64_t                epoch_{ 0 };
    std::vector<HotChunkPtr>     hot_;        // std::shared_ptr<const HotChunk>
    std::vector<ColdChunkPtr>    cold_;
    std::vector<PrototypeFold>   prototypes_; // 原型 → 本地折叠块（行区间 + revision）
    std::vector<InstanceSpan>    instances_;  // InstanceId → 行区间（O(1) 定位）
    ValueRegistry                registry_;
    std::vector<LightValue>      lights_;
    Budget                       budget_{};
    friend class Publisher;                   // 只有 Publisher 能造
};

using SnapshotPtr = intrusive_ptr<const Snapshot>;
```

### 3.5 派生值

```cpp
struct View {                       // 纯值
    CameraSnapshot camera{};
    Viewport       viewport{};      // 带单位（Pixels）
    std::uint32_t  layer_mask{ kAllLayers };
    ProjectionKind projection{ ProjectionKind::Perspective };  // Perspective | Ortho | Screen
};

struct DrawItem {                   // 在 frame arena 里，soa 友好的定长记录
    GeometryId   geometry{};
    MaterialId   material{};
    ProgramId    program{};
    NodeId       node{};            // 拾取/诊断
    std::uint32_t primitive{ 0 };
    std::uint32_t instance_first{ 0 };
    std::uint32_t instance_count{ 0 };
    std::uint32_t sort_key{ 0 };    // 参见 §4.4
    float         depth{ 0.0F };
    const Mat4d*  world{ nullptr }; // 指向 Snapshot 的 hot 块（不可变）
    DrawState     state{};
    float         opacity{ 1.0F };
    std::uint8_t  transparent{ 0 };
};

struct FrameValue {                 // 纯值，交给后端
    SnapshotPtr                    snapshot{};
    std::array<PassDraws, kMaxPasses> passes{};
    std::uint32_t                  pass_count{ 0 };
    TargetSize                     target{};
    std::uint64_t                  frame_id{ 0 };
};
```

### 3.6 计划（声明式会话形状）

```cpp
struct PassDesc {                   // 宿主写的声明（值）
    String        name{};           // 稳定名字：差分的键（C7）
    View          view{};           // 相机值 + 视口 + 掩码
    TargetDesc    target{};         // 屏幕 / 离屏（句柄或声明）
    ClearPolicy   clear{};
    DrawState     state{};          // pass 级基础状态
    std::int32_t  order{ 0 };
    std::vector<String> inputs{};   // 屏幕 pass 的输入（按名字）
};

struct FramePlan {                  // 一帧的完整声明
    std::vector<PassDesc> passes{};
    bool operator==(const FramePlan&) const = default;   // 差分用值比较
};
```

### 3.7 后端契约

```cpp
struct SessionConfig {              // 能力协商（R25）
    std::vector<Capability> required{};
    std::vector<Capability> optional{};
    PipelineDepth           depth{ PipelineDepth::One };
};

struct FrameResult {
    PresentOutcome outcome{ PresentOutcome::Skipped };
    std::uint64_t  frame_id{ 0 };
    std::uint64_t  snapshot_epoch{ 0 };
    std::uint32_t  builds{ 0 };      // 本帧描述的条目数（稳态应为 0）
    std::uint32_t  diagnostics{ 0 };
    double         record_micros{ 0.0 };
    double         execute_micros{ 0.0 };
};

/// C++20 没有 std::expected：一个极小的 Result，只有"值"或"一个问题"两态。
template <class T> class Result {
  public:
    static Result ok(T value);
    static Result fail(Diagnostic problem);
    explicit operator bool() const;          // true = 成功
    [[nodiscard]] T&                value();       // 前置：成功
    [[nodiscard]] const Diagnostic& problem() const; // 前置：失败
};

class Backend {                      // 后端 = 纯执行器
  public:
    virtual ~Backend() = default;
    virtual Result<void> create(const SessionConfig& config) = 0;         // 协商（失败带 code）
    virtual void         alignPlan(const FramePlan& plan) = 0;            // 帧边界差分
    virtual FrameResult  execute(const FrameValue& frame, FrameArena& arena) = 0;
    virtual void         release() = 0;
};
```

**`execute` 的三个契约**：不读内容对象（只有值 + 句柄）；只在引擎线程调用；不改 `FrameValue`。

---

## 4. 算法

### 4.1 publish：O(变更)

```
publish(scene) -> SnapshotPtr:
  1. 若批未关闭（edit_depth > 0）⇒ 返回 nullptr（不发布，R11）
  2. next = 浅拷贝 prev：块指针数组、instances_、prototypes_、registry_（都是指针/小结构，O(块数)）
  3. for 每个脏实例（编辑路径记录的 dirty set，C15 的等价物）:
       span = instances_[instance]                 // O(1)
       hot  = cloneOnWrite(next.hot_[span.chunk])  // 每块只复制一次
       折叠：world[row] = chain × local(row)        // 只写 span 覆盖的行
       重算 world_box（从 local_box + world）
  4. for 每个被引用且 revision 变了的素材 id（按 id→rev 表比对）:
       next.registry_[id] = 新值（内容侧刚 build 的 MaterialDescription / GeometryDescription …）
  5. 结构变化（增删/换父）：重建受影响的原型块与 instances_ 映射（O(受影响子树)）
  6. next.epoch = prev.epoch + 1；原子换指针；记录预算（R8）
```

- **成本**：改一个关节 = 1~2 个 hot 块复制（~20 KB）+ 子树行的矩阵乘；与总行数无关。
- **材质/几何改动无需通知**（R12）：`id→rev` 表在注册表里，发布时比对；这正是今天 `refreshMaterials`
  的正确位置，但**不扫行**。
- **为什么保留"折叠进快照"而不是每帧折叠**：今天实测 2000 drawable 相机移动的 collect 是 2160 µs
  （fold 已缓存），若每帧重折叠就是 O(n) 增量 ⇒ 会撞 `N1`。所以折叠结果留在快照里，
  用 **hot/cold 分离**把"动画的复制成本"压到块级。

### 4.2 fold：原型算一次，放置 N 次

```
PrototypeFold { NodeId id; std::uint64_t revision; RowSpan local; }   // 本地行（父=单位矩阵）
rows(instance) = Placement(实例链) ∘ PrototypeLocalRows(原型, revision)
```

- 原型内部（不含根放置）只依赖原型自身与 revision ⇒ 缓存进快照；
- 改某个实例的 `local` ⇒ 只重写它 span 覆盖的 hot 行（矩阵乘）；
- 改原型 ⇒ revision 变 ⇒ 所有引用它的实例在下一发布重写；
- 无环检查在插入时做（沿祖先走，O(深度)）。

### 4.3 select：纯函数

```
select(snapshot, view, arena) -> DrawList:
  frustum = Frustum::from(view)                       // 与今天同一个工具
  for 每个冷块 c（按行序）:
     if (c.flags.view_dependent 任一)  ⇒ 逐行处理（billboard 等）
     else if (frustum.isOutside(transform(c.local_box_union, c.chain[0])))  ⇒ 整块跳过
     else 逐行：cull → 组成 DrawItem（world 指针指向 hot 块，零拷贝）
  排序（§4.4）
```

- 同一次调用内**不触碰任何内容对象**（只有值与句柄）⇒ 任意线程可跑，可并行；
- 视图无关的折叠已在快照里，select 每视图只付裁剪 + 排序（`R15`）。

### 4.4 排序键与全序

```
sort_key = (draw_order+2^31) << 32 | (layer:8) | (transparent:1) | (depth_quantized:23)
```

- 是**全序**（`R17`）：任意输入的任意置换，排序结果相同；
- 不透明段按 key 升序；透明段按 `(key, -depth)`（远到近）；
- `depth_quantized` 用近平面/远平面归一后取 23 位，避免 NaN 与精度抖动。

### 4.5 record：分块并行 + 确定性合并

```
record(drawlist, resident, arena):
  把 pass 内的 DrawItem 按 N 段切分（N = 并行度）
  每段独立写各自的 bump 区（互不重叠）
  合并：按段序拼接 ⇒ 与串行逐项相同（R20）
```

**注意**：vsg 的录制本身要碰设备状态；本设计里"record"指**构建命令块**（纯），
交给后端 `execute` 的部分仍是单线程。并行度先做 1，接口留好（`G8`）。

### 4.6 回收：快照代际

```
每个驻留条目记录 last_epoch（最后一次被哪个 snapshot 引用的代）
oldest_live = min(在飞帧各自的 snapshot.epoch, 当前 snapshot.epoch)
在帧边界：释放所有 last_epoch < oldest_live 且不再被引用的条目
```

- 精确：给定活快照集，回收集**可枚举**（`T6`）；
- 不需要 `retirement` 时间窗、`markUsed`、`kept` 兜底；
- 与 `PipelineDepth > 1` 天然兼容：每个在飞槽持有自己的快照引用（这正是 `I4`）。

### 4.7 计划差分

```
alignPlan(next):
  key = FNV1a(name)                      // 稳定名字（C7）
  匹配 → 复用驻留；新增 → 创建；消失 → 帧边界释放
  顺序：order 升序，同 order 按声明序（与今天一致）
```

- 宿主随时可发布新计划（不必等帧），**下一帧边界**生效 ⇒ "帧在飞时改形状"不可表达（`R21`）。

---

## 5. 线程模型

| 角色 | 谁 | 规则 |
|---|---|---|
| **写者** | 一条线程（通常 UI） | 编辑场景；调 `publish`；同一时刻只有一条（`C2`） |
| **引擎线程** | Session 持有 | 唯一碰设备；消费 `FrameValue` |
| **任意线程** | 查询/派生 | 只读 `Snapshot`（不可变）⇒ 安全 |

**顺序交接**（多线程、不并发）是允许的：写者可以从 A 换到 B，前提是交接处有 **happens-before**
（mutex / CV / future / join / atomic）。判据就是 `scripts/tsan_check.sh`。

**恰好一条值回边**：`FrameResult` 由引擎线程生产、写者消费（用于"用画面里的相机瞄准"，`R18`）。

---

## 6. 内存模型

| 项 | 方案 |
|---|---|
| 每帧派生数据 | **三缓冲 arena**（bump），帧完成即整体丢弃；峰值 = 3 × 单帧字节 |
| 快照常驻 | 分块 + 结构共享；常驻 ≈ 行表字节 + 共享块（`N3`：≤ 2×） |
| 引擎侧表 | `std::vector<Entry>` 按 `index` 索引；大小 = 峰值对象数（注册表可查，`R8` 的诊断同源） |
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
    NodeId            node{};    // 可空
    std::uint32_t     slot{ 0 }; // 纹理槽等
    std::uint64_t     value{ 0 }; // 数值上下文（预算/行数/字节）
};
```

| code（示例） | 触发 |
|---|---|
| `kBudgetRowsExceeded` | 行预算超限（`R8`） |
| `kPrototypeCycle` | 原型包含成环（`R7`） |
| `kCapabilityMissing` | 必选能力缺失（`R25`） |
| `kContentNotPublished` | 命令引用了未发布的素材（对齐今天的行为） |
| `kPipelineNotReady` | 异步编译未完成（该 draw 跳过，episode 一次） |

单一路由 + `ReportOnce` episode（沿用今天已验证的机制）。

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
| Q3 | 并行 record 的真实收益 | vsg 单线程执行可能限制；先留接口不做 |
| Q4 | 值类型的内存占用（`MaterialParameters` 含 `shared_ptr`） | 影响拷贝成本，测量后决定是否改成句柄 + 表 |
| Q5 | `domain:8` 在单域场景是否浪费 | 保留（8 bit 换可检测）；可压缩到 flags 位 |
| Q6 | 单位类型化对 API 的侵入度 | 先只做 `Viewport`/`TargetSize` 试点 |
| Q7 | 屏幕空间放置是否并入 `ViewPlacement` | 是；但要保证与 `ProjectionKind::Screen` 的语义不重复 |
