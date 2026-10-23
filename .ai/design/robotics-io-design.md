# Robotics IO 模块设计（XML 序列化）

> **状态**：现行
> **最后核对**：2026-10-23
> **范围**：设备/工作单元的 XML 序列化、VFS 打包与场景序列化前置。
> **权威来源**：`src/robotics/io/**`、`tests/test_robotics_io`。
> **待办**：`../backlog/`

## 1. 概要设计

### 1.1 问题与背景

`vn::robotics::workcell` 的 `Device` / `MotionDevice` / `Scanner` / `Workcell` 需要 XML 序列化与反序列化，
还要能把整棵场景（含设备、网格）打成单个包，不落临时目录。URDF 的 `<robot>` 语义不够（多 DoF、扫描仪、
材质库、包中包），所以采用自研 `<device>` / `<workcell>` 格式。

### 1.2 目标

- `RoboticsIO` 独立模块：`XmlIOBase` / `DeviceIO` / `WorkcellIO`。
- XML 覆盖设备、工作站、材质、形状、关节/DoF、扫描仪相机/投影仪。
- 位置是 `vn::io::Vfs + 虚拟路径`；打包为 `.vdevpkg` / `.vwspkg`（zip）。
- mesh 用原生 bin + XML 描述；共享 mesh 去重。

### 1.3 非目标

- 不用 URDF 格式；不实现 BRep/STEP（只预留 `<brep>`）。
- 不做 HDF5/CBOR/Zarr 等替代格式。
- 不落临时目录；包直接在内存/VFS 上读写。
- 不把材质就地写进 visual（禁止 `exportMaterial` 就地材质）。

### 1.4 一句话结论

`RoboticsIO` 是无状态 XML 序列化层：`XmlIOBase` 提供解析/导出原语，`DeviceIO`/`WorkcellIO` 组合它们；
位置是 `Vfs + path`，包是 zip，mesh 是原生 bin，设备定义可复用、包可嵌套。

## 2. 设计要点

### 2.1 无状态 `XmlIOBase`

- `XmlIOBase` / `DeviceIO` / `WorkcellIO` 都没有类级可变状态；每次操作的 `ParseContext`/`ExportContext`
  携带单位缩放、活动 `Vfs`、`vfs_dir`、工作站指针与警告缓冲。
- 实例可重入、可安全复用（含多线程，每次操作自带 context）。
- 加载失败抛 `std::runtime_error`；非致命问题进 `ctx.msgs`。

### 2.2 XML 格式

- 设备根 `<device name kind version>`；工作站根 `<workcell name version>`。
- 姿态统一四元数 `quat="x y z w"`（`Isometry3d` 原生四元数，无损往返；不做 RPY）。
- 长度单位写 metadata `length_unit`，加载/导出按 `LoadOptions`/`SaveOptions` 缩放。
- 关节 `type` = `fixed/revolute/prismatic/planar`；每个 DoF 一个 `<dof>`（planar = 3 个）。
- 形状：box/sphere/cylinder/cone/ellipsoid/triangle_mesh/indexed_triangle_mesh；`<brep>` 预留。
- 工作站 `<obj type="device|rigid_object">`；设备引用 `file` 可以是 `.vdev` 或 `.vdevpkg`。

### 2.3 VFS 与包

- 位置不是裸 path，而是 `vn::io::Vfs + 虚拟路径`；`DirectoryVfs` 调试、`ZipArchive` 包。
- `loadPkg(path)` 用 `open(..., ReadOnly)` 惰性读；`savePkg(cell, Vfs&, options)` 无源树 + `saveAs`。
- 包中包：场景包里的 `.vdevpkg` 条目读出字节，再开成嵌套 `ZipArchive` 解析。
- mesh 存储：`geoms/*.bin`（positions/normals/indices）+ XML `indexed_triangle_mesh` 引用；
  共享 mesh 只写一次（`mesh_paths` 去重）。

### 2.4 数据映射

| Vine 模型 | XML |
| --- | --- |
| `DeviceMetadata` | `<metadata>`（含 create/modified time、length_unit、iksolver） |
| `DeviceData.materials` | `<material name>` 命名材质库 |
| `DeviceData.links` | `<link>`（visual + collision） |
| `Joint` / `DofInfo` | `<joint>` + 多个 `<dof>` |
| `DeviceKind` | `<device kind>`（Scanner → Scanner，其余 → MotionDevice） |
| `Scanner::Camera/Projector` | `<camera>` / `<projector>` |
| `SceneObject` / `RigidObject` | `<obj type>` |
| `Workcell::name` | `<workcell name>` |

## 3. 功能点

### 3.1 功能索引

| # | 功能 | 行为 | 状态 |
| --- | --- | --- | --- |
| F1 | 设备 XML | `DeviceIO::loadXml/saveXml`，含 link/joint/DoF/材质/形状 | 已落地 |
| F2 | 工作站 XML | `WorkcellIO::loadXml/saveXml`，含 obj/children/parent_frame | 已落地 |
| F3 | 数据映射 | 模型 ↔ XML 字段一一对应 | 已落地 |
| F4 | VFS/打包 | `loadPkg/savePkg`，`.vdevpkg`/`.vwspkg`，包中包 | 已落地 |
| F5 | mesh 存储 | 原生 bin + XML 引用；共享去重 | 已落地 |
| F6 | BRep/STEP | `<brep>` 预留，本期不实现 | 未做 |

### 3.1.1 F1 设备 XML

- **行为**：`DeviceIO::loadXml(path|string|Vfs, dir)` / `saveXml(...)`；解析 metadata、materials、links、
  joints、DoF、camera/projector、parameters。
- **约束**（C1）：根 `<device>`；姿态四元数；长度按 metadata `length_unit` 缩放。
- **要求**（R1）：`DeviceKind` 决定工厂（Scanner/MotionDevice）；材质按名引用，不在 visual 里就地写。
- **验收**（T1）：`tests/test_robotics_io` 的设备 XML 用例。
- **注意**（N1）：关节/DoF 的 origin 是 `Frame::fixedTransform` / `DofInfo.origin`，不是同一个东西。

### 3.1.2 F2 工作站 XML

- **行为**：`WorkcellIO::loadXml/saveXml`；`<obj>` 可嵌套 `<children>`，`parent_frame` 挂到设备帧。
- **约束**（C2）：设备引用 `file` 可以是 `.vdev` 或 `.vdevpkg`；同一 `file` 可多实例，定义只写一次。
- **要求**（R2）：`home`/`q`/`resolutions` 是实例属性，不进设备定义。
- **验收**（T2）：`tests/test_robotics_io` 的工作站/套件用例。
- **注意**（N2）：`.vdevpkg` 是包中包，按扩展名分发。

### 3.1.3 F3 数据映射

- **行为**：`DeviceMetadata`、`DeviceData`、`Link`、`Joint`、`DofInfo`、`DeviceKind`、`Scanner`、
  `SceneObject`、`RigidObject`、`Workcell` 与 XML 字段一一对应。
- **约束**（C3）：映射表是契约；新增字段要有默认值/版本兼容。
- **要求**（R3）：四元数无损往返；`Q2Str` 用空格分隔。
- **验收**（T3）：往返用例逐字段比对。
- **注意**（N3）：`iksolver` 用 metadata 字符串（None/Pieper/Jacobian）。

### 3.1.4 F4 VFS/打包

- **行为**：`loadPkg(path)` / `savePkg(cell, Vfs&, options)`；包是 zip，mesh 是 bin。
- **约束**（C4）：不落临时目录；`ZipArchive` 惰性读、无源树 `saveAs`。
- **要求**（R4）：包中包从场景包读出字节再开 `ZipArchive`；共享 mesh 只写一次。
- **验收**（T4）：`PkgIOTest`（8 例）。
- **注意**（N4）：大包避免 `toBytes()`/`saveAs(ostream&)`（峰值整包 ×2）。

### 3.1.5 F5 mesh 存储

- **行为**：`indexed_triangle_mesh` 引用 `geoms/*.bin`（positions/normals/indices）；纯 XML 时为相对路径。
- **约束**（C5）：bin 是原生二进制（不是 base64）；`vertex_count`/`triangle_count` 与 bin 一致。
- **要求**（R5）：共享 mesh 去重；设备实例复用不重复写定义。
- **验收**（T5）：包内 mesh 往返用例。
- **注意**（N5）：bin 路径在包内是虚拟路径，不是磁盘路径。

### 3.1.6 F6 BRep/STEP

- **行为**：`<brep>` 预留；本期不实现。
- **约束**（C6）：出现 `<brep>` 时明确报不支持，不静默当空形状。
- **要求**（R6）：将来接入时保持 XML 形状兼容。
- **验收**（T6）：无（未实现）。
- **注意**（N6）：BRep 依赖 OCCT，属于后续阶段。

## 4. 公开接口

### 4.1 `DeviceIO`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `loadXml(path, LoadOptions)` | 从文件加载设备 | 失败抛 `runtime_error` |
| `loadXml(string, ...)` / `loadXml(Vfs&, dir, ...)` | 从字符串/VFS 加载 | 位置是 VFS + path |
| `saveXml(...)` | 导出设备 | 单位按 `SaveOptions` |
| `LoadOptions` / `SaveOptions` | `source_len_unit` / `target_len_unit` | 默认 mm ↔ m |

### 4.2 `WorkcellIO`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `loadXml(path|Vfs, ...)` | 加载工作站 | 设备引用 `.vdev`/`.vdevpkg` |
| `saveXml(...)` | 导出工作站 | 设备定义只写一次 |
| `loadPkg(path)` / `savePkg(cell, Vfs&, options)` | 打包 | zip，无临时目录 |

### 4.3 `XmlIOBase`

| 接口 | 契约 | 备注 |
| --- | --- | --- |
| `parsePose/exportPose` | 四元数姿态 | 无损 |
| `parseGeometry/exportGeometry` | 形状 | box/sphere/.../mesh |
| `parseMaterial` | 材质库条目 | 按名引用 |
| `parseLink/exportLink`、`parseJoint/exportJoint` | 连杆/关节 | 设备与工作站共用 |

## 5. 约束、要求与不变量

### 5.1 约束（C#）

| # | 约束 |
| --- | --- |
| C1 | 根 `<device>` / `<workcell>`；姿态四元数；长度按 metadata 缩放。 |
| C2 | 设备引用 `.vdev`/`.vdevpkg`；同一文件可多实例，定义只写一次。 |
| C3 | 数据映射表是契约；新增字段有默认值/版本兼容。 |
| C4 | 不落临时目录；包用 zip 惰性读/无源树 saveAs。 |
| C5 | mesh bin 是原生二进制；计数与 bin 一致。 |
| C6 | `<brep>` 未实现时明确报不支持。 |

### 5.2 要求（R#）

| # | 要求 |
| --- | --- |
| R1 | `DeviceKind` 决定工厂；材质按名引用。 |
| R2 | 实例属性不进设备定义。 |
| R3 | 四元数无损；`Q2Str` 空格分隔。 |
| R4 | 包中包按扩展名分发；共享 mesh 只写一次。 |
| R5 | 共享 mesh 去重；设备实例复用不重复写定义。 |
| R6 | 将来接 BRep 保持 XML 兼容。 |

### 5.3 不变量（N#）

| # | 不变量 |
| --- | --- |
| N1 | `XmlIOBase`/`DeviceIO`/`WorkcellIO` 无类级可变状态，可重入。 |
| N2 | 位置是 `Vfs + 虚拟路径`，不是裸 path。 |
| N3 | 设备定义只写一次；实例差异在 `<obj>` 属性。 |
| N4 | 加载失败抛 `runtime_error`；非致命问题进 `msgs`。 |

## 6. 注意事项与陷阱

### 6.1 关节 vs DoF 的 origin

- 会怎么错：把 `<joint><origin>` 当成 DoF 的 origin。
- 怎么判：joint origin = `Frame::fixedTransform`；dof origin = `DofInfo.origin`。
- 怎么防：XML 里两个 `<origin>` 分属不同层级。

### 6.2 单位缩放

- 会怎么错：文件 mm、内存 m 混用，模型大 1000 倍。
- 怎么判：metadata `length_unit` 与 `LoadOptions`/`SaveOptions` 不一致。
- 怎么防：加载/导出都显式给 source/target 单位；默认 mm ↔ m。

### 6.3 包中包

- 会怎么错：把 `.vdevpkg` 当普通 XML 解析。
- 怎么判：扩展名分发；场景包里读出字节再开嵌套 `ZipArchive`。
- 怎么防：按扩展名分发，不猜内容。

## 7. 已否决选项

| # | 选项 | 理由 | ADR |
| --- | --- | --- | --- |
| D1 | 用 URDF 格式 | `<robot>` 语义不够（多 DoF、扫描仪、材质库）。 | — |
| D2 | `IMemoryVfs` / `ZipMemoryVfs` 两套 VFS | 已由 `vn::io::Vfs` / `ZipArchive` 统一。 | — |
| D3 | HDF5/CBOR/Zarr | 生态/依赖/工具链不匹配；zip+XML 足够。 | — |
| D4 | 落临时目录再打包 | 慢、脏、并发问题；内存/VFS 直接打包。 | — |
| D5 | 就地材质（`exportMaterial`） | 材质应进命名库，visual 按名引用。 | — |
| D6 | base64 内嵌 mesh | 包内 bin 更小、可流式、可去重。 | — |

## 8. 测试与验收

| # | 判据 | 用例 / 命令 |
| --- | --- | --- |
| T1 | 设备 XML 往返 | `tests/test_robotics_io` |
| T2 | 工作站/嵌套 obj | 同上 |
| T3 | 字段映射/四元数 | 同上 |
| T4 | 包/包中包/mesh | `PkgIOTest`（8 例） |
| T5 | 共享 mesh 去重 | 同上 |
| T6 | BRep 未实现 | 报不支持 |

## 9. 状态与演进

### 9.1 当前状态

阶段一（VFS 基础设施）、阶段二（场景序列化）、阶段三（打包）均已落地；`tests/test_robotics_io` 全通过。

### 9.2 已知缺口 / 待办

- BRep/STEP 未实现（`<brep>` 预留）。
- mesh 只有三角网格 + 法线/索引；BRep 要等 OCCT 接入。
- 场景序列化前置的模型改动见 §6；WORKCELL 指针/帧查找仍在上下文中。

### 9.3 兼容性 / 迁移

旧 `IMemoryVfs`/`ZipMemoryVfs`/`ZipVfs::openZip` 已删除；新代码用 `vn::io::Vfs` + `ZipArchive`。
`XmlIOBase` 无状态，实例可复用。

## 10. 参考

### 10.1 代码

- `src/robotics/io/sdk/vine/robotics/io/XmlIOBase.hpp`
- `src/robotics/io/sdk/vine/robotics/io/DeviceIO.hpp`
- `src/robotics/io/sdk/vine/robotics/io/WorkcellIO.hpp`
- `src/robotics/io/src/IoUtils.hpp`
- `tests/test_robotics_io`

### 10.2 文档与 ADR

- `iobase-vfs-design.md`
- `robotics-proximity-design.md`
- `../backlog/`

### 10.3 外部资料

- VMR（外部仓库）的 `VDeviceIO` / `VWorkcellIO` / `VUrdfIOBase`。
- tinyxml2 10.0.0。
