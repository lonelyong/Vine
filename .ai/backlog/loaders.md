# loaders —— 开放待办

> 只写**还没做、有触发条件**的条目；做完删除，决定不做也删除（理由进 ADR）。
> 现行设计见 `../design/meshio-brepio-design.md`。

## 1. 点云 I/O 的归属与入口

- **触发**：出现要读写点云文件的消费者（PCD、LAS/LAZ、E57、PTS/PTX、XYZ，或只有顶点的 PLY），或模型查看器要显示这类文件。
- **内容**：在 `src/loaders/` 下新开 `pointcloudio`，**不进 `meshio`**——那个模块的名字说的是网格，让它返回 `PointCloud` 违反 `G4`；与 `graphics-geometry.md` D6（拒绝把 `PointCloud` 做成 `Mesh` 子类）是同一条理由，只是上升了一层。
  - 公开面与 `meshio` / `brepio` / `imageio` 同形：表驱动格式枚举、`Result` / `IoError`、状态归实例、无单例。
  - `.ply` 的扩展名重叠不是冲突：仓库没有全局 loader 注册表（`defaultInstance()` 已删），每个模块的 `supportedExtensions()` 只描述“我能读什么”。`meshio` 继续按面理解 `.ply`；`pointcloudio` 只在真需要“只有顶点的 PLY”时也声明 `.ply`，由调用方按“要网格还是要点云”选 loader。需要“随便读一个文件”的统一入口时，才需要一个嗅探内容的调度层——那时再做（`G8`）。
  - 任何情况下都不进 `geometry`（那里不碰 IO）也不进 `viz`（渲染是消费者）：数据是 `geometry::PointCloud`，文件在 loaders。
  - assimp（本仓库 v5.4.3）没有点云导入器：PLY 是唯一能“只有顶点”的入口，而且读回来是一个没有面的 mesh；PCD、LAS/LAZ、E57、PTS/PTX、XYZ 都不在它的格式家族里。LAS/LAZ 要 LASzip 或 PDAL，E57 要 libE57Format，PCD/PTS/XYZ 可以手写解析，厂商原始格式（FLS/RXP/ZFS）没有公开读取路径。
- **前置设计问题**：`Model::Entry` 只持有 `Mesh`，点云装不进去。默认做法是单独一个入口返回 `Result<intrusive_ptr<PointCloud>>`（像 `brepio` 直接返回 `BrepShape`）；给 `Model` 加条目类型要等“一个文件里既有网格又有点云”的真实来源（`G8`）。
- **验收**：新模块的测试（`test_pointcloudio`）覆盖“PCD / 只有顶点的 PLY → `PointCloud`，点数与法线/颜色通道和文件一致”。
